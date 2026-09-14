# Tab5 bridge (Scala 3 + ZIO)

The LAN service the Tab5 and the Waveshare knobs talk to. It runs on the DGX Spark next to Ollama
and speaks to the Sonos players over UPnP, so nothing on the devices needs a Python runtime or a
cloud account.

| Endpoint | Purpose |
|---|---|
| `GET /health` | liveness, version, configured model |
| `GET /sonos/rooms` | every visible player with state, volume and now-playing text |
| `GET /sonos/state?room=` | one player |
| `POST /sonos/play_url {room,url,title}` | play an internet radio stream on a room |
| `POST /sonos/cmd {room,action}` | play, pause, stop, next, mute, unmute |
| `POST /sonos/volume {room,volume}` | set volume 0-100 |
| `POST /ask {question,system?}` | chat with the model on the Spark |
| `POST /translate {text,from,to}` | translation tuned for Costa Rican Spanish |
| `POST /transcribe?language=en|es|auto` (body: raw 16 kHz mono 16-bit PCM) | speech to text via whisper.cpp on the GPU |
| `POST /speak {text,language}` | text to speech via Piper; returns `audio/wav` (22.05 kHz mono) |

The Sonos routes keep the Python bridge's contract, so the Tab5 firmware and the knobs are unchanged.

## Layout

| File | Role |
|---|---|
| `Main.scala` | wiring: config, HTTP client, services, server |
| `Config.scala` | `application.conf` with environment overrides (`TAB5_PORT`, `OLLAMA_URL`, `OLLAMA_MODEL`, `SONOS_SCAN_SUBNET`) |
| `Api.scala` | routes and JSON request/response types |
| `Sonos.scala` | SSDP discovery with subnet-scan fallback, household topology, AVTransport / RenderingControl actions |
| `Soap.scala` | UPnP SOAP envelopes, response parsing, radio metadata |
| `Ollama.scala` | Ollama `/api/chat` client |
| `Speech.scala` | whisper.cpp server client (multipart, content-length), Piper process runner, WAV header |

## Develop

```
sbt compile test        # -Xfatal-warnings: warnings are errors
sbt run                 # serves on :8765, Ollama at localhost:11434 unless OLLAMA_URL is set
TAB5_PORT=8766 OLLAMA_URL=http://spark-4dfb.local:11434 sbt run
```

## Deploy to the Spark

The Spark has Docker and Java 8 only, so the bridge runs in a Temurin 21 container with host
networking (SSDP multicast needs it). `./deploy.sh` builds the JAR, copies it with the Dockerfile
and compose file, and restarts the container. The Tab5 finds it as `spark-4dfb.local`.

## Speech services on the Spark

- **whisper.cpp** built with CUDA in `~/speech/whisper.cpp` (`cmake -B build -DGGML_CUDA=1`, target
  `whisper-server`), model `ggml-large-v3-turbo.bin`. Runs as the systemd user unit in
  `whisper-server.service` on 127.0.0.1:8178; `loginctl enable-linger` keeps it up without a login.
  Transcribes a five-second clip in about 0.3 s.
- **Piper** (`~/speech/piper`, the aarch64 release) with voices `en_US-lessac-medium` and
  `es_MX-claude-high`, mounted read-only into the bridge container at `/opt/piper`.

## Next

Spotify profile pull, a vision endpoint for the Tab5's camera, and the Tab5 firmware side of
translation (push-to-talk, transcript, spoken reply).
