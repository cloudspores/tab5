// Firmware update from the GitHub catalogue (catalog.json) via esp_https_ota.
#pragma once
#include <cstddef>
namespace update {
const char *running_version();
// Fetch the catalogue; returns true and fills version/url when a newer firmware is listed.
bool check(char *version, size_t vcap, char *url, size_t ucap, char *notes, size_t ncap);
// Download and install; restarts on success. Calls progress(percent) as it goes.
bool install(const char *url, void (*progress)(int percent));
}
