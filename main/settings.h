// Persistent settings in NVS.
#pragma once
#include <cstdint>
#include <cstddef>

namespace settings {
void init();
int  get_int(const char *key, int def);
void set_int(const char *key, int v);
bool get_str(const char *key, char *out, size_t cap);
void set_str(const char *key, const char *v);
bool get_blob(const char *key, void *out, size_t len);
void set_blob(const char *key, const void *v, size_t len);
}
