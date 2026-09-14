package tab5.bridge

import zio.*
import zio.http.*
import zio.json.*
import java.nio.{ByteBuffer, ByteOrder}

/**
 * Speech services on the Spark.
 *
 *  - Transcription goes to a whisper.cpp server (CUDA build) over its /inference endpoint.
 *  - Synthesis spawns Piper, a C++ text-to-speech binary, and returns the WAV it writes.
 *
 * Devices send raw 16 kHz mono 16-bit PCM (what the Tab5 and knob microphones produce); the
 * bridge wraps it in a WAV header for whisper.
 */
/** A transcription with whisper's language detection and its estimate that the clip is not speech. */
final case class Transcription(text: String, language: String, noSpeechProb: Double)

trait Speech:
  /** Transcribe 16 kHz mono 16-bit PCM. `language` is an ISO code or "auto". */
  def transcribe(pcm16k: Chunk[Byte], language: String): Task[String]
  /** Transcribe with detected language and no-speech probability. */
  def transcribeDetailed(pcm16k: Chunk[Byte], language: String): Task[Transcription]
  /** Synthesize speech; returns a complete WAV file (22.05 kHz mono 16-bit from Piper's medium voices). */
  def speak(text: String, language: String): Task[Chunk[Byte]]

object Speech:
  def transcribe(pcm: Chunk[Byte], language: String): RIO[Speech, String] = ZIO.serviceWithZIO(_.transcribe(pcm, language))
  def speak(text: String, language: String): RIO[Speech, Chunk[Byte]] = ZIO.serviceWithZIO(_.speak(text, language))

  val live: ZLayer[BridgeConfig & Client, Nothing, Speech] = ZLayer.fromFunction { (cfg: BridgeConfig, client: Client) =>
    SpeechLive(cfg.speech, client)
  }

  /** 44-byte RIFF header for PCM data. */
  def wavHeader(dataLen: Int, sampleRate: Int, channels: Int = 1, bits: Int = 16): Chunk[Byte] =
    val b = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
    val byteRate = sampleRate * channels * bits / 8
    b.put("RIFF".getBytes).putInt(36 + dataLen).put("WAVE".getBytes)
      .put("fmt ".getBytes).putInt(16).putShort(1).putShort(channels.toShort).putInt(sampleRate)
      .putInt(byteRate).putShort((channels * bits / 8).toShort).putShort(bits.toShort)
      .put("data".getBytes).putInt(dataLen)
    Chunk.fromArray(b.array())

private final case class WhisperResponse(text: String) derives JsonDecoder
private final case class WhisperSegment(no_speech_prob: Option[Double]) derives JsonDecoder
private final case class WhisperVerbose(text: String, detected_language: Option[String], language: Option[String], segments: Option[List[WhisperSegment]]) derives JsonDecoder

private final case class SpeechLive(cfg: SpeechConfig, client: Client) extends Speech:

  def transcribe(pcm: Chunk[Byte], language: String): Task[String] =
    transcribeDetailed(pcm, language).map(_.text)

  def transcribeDetailed(pcm: Chunk[Byte], language: String): Task[Transcription] =
    val wav = Speech.wavHeader(pcm.length, 16000) ++ pcm
    val form = Form(
      FormField.binaryField("file", wav, MediaType.audio.wav, filename = Some("audio.wav")),
      FormField.simpleField("response_format", "verbose_json"),
      FormField.simpleField("language", if language == "auto" then "auto" else language),
      FormField.simpleField("temperature", "0.0")
    )
    // whisper.cpp's server (cpp-httplib) rejects chunked uploads, so the form is serialized up front
    // and sent with a Content-Length.
    val boundary = Boundary("tab5speech")
    for
      bytes <- form.multipartBytes(boundary).runCollect
      req    = Request
                 .post(URL.decode(s"${cfg.whisperUrl}/inference").toOption.get, Body.fromChunk(bytes))
                 .addHeader(Header.ContentType(MediaType.multipart.`form-data`, boundary = Some(boundary)))
      resp  <- client.batched(req).timeoutFail(new java.io.IOException("whisper timed out"))(60.seconds)
      text <- resp.body.asString
      _    <- ZIO.fail(new java.io.IOException(s"whisper HTTP ${resp.status.code}: ${text.take(200)}")).when(!resp.status.isSuccess)
      out  <- ZIO.fromEither(text.fromJson[WhisperVerbose]).mapError(e => new java.io.IOException(s"whisper response: $e"))
      segs  = out.segments.getOrElse(Nil).flatMap(_.no_speech_prob)
      noSp  = if segs.isEmpty then 0.0 else segs.sum / segs.size
    yield Transcription(out.text.trim, out.detected_language.orElse(out.language).getOrElse("auto"), noSp)

  def speak(text: String, language: String): Task[Chunk[Byte]] =
    val voice = if language.startsWith("es") then cfg.voiceEs else cfg.voiceEn
    val cmd = List(cfg.piperBin, "--model", s"${cfg.voiceDir}/$voice.onnx", "--output_file", "-")
    ZIO.attemptBlocking {
      val pb = new ProcessBuilder(cmd*)
      pb.redirectError(ProcessBuilder.Redirect.DISCARD)
      val p = pb.start()
      val writer = new Thread(() => {
        try { p.getOutputStream.write(text.getBytes("UTF-8")); p.getOutputStream.write('\n') }
        finally p.getOutputStream.close()
      })
      writer.start()
      val out = p.getInputStream.readAllBytes()
      writer.join()
      if p.waitFor() != 0 || out.length <= 44 then throw new java.io.IOException(s"piper failed for voice $voice")
      Chunk.fromArray(out)
    }.timeoutFail(new java.io.IOException("piper timed out"))(30.seconds)
