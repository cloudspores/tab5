/**
 * @file console.cpp
 * @brief USB serial command console: a small verb table and a polling reader task.
 */
#include "console.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "console";

namespace {

struct Cmd { const char *verb; console::Handler h; const char *help; };
Cmd cmds[32]; int ncmds = 0;

void help(const char *, int)
{
    for (int i = 0; i < ncmds; i++) ESP_LOGI(TAG, "  %-10s %s", cmds[i].verb, cmds[i].help);
}

/** Read lines from stdin and dispatch. Handlers must be quick; long work belongs in a task. */
void task(void *)
{
    char line[96]; int n = 0;
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (ch == '\r') continue;
        if (ch != '\n') { if (n < (int)sizeof line - 1) line[n++] = (char)ch; continue; }
        line[n] = 0; n = 0;
        if (!line[0]) continue;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        bool found = false;
        for (int i = 0; i < ncmds; i++) {
            if (strcmp(cmds[i].verb, line)) continue;
            cmds[i].h(arg ? arg : "", arg ? atoi(arg) : 0);
            found = true;
            break;
        }
        if (!found) ESP_LOGW(TAG, "unknown command '%s' (try: help)", line);
    }
}

} // namespace

namespace console {

void add(const char *verb, Handler h, const char *helptext)
{
    for (int i = 0; i < ncmds; i++) if (!strcmp(cmds[i].verb, verb)) { cmds[i] = {verb, h, helptext}; return; }
    if (ncmds < 32) cmds[ncmds++] = {verb, h, helptext};
}

void start()
{
    add("help", help, "list commands");
    xTaskCreatePinnedToCore(task, "console", 12 * 1024, nullptr, 3, nullptr, 0);   // verbs run app enter/exit hooks (SD mount, LVGL)
}

}
