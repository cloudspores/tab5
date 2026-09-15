package tab5.bridge

import zio.*
import zio.http.*
import zio.json.*

/** Request bodies. Field names match the Python bridge so the Tab5 and knobs need no changes. */
final case class PlayUrl(room: String, url: String, title: String = "") derives JsonDecoder
final case class Cmd(room: String = "all", action: String) derives JsonDecoder
final case class Volume(room: String, volume: Option[Int] = None, delta: Option[Int] = None) derives JsonDecoder
final case class Ask(question: String, system: Option[String] = None) derives JsonDecoder
final case class Translate(text: String, from: String = "auto", to: String = "en") derives JsonDecoder
final case class Speak(text: String, language: String = "en") derives JsonDecoder
final case class RelayTo(room: String) derives JsonDecoder
final case class Transcript(text: String) derives JsonEncoder

final case class Result(result: String) derives JsonEncoder
final case class Answer(answer: String) derives JsonEncoder
final case class Translation(text: String, from: String, to: String) derives JsonEncoder
final case class Health(ok: Boolean, service: String, version: String, model: String) derives JsonEncoder

/** HTTP surface of the bridge. */
object Api:

  /** Decode a JSON body or answer 400. */
  private def body[A: JsonDecoder](req: Request): IO[Response, A] =
    req.body.asString.orElseFail(Response.badRequest("unreadable body")).flatMap { s =>
      ZIO.fromEither(s.fromJson[A]).mapError(e => Response.badRequest(s"bad request: $e"))
    }

  /** Turn a failed effect into a 502 with the message, so the device sees why. */
  private def orError[R, A](z: RIO[R, A]): ZIO[R, Response, A] =
    z.mapError(e => Response.error(Status.BadGateway, Option(e.getMessage).getOrElse(e.toString)))

  /** Live translation over a WebSocket; see Live for the protocol. */
  private def liveSocket(cfg: BridgeConfig): WebSocketApp[Sonos & Ollama & Speech] =
    Handler.webSocket { channel =>
      for
        speech <- ZIO.service[Speech]
        ollama <- ZIO.service[Ollama]
        in     <- Queue.bounded[Live.In](256)
        out    <- Queue.bounded[Live.Out](64)
        sender <- out.take.flatMap {
                    case Live.Out.Text(json) => channel.send(ChannelEvent.Read(WebSocketFrame.text(json)))
                    case Live.Out.Audio(pcm) => channel.send(ChannelEvent.Read(WebSocketFrame.binary(pcm)))
                  }.forever.fork
        session <- Live.run(in, out, speech, ollama, cfg.translator.model, cfg.translator.live).fork
        _      <- channel.receiveAll {
                    case ChannelEvent.Read(WebSocketFrame.Binary(bytes)) => in.offer(Live.In.Pcm(bytes)).unit
                    case ChannelEvent.Read(WebSocketFrame.Text(text))    => in.offer(Live.In.Control(text)).unit
                    case ChannelEvent.Read(WebSocketFrame.Close(_, _))   => in.offer(Live.In.Closed).unit
                    case ChannelEvent.Unregistered                        => in.offer(Live.In.Closed).unit
                    case _                                                => ZIO.unit
                  }.ensuring(in.offer(Live.In.Closed) *> session.join.ignore *> sender.interrupt)
      yield ()
    }

  /** The Tab5 synth's audio arrives here as binary PCM frames; see SynthRelay. */
  private val synthSocket: WebSocketApp[SynthRelay] =
    Handler.webSocket { channel =>
      channel.receiveAll {
        case ChannelEvent.Read(WebSocketFrame.Binary(bytes)) => SynthRelay.push(bytes)
        case _                                                => ZIO.unit
      }
    }

  def routes(cfg: BridgeConfig): Routes[Sonos & Ollama & Speech & SynthRelay, Response] = Routes(
    Method.GET / "translate" / "live" -> handler(liveSocket(cfg).toResponse),

    // synth relay: device audio in, MP3 out, and a helper that points a room at the stream
    Method.GET / "synth" / "in" -> handler(synthSocket.toResponse),
    Method.GET / "synth" / "stream.mp3" -> handler {
      SynthRelay.stream.map(s => Response(body = Body.fromStreamChunked(s), headers = Headers(Header.ContentType(MediaType.audio.mpeg), Header.CacheControl.NoCache)))
    },
    // development aid: the device uploads a raw RGB565 screenshot, saved for the developer to fetch
    Method.POST / "shot" -> handler { (req: Request) =>
      val w = req.url.queryParams.queryParam("w").getOrElse("0"); val h = req.url.queryParams.queryParam("h").getOrElse("0")
      for
        bytes <- req.body.asChunk.orElseFail(Response.badRequest("unreadable body"))
        path   = s"/tmp/tab5-shot-${w}x${h}.rgb565"
        _     <- ZIO.attemptBlocking(java.nio.file.Files.write(java.nio.file.Paths.get(path), bytes.toArray)).orElseFail(Response.internalServerError("write failed"))
        _     <- ZIO.logInfo(s"screenshot ${bytes.length} bytes -> $path")
      yield Response.json(Result(path).toJson)
    },
    Method.POST / "synth" / "sonos" -> handler { (req: Request) =>
      for
        r   <- body[RelayTo](req)
        url <- SynthRelay.streamUrl
        msg <- orError(Sonos.playUrl(r.room, url, "Tab5 Synth"))
      yield Response.json(Result(msg).toJson)
    },

    Method.GET / "health" -> handler {
      Response.json(Health(true, "tab5-bridge", BridgeVersion.current, cfg.ollama.model).toJson)
    },

    // ---- Sonos: same contract as the Python bridge ----
    Method.GET / "sonos" / "rooms" -> handler {
      orError(Sonos.rooms).map(rs => Response.json(rs.toJson))
    },
    Method.GET / "sonos" / "state" -> handler { (req: Request) =>
      val room = req.url.queryParams.queryParam("room").getOrElse("")
      orError(Sonos.state(room)).map(r => Response.json(r.toJson))
    },
    Method.POST / "sonos" / "play_url" -> handler { (req: Request) =>
      body[PlayUrl](req).flatMap(p => orError(Sonos.playUrl(p.room, p.url, p.title))).map(m => Response.json(Result(m).toJson))
    },
    Method.POST / "sonos" / "cmd" -> handler { (req: Request) =>
      body[Cmd](req).flatMap(c => orError(Sonos.command(c.room, c.action))).map(m => Response.json(Result(m).toJson))
    },
    Method.POST / "sonos" / "volume" -> handler { (req: Request) =>
      body[Volume](req).flatMap { v =>
        val target = v.volume.orElse(v.delta.map(d => 50 + d))   // delta without a current reading: best effort
        orError(target match
          case Some(t) => Sonos.setVolume(v.room, t)
          case None    => ZIO.fail(new IllegalArgumentException("volume or delta required")))
      }.map(m => Response.json(Result(m).toJson))
    },

    // ---- Language model on the Spark ----
    Method.POST / "ask" -> handler { (req: Request) =>
      body[Ask](req).flatMap { a =>
        val system = a.system.getOrElse("You are a concise assistant on a small kitchen display. Answer in at most three short sentences.")
        orError(Ollama.chat(system, a.question))
      }.map(ans => Response.json(Answer(ans).toJson))
    },
    // ---- Speech on the Spark ----
    // Body: raw 16 kHz mono 16-bit PCM. Query: ?language=es|en|auto
    Method.POST / "transcribe" -> handler { (req: Request) =>
      val lang = req.url.queryParams.queryParam("language").getOrElse("auto")
      req.body.asChunk.orElseFail(Response.badRequest("unreadable body")).flatMap { pcm =>
        orError(Speech.transcribe(pcm, lang))
      }.map(t => Response.json(Transcript(t).toJson))
    },
    // Body: {text, language}. Response: audio/wav
    Method.POST / "speak" -> handler { (req: Request) =>
      body[Speak](req).flatMap(s => orError(Speech.speak(s.text, s.language))).map { wav =>
        Response(status = Status.Ok, headers = Headers(Header.ContentType(MediaType.audio.wav)), body = Body.fromChunk(wav))
      }
    },

    Method.POST / "translate" -> handler { (req: Request) =>
      body[Translate](req).flatMap { t =>
        val system =
          s"""You translate between Spanish and English for a conversation in Costa Rica. Source language: ${t.from} (detect it if "auto"). Target: ${t.to}.
             |Keep the register (usted/vos) natural for Costa Rica. Reply with the translation only, no commentary.""".stripMargin
        orError(Ollama.chat(system, t.text)).map(out => Translation(out, t.from, t.to))
      }.map(tr => Response.json(tr.toJson))
    }
  )
