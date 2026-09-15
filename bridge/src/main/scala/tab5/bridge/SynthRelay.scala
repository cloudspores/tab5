package tab5.bridge

import zio.*
import zio.stream.*
import java.io.{InputStream, OutputStream}
import java.net.{DatagramSocket, InetAddress}

/**
 * Relays the Tab5 synth's live audio to Sonos.
 *
 * The Tab5 pushes 44.1 kHz 16-bit stereo PCM over a WebSocket. An ffmpeg process encodes it
 * to a constant-bitrate MP3 stream, which is published to every HTTP subscriber; Sonos is then
 * pointed at that URL as an internet radio station, the same path the radio app uses for
 * SomaFM. When the Tab5 is silent or disconnected the encoder is fed silence so the stream
 * never stalls. Sonos buffers a second or two of a radio stream, so this is a relay, not a
 * monitor: the Tab5's own speaker stays the low-latency output.
 */
trait SynthRelay:
  /** Accept a chunk of PCM from the device. */
  def push(pcm: Chunk[Byte]): UIO[Unit]
  /** A live MP3 stream for one HTTP client. */
  def stream: UIO[ZStream[Any, Nothing, Byte]]
  /** Public URL of the stream, as a Sonos player on the LAN must fetch it. */
  def streamUrl: UIO[String]

object SynthRelay:
  def push(pcm: Chunk[Byte]): URIO[SynthRelay, Unit] = ZIO.serviceWithZIO(_.push(pcm))
  def stream: URIO[SynthRelay, ZStream[Any, Nothing, Byte]] = ZIO.serviceWithZIO(_.stream)
  def streamUrl: URIO[SynthRelay, String] = ZIO.serviceWithZIO(_.streamUrl)

  private val SampleRate = 44100
  private val FrameBytes = 4                                   // 16-bit stereo
  private val SilenceMs  = 100                                 // written when nothing arrives for this long
  private val Silence    = Chunk.fill(SampleRate * FrameBytes * SilenceMs / 1000)(0.toByte)

  val live: ZLayer[BridgeConfig, Throwable, SynthRelay] = ZLayer.scoped {
    for
      cfg   <- ZIO.service[BridgeConfig]
      host  <- ZIO.attempt(if cfg.synth.publicHost.nonEmpty then cfg.synth.publicHost else detectHost(cfg.sonos.scanSubnet))
      pcm   <- Queue.sliding[Chunk[Byte]](8)                   // ~370 ms at the device's 46 ms chunks: anything queued here is added latency
      hub   <- Hub.sliding[Chunk[Byte]](256)
      proc  <- ZIO.acquireRelease(startEncoder(cfg.synth.bitrateKbps))(p => ZIO.succeed(p.destroy()))
      _     <- writer(pcm, proc.getOutputStream).forkScoped
      _     <- reader(proc.getInputStream, hub).forkScoped
      _     <- ZIO.logInfo(s"synth relay: ffmpeg ${cfg.synth.bitrateKbps} kbps, stream at http://$host:${cfg.port}/synth/stream.mp3")
    yield new SynthRelay:
      def push(bytes: Chunk[Byte]): UIO[Unit] = pcm.offer(bytes).unit
      def stream: UIO[ZStream[Any, Nothing, Byte]] =
        ZIO.succeed(ZStream.unwrapScoped(hub.subscribe.map(q => ZStream.fromQueue(q).flattenChunks)))
      def streamUrl: UIO[String] = ZIO.succeed(s"http://$host:${cfg.port}/synth/stream.mp3")
  }

  /** ffmpeg: raw PCM in, MP3 out, flushing every packet to keep latency low. */
  private def startEncoder(kbps: Int): Task[Process] = ZIO.attempt {
    val cmd = List("ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
      "-f", "s16le", "-ar", SampleRate.toString, "-ac", "2", "-i", "pipe:0",
      "-codec:a", "libmp3lame", "-b:a", s"${kbps}k", "-f", "mp3", "-flush_packets", "1", "pipe:1")
    new ProcessBuilder(cmd*).redirectError(ProcessBuilder.Redirect.INHERIT).start()
  }

  /** Feed the encoder in real time: device audio when it arrives, silence otherwise. */
  private def writer(pcm: Queue[Chunk[Byte]], out: OutputStream): UIO[Unit] =
    pcm.take.timeout(SilenceMs.millis).flatMap { got =>
      val bytes = got.getOrElse(Silence)
      ZIO.attemptBlocking { out.write(bytes.toArray); out.flush() }.ignore
    }.forever

  /** Publish the encoder's output to the subscribers. */
  private def reader(in: InputStream, hub: Hub[Chunk[Byte]]): UIO[Unit] =
    ZIO.attemptBlocking {
      val buf = new Array[Byte](4096)
      val n = in.read(buf)
      if n > 0 then Chunk.fromArray(buf.take(n)) else Chunk.empty
    }.orElseSucceed(Chunk.empty).flatMap(c => if c.isEmpty then ZIO.sleep(10.millis) else hub.publish(c).unit).forever

  /** The address a Sonos player would reach this host at: the interface that routes to the Sonos subnet. */
  private def detectHost(subnet: String): String =
    val probe = subnet.takeWhile(_ != '/').split('.') match
      case Array(a, b, c, _) => s"$a.$b.$c.1"
      case _                 => "10.0.0.1"
    val s = new DatagramSocket()
    try { s.connect(InetAddress.getByName(probe), 1400); s.getLocalAddress.getHostAddress }
    finally s.close()
