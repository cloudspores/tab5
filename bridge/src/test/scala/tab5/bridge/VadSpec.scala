package tab5.bridge

import zio.Chunk
import zio.test.*

object VadSpec extends ZIOSpecDefault:
  /** A 440 Hz tone at a given amplitude, 16 kHz mono. */
  private def tone(ms: Int, amp: Int): Chunk[Byte] =
    Audio.bytes(Array.tabulate(16000 * ms / 1000)(i => (amp * math.sin(2 * math.Pi * 440 * i / 16000)).toShort))

  def spec = suite("Vad")(
    test("silence produces no segments") {
      val (_, ev) = Vad().feed(Vad.State(), Audio.silence(3000))
      assertTrue(ev.isEmpty)
    },
    test("a burst of sound between silences yields one segment containing it") {
      val vad = Vad()
      val audio = Audio.silence(1000) ++ tone(1200, 8000) ++ Audio.silence(1500)
      val (st, ev) = vad.feed(Vad.State(), audio)
      val ends = ev.collect { case Vad.Event.End(p) => p }
      assertTrue(
        ev.contains(Vad.Event.Start),
        ends.size == 1,
        ends.head.length > 16000 * 2 * 1,      // at least ~1 s of audio (tone + pre-roll + trailing quiet)
        ends.head.length < 16000 * 2 * 3,
        !st.inSpeech
      )
    },
    test("a long monologue is cut at the maximum segment length") {
      val vad = Vad(maxSegmentMs = 2000)
      val (_, ev) = vad.feed(Vad.State(), tone(5000, 8000) ++ Audio.silence(1000))
      assertTrue(ev.collect { case Vad.Event.End(_) => 1 }.sum >= 2)
    },
    test("resampling keeps duration and mono downmix halves stereo") {
      val mono22 = Audio.bytes(Array.fill(22050)(1000.toShort))
      val r = Audio.resample(mono22, 22050, 16000)
      val stereo = Audio.bytes(Array.fill(200)(500.toShort))
      assertTrue(r.length == 16000 * 2, Audio.toMono(stereo, 2).length == 200)
    }
  )
