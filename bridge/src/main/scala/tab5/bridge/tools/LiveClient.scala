package tab5.bridge.tools

import zio.*
import tab5.bridge.Audio
import java.net.URI
import java.net.http.{HttpClient, WebSocket}
import java.nio.ByteBuffer
import java.nio.file.{Files, Paths}
import java.util.concurrent.{CompletionStage, ConcurrentLinkedQueue}

/**
 * Test client for the live translator, on the JDK's WebSocket. Streams WAV files (any rate,
 * mono or stereo) to the bridge in real time as 16 kHz PCM with a pause between files, prints
 * the events that come back, and writes received audio to /tmp/live-N.pcm (16 kHz mono).
 *
 *   java -cp tab5-bridge.jar tab5.bridge.tools.LiveClient ws://spark-4dfb.local:8765/translate/live a.wav b.wav
 */
object LiveClient extends ZIOAppDefault:

  private val frameMs = 100
  private val frameBytes = 16000 * frameMs / 1000 * 2

  /** Collects incoming frames; the JDK delivers text and binary in fragments, so we reassemble. */
  private final class Listener(events: ConcurrentLinkedQueue[Either[String, Array[Byte]]]) extends WebSocket.Listener:
    private val text = new StringBuilder
    private var bin  = Array.empty[Byte]
    override def onOpen(ws: WebSocket): Unit = { events.add(Left("connected")); ws.request(1) }
    override def onText(ws: WebSocket, data: CharSequence, last: Boolean): CompletionStage[?] =
      text.append(data); if last then { events.add(Left(text.toString)); text.clear() }
      ws.request(1); null
    override def onBinary(ws: WebSocket, data: ByteBuffer, last: Boolean): CompletionStage[?] =
      val a = new Array[Byte](data.remaining()); data.get(a); bin = bin ++ a
      if last then { events.add(Right(bin)); bin = Array.empty }
      ws.request(1); null
    override def onClose(ws: WebSocket, code: Int, reason: String): CompletionStage[?] = { events.add(Left(s"closed $code $reason")); null }
    override def onError(ws: WebSocket, e: Throwable): Unit = { events.add(Left(s"error ${e.getMessage}")): Unit }

  def run: ZIO[ZIOAppArgs, Throwable, Unit] =
    for
      args  <- getArgs
      url    = args.headOption.getOrElse("ws://spark-4dfb.local:8765/translate/live")
      pcm   <- ZIO.foreach(args.drop(1).toList) { f =>
                 ZIO.fromEither(Audio.parseWav(Chunk.fromArray(Files.readAllBytes(Paths.get(f))))).mapError(new RuntimeException(_))
                   .map(w => Audio.resample(Audio.toMono(w.pcm, w.channels), w.sampleRate, 16000))
               }
      stream = pcm.foldLeft(Audio.silence(800))((acc, p) => acc ++ p ++ Audio.silence(1200)) ++ Audio.silence(2500)
      _     <- Console.printLine(s"streaming ${stream.length / 32000} s of audio to $url")
      events = new ConcurrentLinkedQueue[Either[String, Array[Byte]]]()
      ws    <- ZIO.fromCompletionStage(HttpClient.newHttpClient().newWebSocketBuilder().buildAsync(URI.create(url), new Listener(events)))
      _     <- ZIO.fromCompletionStage(ws.sendText("""{"type":"config","a":"es","b":"en","speak":true}""", true))
      start <- Clock.currentTime(java.util.concurrent.TimeUnit.MILLISECONDS)
      count <- Ref.make(0)
      drain  = ZIO.attempt(Iterator.continually(events.poll()).takeWhile(_ != null).toList).flatMap { evs =>
                 ZIO.foreachDiscard(evs) {
                   case Left(t)  => Clock.currentTime(java.util.concurrent.TimeUnit.MILLISECONDS).flatMap(ms => Console.printLine(f"${(ms - start) / 1000.0}%5.1fs  $t"))
                   case Right(b) => count.updateAndGet(_ + 1).flatMap(n => ZIO.attempt(Files.write(Paths.get(s"/tmp/live-$n.pcm"), b)) *> Console.printLine(s"        <audio ${b.length} bytes -> /tmp/live-$n.pcm>"))
                 }
               }
      _     <- ZIO.foreachDiscard(stream.grouped(frameBytes).toList) { f =>
                 ZIO.fromCompletionStage(ws.sendBinary(ByteBuffer.wrap(f.toArray), true)) *> drain *> ZIO.sleep(frameMs.millis)
               }
      _     <- (drain *> ZIO.sleep(500.millis)).repeatN(30)      // let the last translations arrive
      _     <- ZIO.fromCompletionStage(ws.sendClose(WebSocket.NORMAL_CLOSURE, "done")).ignore
      _     <- drain
    yield ()
