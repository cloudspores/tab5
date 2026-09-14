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

## Next

Speech-to-text and text-to-speech endpoints backed by containers on the Spark (Whisper, a TTS
server), the Spotify profile pull, and a vision endpoint for the Tab5's camera.
