/**
 * @file synth_relay.cpp
 * @brief Worker task that streams the engine's audio tap to the bridge and manages the output choice.
 *
 * State lives in `wanted` (the output the user chose) and `active` (what the worker has set up).
 * The worker reconciles the two: switching to a room means asking the bridge to point the room
 * at its stream, muting the local speaker, opening the WebSocket and pumping PCM; switching back
 * stops the room, closes the socket and unmutes. Bridge calls take up to a second, which is why
 * they happen here and not in the LVGL task.
 */
#include "synth_relay.h"
#include "synth_engine.h"
#include "bridge.h"
#include "net.h"
#include "settings.h"

#include <cstring>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_websocket_client.h"
#include "esp_log.h"

static const char *TAG = "relay";

namespace synth_relay {
namespace {

constexpr int CHUNK = 8192;                          ///< bytes per WebSocket frame: 46 ms of stereo

SemaphoreHandle_t mtx;
std::vector<std::string> outs = {LOCAL};
std::string wanted = LOCAL, active = LOCAL;
bool running = false, discover_wanted = false;
int  vol_wanted = -1;                                ///< room volume to apply, -1 = nothing pending
volatile int changes = 0;                            ///< bumped when outputs or the selection change
char state[48] = "TAB5 HP";
TaskHandle_t worker_h = nullptr;
esp_websocket_client_handle_t ws = nullptr;
bool connected = false;

void set_state(const char *t) { strlcpy(state, t, sizeof state); }

void ws_event(void *, esp_event_base_t, int32_t id, void *)
{
    if (id == WEBSOCKET_EVENT_CONNECTED) { synth::relay_flush(); connected = true; ESP_LOGI(TAG, "stream connected"); }
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) connected = false;
}

void open_socket()
{
    if (ws) return;
    static char uri[160];
    snprintf(uri, sizeof uri, "ws://%s:%d/synth/in", bridge::resolved_host(), bridge::port());
    esp_websocket_client_config_t cfg = {};
    cfg.uri = uri;
    cfg.buffer_size = CHUNK + 256;
    cfg.reconnect_timeout_ms = 3000;
    cfg.network_timeout_ms = 5000;
    ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, nullptr);
    esp_websocket_client_start(ws);
}

void close_socket()
{
    if (!ws) return;
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    ws = nullptr; connected = false;
}

/** Bring `active` in line with `wanted`. Runs on the worker. */
void reconcile()
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    std::string target = wanted, previous = active;
    xSemaphoreGive(mtx);
    if (target == previous) return;
    if (previous != LOCAL) {                                   // leaving a room: silence it first
        synth::set_relay(false);
        close_socket();
        bridge::sonos_stop(previous.c_str());
    }
    if (target == LOCAL) {
        synth::set_local_mute(false);
        set_state("TAB5 HP");
    } else {
        if (!net::connected()) { set_state("WAITING FOR WIFI"); return; }   // stay pending; retried every loop
        set_state("CONNECTING");
        if (!bridge::synth_relay(target.c_str())) {
            ESP_LOGW(TAG, "could not start the relay to %s", target.c_str());
            set_state("BRIDGE FAILED");
            xSemaphoreTake(mtx, portMAX_DELAY); wanted = LOCAL; xSemaphoreGive(mtx);
            synth::set_local_mute(false);
            return;
        }
        synth::set_local_mute(true);
        synth::set_relay(true);
        open_socket();
        char t[48]; snprintf(t, sizeof t, "SONOS %s", target.c_str());
        for (char *c = t; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        set_state(t);
    }
    xSemaphoreTake(mtx, portMAX_DELAY); active = target; xSemaphoreGive(mtx);
    changes = changes + 1;
}

void worker(void *)
{
    static uint8_t buf[CHUNK];
    while (running) {
        if (discover_wanted && net::connected()) {
            discover_wanted = false;
            std::vector<std::string> rooms;
            if (bridge::sonos_rooms(rooms)) {
                xSemaphoreTake(mtx, portMAX_DELAY);
                outs.assign(1, LOCAL);
                for (auto &r : rooms) outs.push_back(r);
                xSemaphoreGive(mtx);
                ESP_LOGI(TAG, "%d outputs", (int)outs.size());
                changes = changes + 1;
            }
        }
        reconcile();
        if (vol_wanted >= 0 && active != LOCAL) { int v = vol_wanted; vol_wanted = -1; bridge::sonos_volume(active.c_str(), v); }
        if (synth::relay() && ws && connected) {
            int n = synth::relay_read(buf, sizeof buf);
            if (n >= CHUNK / 2) { esp_websocket_client_send_bin(ws, (const char *)buf, n, pdMS_TO_TICKS(1500)); continue; }   // a WiFi stall must not close the socket
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    close_socket();
    synth::set_relay(false);
    worker_h = nullptr;
    vTaskDelete(nullptr);
}
} // namespace

void start()
{
    if (!mtx) mtx = xSemaphoreCreateMutex();
    char saved[48] = "";
    if (settings::get_str("synth_out", saved, sizeof saved) && saved[0]) wanted = saved;
    if (running) return;
    running = true;
    xTaskCreatePinnedToCore(worker, "synth_relay", 8 * 1024, nullptr, 7, &worker_h, 0);
}

void stop()
{
    if (!running) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    std::string room = active;
    wanted = LOCAL;
    xSemaphoreGive(mtx);
    running = false;
    for (int i = 0; i < 100 && worker_h; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (room != LOCAL) bridge::sonos_stop(room.c_str());
    active = LOCAL;
    synth::set_local_mute(false);
    set_state("TAB5 HP");
}

void discover() { discover_wanted = true; }

const std::vector<std::string> &outputs() { return outs; }

void select(const char *name)
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    wanted = name;
    xSemaphoreGive(mtx);
    settings::set_str("synth_out", name);
    changes = changes + 1;
}

void set_volume(int percent) { vol_wanted = percent; }
int  changed_count() { return changes; }

void next()
{
    xSemaphoreTake(mtx, portMAX_DELAY);
    int idx = 0;
    for (size_t i = 0; i < outs.size(); i++) if (outs[i] == wanted) idx = (int)i;
    std::string n = outs[(idx + 1) % outs.size()];
    xSemaphoreGive(mtx);
    select(n.c_str());
}

const char *current() { return wanted.c_str(); }
const char *status() { return state; }

}
