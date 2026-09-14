// HTTP radio stream player: ICY metadata parsing, MP3/AAC decode, speaker output.
#pragma once

namespace stream {
struct Station {
    const char *name;
    const char *url;
};
void init();                       // codec + decoder setup; call once, before play
void play(const Station &s);       // start (or switch to) a station; non-blocking
void stop();
bool playing();
void set_mute(bool on);
void set_volume(int percent);   // 0..100
bool muted();
}
