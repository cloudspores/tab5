/**
 * @file synth_engine.cpp
 * @brief FM engine wrapper: msfa SynthUnit fed by MIDI bytes, rendered on core 1 to the codec.
 */
#include "synth_engine.h"
#include "stream.h"
#include "synth_bank.h"

#include <cstring>
#include <new>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "synth.h"        // msfa: defines N (render granularity); must precede the other engine headers
#include "synth_unit.h"
#include "ringbuffer.h"

static const char *TAG = "synth";

namespace {

RingBuffer *ring = nullptr;                ///< MIDI bytes into the engine (64 KB, lives in PSRAM)
SynthUnit  *unit = nullptr;
TaskHandle_t render_task_h = nullptr;
bool write_failed = false;                 ///< first codec write error already logged
volatile bool run = false;
bool spk_open = false;
int peak = 0;
uint32_t blocks_rendered = 0;     ///< diagnostics: render loop iterations
uint32_t midi_bytes_in = 0;       ///< diagnostics: bytes queued to the engine
uint8_t bank[4096];                        ///< packed copy of the loaded bank, for names
char names[synth::BANK_VOICES][11];
int current = 0;

constexpr int BLOCK = 256;                 ///< mono samples per render; 5.8 ms at 44.1 kHz

SemaphoreHandle_t midi_mtx = nullptr;      ///< the ring has one reader but several writers (UI, arp timer, console)

// Relay tap: a copy of the rendered stereo blocks for the Sonos relay task. One producer
// (the render task) and one consumer (the relay task). When the consumer falls behind (a
// WiFi stall) the oldest audio is discarded: the relay skips rather than lagging further,
// since anything queued here becomes permanent latency at the Sonos end.
constexpr int TAP_BYTES = 64 * 1024;       ///< ~370 ms of 44.1 kHz stereo
uint8_t *tap = nullptr;
volatile int tap_head = 0, tap_tail = 0;
volatile bool tap_on = false;
bool local_muted = false;

void tap_write(const uint8_t *b, int n)
{
    int head = tap_head, tail = tap_tail;
    int space = (tail - head - 1 + TAP_BYTES) % TAP_BYTES;
    if (n > space) tap_tail = (tail + (n - space)) % TAP_BYTES;    // consumer is behind: skip the oldest bytes
    int first = TAP_BYTES - head; if (first > n) first = n;
    memcpy(tap + head, b, first);
    if (n > first) memcpy(tap, b + first, n - first);
    tap_head = (head + n) % TAP_BYTES;
}

/** Queue a MIDI message for the render task; messages from different tasks never interleave. */
void midi(const uint8_t *b, int n)
{
    if (!ring) return;
    xSemaphoreTake(midi_mtx, portMAX_DELAY);
    ring->Write(b, n); midi_bytes_in += n;
    xSemaphoreGive(midi_mtx);
}

/** Render loop: mono int16 from the engine, duplicated to stereo, written to the codec. */
void render_task(void *)
{
    auto spk = (esp_codec_dev_handle_t)stream::speaker();
    static int16_t mono[BLOCK];
    static int16_t stereo[BLOCK * 2];
    while (run) {
        unit->GetSamples(BLOCK, mono);
        int pk = 0;
        for (int i = 0; i < BLOCK; i++) {
            int v = mono[i] < 0 ? -mono[i] : mono[i];
            if (v > pk) pk = v;
            stereo[2 * i] = mono[i]; stereo[2 * i + 1] = mono[i];
        }
        peak = pk;
        blocks_rendered++;
        if (tap_on) tap_write((const uint8_t *)stereo, sizeof stereo);
        // A successful write blocks on the I2S DMA queue and paces the loop. If the
        // codec is not writable (closed underneath us, or a driver error) we must not
        // spin: yield for one block's worth of time so LVGL and the idle task on this
        // core keep running, and report the first failure.
        int r = esp_codec_dev_write(spk, stereo, sizeof stereo);
        if (r != ESP_CODEC_DEV_OK) {
            if (!write_failed) { ESP_LOGE(TAG, "codec write failed (%d)", r); write_failed = true; }
            vTaskDelay(pdMS_TO_TICKS(6));
        } else {
            write_failed = false;
        }
    }
    render_task_h = nullptr;
    vTaskDelete(nullptr);
}

} // namespace

