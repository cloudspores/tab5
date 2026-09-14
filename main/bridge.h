// Tiny client for the Mac bridge (FastAPI on BRIDGE_HOST:BRIDGE_PORT).
#pragma once
#include <string>
#include <vector>

namespace bridge {
void set_host(const char *host_or_ip);   // persisted; used after the next restart
bool reachable();
bool sonos_rooms(std::vector<std::string> &names);
bool sonos_play_url(const char *room, const char *url, const char *title);
bool sonos_stop(const char *room);
bool sonos_volume(const char *room, int volume);
bool sonos_now_playing(const char *room, std::string &title, std::string &artist, bool &playing);
}
