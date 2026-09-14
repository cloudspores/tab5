#include "settings.h"
#include <cstring>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static nvs_handle_t h = 0;

namespace settings {

void init()
{
    if (nvs_open("radio", NVS_READWRITE, &h) != ESP_OK) ESP_LOGE(TAG, "nvs open failed");
}

int get_int(const char *key, int def)
{
    int32_t v = def;
    if (h && nvs_get_i32(h, key, &v) != ESP_OK) return def;
    return v;
}

void set_int(const char *key, int v)
{
    if (!h) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
}

bool get_str(const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (!h || nvs_get_str(h, key, out, &len) != ESP_OK) { out[0] = 0; return false; }
    return true;
}

void set_str(const char *key, const char *v)
{
    if (!h) return;
    nvs_set_str(h, key, v);
    nvs_commit(h);
}

bool get_blob(const char *key, void *out, size_t len)
{
    size_t l = len;
    return h && nvs_get_blob(h, key, out, &l) == ESP_OK && l == len;
}

void set_blob(const char *key, const void *v, size_t len)
{
    if (!h) return;
    nvs_set_blob(h, key, v, len);
    nvs_commit(h);
}

} // namespace settings
