# Tab5 Internet Radio

Internet radio firmware for the M5Stack Tab5 (ESP32-P4 + ESP32-C6), written in C++ on ESP-IDF v6.1.
UI direction: instrument panel (numbered modules, one orange accent). Mockup:
https://claude.ai/code/artifact/8ea29e09-ca1e-427d-acb4-8b179b471b47

## Status

- Display + touch via the official `m5stack_tab5` BSP, LVGL 9, landscape via the P4's PPA rotation.
- WiFi through the on-board C6 (esp_hosted over SDIO, esp_wifi_remote). Clock via SNTP (Costa Rica time).
- HTTP MP3/AAC stream → esp_audio_codec → ES8388 codec → speaker, with ICY "now playing" metadata;
  auto-reconnect on drop.
- Modules: 01 STATION (tap = next), 02 CONTROL (volume, mute), 03 SIGNAL (dBFS dot meter),
  04 TUNE (scrollable band, SEARCH / CR TOP / HOME keys), 05 PRESETS (tap = play, hold = store).
- Station search and the Costa Rica top list come from the Radio Browser directory over HTTPS.
- OUT selector (top right) cycles TAB5 → Sonos rooms reported by the Mac bridge; on a Sonos room the
  stream is handed to the room, the Tab5 shows the room's now-playing text, and the volume keys set the
  room volume.
- Last station, volume, mute, output and presets persist in NVS.

## Serial console

Lines typed on the USB serial console (115200) drive the same commands as the touch UI, handy for
testing without touching the screen:

```
status | next | tune N | preset N | store N | out N | vol N | mute | search TEXT | cr | home | result N
```

## Bridge endpoints used

`GET /health`, `GET /sonos/rooms`, `POST /sonos/play_url {room,url,title}`, `POST /sonos/cmd {room,action}`,
`POST /sonos/volume {room,volume}`, `GET /sonos/state?room=`. The bridge lives in the knob project
(`../Waveshare .../bridge`, `uv run server.py`); `BRIDGE_HOST` in `main/secrets.h` must match the Mac's IP.

## Repository, builds, releases

GitHub: https://github.com/cloudspores/tab5. Every push to `main` builds the firmware with Espressif's
v6.1 container (`.github/workflows/build.yml`); pushing a `v*` tag attaches the binaries to a GitHub
Release. `catalog.json` at the repo root is the launcher's catalogue of firmware and content packs
(served raw from GitHub). Two files are deliberately not in the repo: `main/secrets.h` (credentials,
built from the example in CI) and `main/c6_fw.bin` (M5Stack's C6 firmware; without it the C6 update
step is compiled out, which is fine on a board whose C6 is already updated).

## Layout

| Path | What |
|---|---|
| `main/main.cpp` | boot sequence only |
| `main/launcher.*`, `app.h` | home screen, app registry and switching |
| `main/theme.*`, `topbar.*` | instrument-panel palette/fonts/primitives and the shared top bar |
| `main/net.*` | C6 link, WiFi station, SNTP |
| `main/console.*` | USB serial command console with per-app verbs |
| `main/radio_app.*` | radio logic: control task, outputs, presets, search, console verbs |
| `main/radio_ui.*` | radio screen (LVGL) |
| `main/stream.*` | HTTP stream, ICY parsing, decode, playback, auto-reconnect |
| `main/stations.*` | built-in list, Radio Browser search, presets in NVS |
| `main/bridge.*` | client for the Mac bridge (Sonos rooms, handoff, volume, state) |
| `main/settings_app.*`, `update.*` | settings screen; firmware update from the GitHub catalogue |
| `main/c6_update.*` | one-time OTA of the C6 co-processor firmware over the hosted link |
| `main/fonts/` | Familjen Grotesk and JetBrains Mono converted for LVGL (OFL) |
| `main/secrets.h` | WiFi credentials and bridge address (gitignored; copy from `secrets.h.example`) |
| `design/` | design canvas sources for the radio and the synth/launcher |
| `catalog.json` | firmware/content catalogue read by the launcher |

This folder lives at `~/Projects/esp32/tab5-radio` and is linked from `M5Stack Tab5/radio`.
It must stay at a path without spaces: ESP-IDF and some components break on them.

## Build and flash

```
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

A USB reset does not power-cycle the display, so the firmware cycles the LCD and touch rails itself at
boot; if the screen ever stays dark, hold the power button for a real power cycle.

## Restore the stock firmware

```
esptool --port /dev/cu.usbmodem1101 write-flash 0 "../M5Stack Tab5/backup/tab5_stock_uiflow2_v2.5.1_2026-09-13.bin"
```

The C6 keeps hosted 2.12.0 after this project ran once; the stock UIFlow2 firmware works with it.

## Lessons that cost time

- `esp_audio_codec` 2.6+ needs ESP32-P4 chip revision 3.0; this board is v1.3, so the project pins `~2.5`.
- LVGL draw buffers in internal DMA RAM starve the SDIO transport ("mempool OOM", `dma_alloc failed`).
  Full-frame buffers go in PSRAM, as in M5Stack's own demo.
- Software rotation of a 1280x720 frame pegs a core; enable `CONFIG_LVGL_PORT_ENABLE_PPA`.
- The C6 shipped with esp-hosted 1.4.1; it is updated to 2.12.0 at first boot by `c6_update.cpp`.
- ESP-IDF's HTTP client exposes response headers only through the event callback.
