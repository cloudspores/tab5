// Station model, built-in list, Radio Browser search, presets.
#pragma once
#include <vector>
#include <cstddef>

namespace stations {

struct Station {
    char name[64];
    char url[192];
    char codec[8];      // "MP3", "AAC", ...
    int  bitrate;       // kbps, 0 if unknown
    char country[3];    // ISO code
};

constexpr int PRESET_SLOTS = 6;

const std::vector<Station> &builtin();

// Blocking HTTPS query against the Radio Browser directory. Either argument may be empty.
// Returns false on transport failure; `out` is replaced.
bool search(const char *name, const char *countrycode, std::vector<Station> &out, int limit = 12);

// Presets live in NVS. An empty slot has name[0] == 0.
const Station &preset(int slot);
void set_preset(int slot, const Station &s);
void load_presets();

// Last played station, for resume on boot.
bool load_last(Station &s);
void save_last(const Station &s);

}