namespace synth {

bool start()
{
    if (run) return true;
    static bool tables = false;
    if (!tables) { SynthUnit::Init(SAMPLE_RATE); tables = true; }
    // The 64 KB MIDI ring and the 13 KB SynthUnit live in PSRAM: internal SRAM is
    // reserved for DMA, task stacks and the network stack. Both are created once.
    if (!midi_mtx) midi_mtx = xSemaphoreCreateMutex();
    if (!tap) tap = (uint8_t *)heap_caps_malloc(TAP_BYTES, MALLOC_CAP_SPIRAM);
    if (!ring) ring = new (heap_caps_malloc(sizeof(RingBuffer), MALLOC_CAP_SPIRAM)) RingBuffer();
    if (!unit) {
        unit = new (heap_caps_malloc(sizeof(SynthUnit), MALLOC_CAP_SPIRAM)) SynthUnit(ring);
        // The engine's own bank holds a single voice followed by uninitialised memory,
        // so install the factory bank straight away (queued to the ring, applied by the
        // render task on its first block).
        load_bank(synth_bank::factory(), synth_bank::BANK_BYTES);
    }
    auto spk = (esp_codec_dev_handle_t)stream::speaker();
    if (!spk) { ESP_LOGE(TAG, "no speaker"); return false; }
    // Opening the codec allocates I2S DMA descriptors; esp_codec_dev does not survive
    // that allocation failing, so refuse to start when DMA-capable memory is scarce.
    size_t dma_free = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (dma_free < 8 * 1024) { ESP_LOGE(TAG, "not enough DMA memory to open the codec (%u bytes)", (unsigned)dma_free); return false; }
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16; fs.channel = 2;
    fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
    fs.sample_rate = SAMPLE_RATE;
    if (esp_codec_dev_open(spk, &fs) != 0) { ESP_LOGE(TAG, "speaker open failed"); return false; }
    esp_codec_dev_set_out_vol(spk, stream::volume_percent());
    // The codec remembers the radio's mute across close/open; the synth always plays audibly
    // (the radio re-applies its own mute when it reopens the codec).
    esp_codec_dev_set_out_mute(spk, local_muted);
    spk_open = true;
    run = true;
    xTaskCreatePinnedToCore(render_task, "fm_render", 12 * 1024, nullptr, 20, &render_task_h, 1);
    ESP_LOGI(TAG, "engine running, %d Hz, block %d", SAMPLE_RATE, BLOCK);
    return true;
}

void stop()
{
    if (!run) return;
    all_notes_off();
    run = false;
    for (int i = 0; i < 50 && render_task_h; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (spk_open) { esp_codec_dev_close((esp_codec_dev_handle_t)stream::speaker()); spk_open = false; }
    ESP_LOGI(TAG, "engine stopped");
}

bool running() { return run; }

void set_relay(bool on) { if (on && !tap_on) { tap_head = 0; tap_tail = 0; } tap_on = on; }
void relay_flush() { tap_tail = tap_head; }
bool relay() { return tap_on; }
int  relay_read(uint8_t *out, int max)
{
    int head = tap_head, tail = tap_tail;
    int avail = (head - tail + TAP_BYTES) % TAP_BYTES;
    int n = avail < max ? avail : max;
    int first = TAP_BYTES - tail; if (first > n) first = n;
    memcpy(out, tap + tail, first);
    if (n > first) memcpy(out + first, tap, n - first);
    tap_tail = (tail + n) % TAP_BYTES;
    return n;
}
void set_local_mute(bool on)
{
    local_muted = on;
    if (spk_open) esp_codec_dev_set_out_mute((esp_codec_dev_handle_t)stream::speaker(), on);
}

void note_on(int note, int vel) { uint8_t m[3] = {0x90, (uint8_t)(note & 0x7f), (uint8_t)(vel & 0x7f)}; midi(m, 3); }
void note_off(int note)         { uint8_t m[3] = {0x80, (uint8_t)(note & 0x7f), 0}; midi(m, 3); }
void controller(int cc, int v)  { uint8_t m[3] = {0xb0, (uint8_t)(cc & 0x7f), (uint8_t)(v & 0x7f)}; midi(m, 3); }
void all_notes_off()            { for (int n = 0; n < 128; n++) note_off(n); }

bool load_bank(const uint8_t *packed, size_t len)
{
    if (len != 4096) { ESP_LOGE(TAG, "bank must be 4096 bytes (got %u)", (unsigned)len); return false; }
    memcpy(bank, packed, 4096);
    for (int i = 0; i < BANK_VOICES; i++) { memcpy(names[i], bank + 128 * i + 118, 10); names[i][10] = 0; }
    // Deliver as the bulk-dump sysex the engine understands: F0 43 00 09 20 00 <4096> <sum> F7
    static uint8_t msg[4104];
    msg[0] = 0xf0; msg[1] = 0x43; msg[2] = 0x00; msg[3] = 0x09; msg[4] = 0x20; msg[5] = 0x00;
    memcpy(msg + 6, packed, 4096);
    int sum = 0; for (int i = 0; i < 4096; i++) sum += packed[i];
    msg[4102] = (uint8_t)((-sum) & 0x7f); msg[4103] = 0xf7;
    midi(msg, sizeof msg);
    return true;
}

void select_voice(int i) { current = i < 0 ? 0 : i >= BANK_VOICES ? BANK_VOICES - 1 : i; uint8_t m[2] = {0xc0, (uint8_t)current}; midi(m, 2); }
int  voice_count() { return BANK_VOICES; }
const char *voice_name(int i)
{
    static char t[11];
    strlcpy(t, names[i < 0 ? 0 : i >= BANK_VOICES ? BANK_VOICES - 1 : i], sizeof t);
    for (int k = 9; k >= 0 && (t[k] == ' ' || t[k] == 0); k--) t[k] = 0;
    return t;
}

int algorithm() { char v[156]; unit->GetUnpacked(v); return v[134] + 1; }
int feedback()  { char v[156]; unit->GetUnpacked(v); return v[135]; }
void get_voice(uint8_t out[VOICE_PARAMS]) { char v[156]; unit->GetUnpacked(v); memcpy(out, v, VOICE_PARAMS); }
void set_voice(const uint8_t in[VOICE_PARAMS]) { char v[156]; memcpy(v, in, VOICE_PARAMS); v[155] = 0; unit->SetUnpacked(v); }
int  last_peak() { return peak; }
void debug_stats(uint32_t &blocks, uint32_t &midi_bytes, int &pending) { blocks = blocks_rendered; midi_bytes = midi_bytes_in; pending = ring ? ring->BytesAvailable() : -1; }
int  active_voices() { return unit ? unit->LiveNotes() : 0; }

}
