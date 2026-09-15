Vendored FM engine from https://github.com/google/music-synthesizer-for-android (Apache-2.0).
Only the synthesis core is included; Android glue, NEON assembly, tests and tools are omitted.

Local changes are marked `tab5:` in the sources:

- `synth_unit.h`: public accessors for the current voice, the bank and the live note count.
- `synth_unit.cc`: the resonant low-pass is bypassed (a per-sample 4x4 float state transition is too
  slow for real time on the ESP32-P4 and is not part of the DX7 signal path); notes that have decayed
  to silence after key-up are retired instead of staying "live" forever.
- `ringbuffer.cc`: `usleep` instead of `nanosleep`, which ESP-IDF's libc lacks.
- `env.cc`, `fm_core.cc`, `resofilter.cc`: signature and type fixes for a toolchain where `int32_t`
  is `long`.
