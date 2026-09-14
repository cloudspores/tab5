package tab5.bridge

import zio.*
import zio.json.*

/**
 * One live translation session (one device).
 *
 * Audio frames come in; the VAD cuts them into phrases at pauses. Each phrase is transcribed with
 * language detection, translated toward the other language of the pair, synthesized, and the
 * results are pushed back as JSON events plus one binary PCM frame per translation. While a phrase
 * is still being spoken, partial transcripts are sent so the screen moves before the pause.
 *
 * Wire protocol (WebSocket):
 *   client -> server  binary: 16 kHz mono 16-bit PCM, any chunk size
 *                     text:   {"type":"config","a":"es","b":"en","speak":true}
 *   server -> client  text:   {"type":"partial","text":...}
 *                             {"type":"segment","lang":"es","text":...}
 *                             {"type":"translation","lang":"en","text":...}
 *                             {"type":"audio","rate":16000,"bytes":N}   followed by one binary frame of PCM
 *                             {"type":"status","text":...}
 */
object Live:

  final case class Config(a: String = "es", b: String = "en", speak: Boolean = true) derives JsonCodec

  /** Outgoing message: text JSON or binary audio. */
  enum Out:
    case Text(json: String)
    case Audio(pcm16k: Chunk[Byte])

  /** Incoming message from the device. */
  enum In:
    case Pcm(bytes: Chunk[Byte])
    case Control(json: String)
    case Closed

  private final case class Partial(text: String) derives JsonEncoder
  private final case class Segment(lang: String, text: String) derives JsonEncoder
  private final case class Translation(lang: String, text: String) derives JsonEncoder
  private final case class AudioHeader(rate: Int, bytes: Int) derives JsonEncoder
  private final case class Status(text: String) derives JsonEncoder
  private def typed(kind: String, body: String): String = s"""{"type":"$kind",${body.drop(1)}"""

  /** Language names whisper reports -> ISO codes we use. */
  private def iso(whisperName: String): String = whisperName.toLowerCase match
    case "spanish" | "es" => "es"
    case "english" | "en" => "en"
    case other            => other.take(2)

  /**
   * Run a session: consume `in` until Closed, emit on `out`. Transcription and translation run in
   * a single worker fiber so phrases are delivered in order; partials are skipped while it is busy.
   */
  def run(in: Queue[In], out: Queue[Out], speech: Speech, ollama: Ollama, translatorModel: String, live: LiveConfig): ZIO[Any, Nothing, Unit] =
    for
      cfgRef   <- Ref.make(Config())
      vadRef   <- Ref.make(Vad.State())
      busy     <- Ref.make(false)
      lastPart <- Ref.make(0L)
      phrases  <- Queue.bounded[Chunk[Byte]](8)
      vad       = Vad(silenceMs = live.silenceMs, maxSegmentMs = live.maxSegmentMs)
      worker   <- phrases.take.flatMap(pcm => busy.set(true) *> handlePhrase(pcm, cfgRef, out, speech, ollama, translatorModel).ensuring(busy.set(false))).forever.fork
      _        <- out.offer(Out.Text(typed("status", Status("listening").toJson)))
      _        <- in.take.flatMap {
        case In.Closed => ZIO.succeed(false)
        case In.Control(json) =>
          ZIO.fromEither(json.fromJson[Config]).foldZIO(
            e => out.offer(Out.Text(typed("status", Status(s"bad config: $e").toJson))).unit,
            c => cfgRef.set(c) *> out.offer(Out.Text(typed("status", Status(s"pair ${c.a}/${c.b}").toJson))).unit
          ).as(true)
        case In.Pcm(bytes) =>
          for
            st <- vadRef.get
            (st2, events) = vad.feed(st, bytes)
            _ <- vadRef.set(st2)
            _ <- ZIO.foreachDiscard(events) {
              case Vad.Event.Start   => out.offer(Out.Text(typed("status", Status("speech").toJson))).unit
              case Vad.Event.End(seg) => ZIO.logInfo(s"live: phrase ${seg.length / 32000.0}s") *> phrases.offer(seg).unit
            }
            // Partial transcript of the open phrase, at most every partialEveryMs and only when idle.
            now <- Clock.currentTime(java.util.concurrent.TimeUnit.MILLISECONDS)
            lp  <- lastPart.get
            b   <- busy.get
            _   <- ZIO.when(st2.inSpeech && !b && now - lp > live.partialEveryMs && st2.segment.length > 16000) {
                     lastPart.set(now) *> busy.set(true) *>
                       speech.transcribeDetailed(st2.segment, "auto")
                         .flatMap(t => ZIO.when(t.text.nonEmpty)(out.offer(Out.Text(typed("partial", Partial(t.text).toJson)))))
                         .ignore.ensuring(busy.set(false)).forkDaemon
                   }
          yield true
      }.repeatWhile(identity).unit
      _        <- worker.interrupt
    yield ()

  /** Transcribe, translate toward the other language, synthesize, emit. Failures become status lines. */
  private def handlePhrase(pcm: Chunk[Byte], cfgRef: Ref[Config], out: Queue[Out], speech: Speech, ollama: Ollama, model: String): UIO[Unit] =
    val work = for
      cfg <- cfgRef.get
      t   <- speech.transcribeDetailed(pcm, "auto")
      _   <- ZIO.when(t.text.nonEmpty && t.noSpeechProb < 0.6) {
               val from = iso(t.language)
               val to   = if from == cfg.a then cfg.b else cfg.a
               for
                 _   <- ZIO.logInfo(s"live: [$from] ${t.text}")
                 _   <- out.offer(Out.Text(typed("segment", Segment(from, t.text).toJson)))
                 tr  <- ollama.chatWith(model, translationPrompt(from, to), t.text, think = false)
                 _   <- ZIO.logInfo(s"live: [$to] $tr")
                 _   <- out.offer(Out.Text(typed("translation", Translation(to, tr).toJson)))
                 _   <- ZIO.when(cfg.speak) {
                          speech.speak(tr, to).flatMap { wav =>
                            ZIO.fromEither(Audio.parseWav(wav)).mapError(new java.io.IOException(_)).flatMap { w =>
                              val pcm16 = Audio.resample(Audio.toMono(w.pcm, w.channels), w.sampleRate, 16000)
                              out.offer(Out.Text(typed("audio", AudioHeader(16000, pcm16.length).toJson))) *> out.offer(Out.Audio(pcm16))
                            }
                          }
                        }
               yield ()
             }
    yield ()
    work.catchAll(e => ZIO.logWarning(s"live: ${e.getMessage}") *> out.offer(Out.Text(typed("status", Status(s"error: ${e.getMessage}").toJson))).unit)

  def translationPrompt(from: String, to: String): String =
    val name = Map("es" -> "Spanish", "en" -> "English")
    s"""You are a live interpreter at a table in Costa Rica. Translate the user's ${name.getOrElse(from, from)} into natural spoken ${name.getOrElse(to, to)}.
       |Keep it short and conversational, keep names and numbers, use Costa Rican usage (usted) when translating into Spanish.
       |Reply with the translation only.""".stripMargin

/** Tunables for the live session. */
final case class LiveConfig(silenceMs: Int, maxSegmentMs: Int, partialEveryMs: Int)
