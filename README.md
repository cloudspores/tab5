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

## Launcher and apps

The firmware boots into the last-used screen. The home screen lists the apps as numbered cards;
tapping the product name in any top bar returns home. Apps implement the small `App` interface in
`main/app.h` (a screen, enter/exit hooks, a status line for the card). The radio keeps playing while
you are on another screen.

## Translate app

Live Spanish/English interpreter for a table conversation. The Tab5 streams its microphones
(16 kHz) to the bridge's `/translate/live` WebSocket; the bridge cuts phrases at pauses, transcribes
with language detection, translates toward the other language on a fast resident model, synthesizes
the reply, and sends text events plus 16 kHz PCM back. The screen shows everything in Spanish on the
top panel (FLIP turns it toward the person opposite) and everything in English on the bottom; VOICE
toggles spoken replies. The microphone is muted while a reply plays. Measured on the bench: about
0.7 s from the end of a phrase to the translation. Console: `open translate`, `listen on|off`, `flip`.

## Synth app

A six-operator FM synthesizer in the DX7 lineage, built on the vendored `msfa` engine (Google's
"music synthesizer for android", Apache-2.0) rendering 44.1 kHz on core 1. The Sound screen shows
the voice (name, index, algorithm, feedback), four macro dials that reshape the voice without the
DX7's 155 parameters (BRIGHT scales the modulators, ATTACK and RELEASE bend the envelopes, MOTION
adds vibrato and tremolo), RANDOM for a fresh voice, PANIC, an output meter and a two-octave touch
keyboard with octave shift. Thirty-two factory voices ship in `main/synth_bank.cpp`, written as
readable parameter tables; DX7 `.syx` banks (4104-byte bulk dumps) in `/tab5/synth/` on the microSD
card are loaded on entry. The radio releases the codec while the synth runs and resumes afterwards.
Console: `open synth`, `note N [off]`, `voice N`, `random`, `macro M V`, `panic`, `peak`, `dump`.

## Updates

Settings → FIRMWARE → CHECK reads `catalog.json` from this repository and offers INSTALL when it
lists a newer version. The image is downloaded from the GitHub Release named in the catalogue and
written to the spare OTA slot; the device restarts into it. To publish a release: bump `version.txt`,
update `catalog.json`, commit, and push a `vX.Y.Z` tag. CI builds and attaches the binaries.

WiFi credentials and the bridge host live in NVS (seeded from `secrets.h` by a local build, or set
with the `wifi` and `bridge` console verbs), so images built by CI without secrets keep working.

## Serial console

Type `help` on the USB serial console (115200). Verbs: `status next tune preset store out vol mute
play bright search cr home result off` for the radio; `home open check install wifi bridge` for the
system.

## Bridge

The device talks to the bridge in `bridge/` (Scala 3 + ZIO), which runs as a container on the DGX Spark
and speaks to Sonos over UPnP and to Ollama for the language model. See `bridge/README.md` for the
endpoints and `bridge/deploy.sh` to deploy. The device stores the bridge's Bonjour name in NVS
(console: `bridge spark-4dfb.local`).

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
| `main/translate_app.*` | live translation: microphone streaming, bridge events, spoken replies |
| `main/synth_engine.*` | FM engine wrapper: render task, MIDI ring, bank loading, voice access |
| `main/synth_app.*`, `synth_ui.*`, `synth_bank.*` | synth logic and macros, Sound screen, factory voices |
| `main/lv_mem_psram.cpp` | LVGL allocator backend that keeps widgets in PSRAM |
| `components/msfa/` | vendored FM synthesis core (Apache-2.0), see its README for local changes |
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
- Internal SRAM is the scarce resource. LVGL's small objects and the synth engine live in PSRAM
  (`lv_mem_psram.cpp`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`); before that the codec could not
  even allocate its I2S DMA descriptors when the synth was the boot screen.
- msfa's resonant filter costs a 4x4 float matrix per sample and stalls the render task; it is
  bypassed. The engine also has no `nanosleep`, and a `SynthUnit` only carries one built-in voice.
