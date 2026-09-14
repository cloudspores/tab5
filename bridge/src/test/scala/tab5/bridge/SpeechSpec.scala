package tab5.bridge

import zio.test.*

object SpeechSpec extends ZIOSpecDefault:
  def spec = suite("Speech")(
    test("WAV header describes 16 kHz mono 16-bit PCM") {
      val h = Speech.wavHeader(32000, 16000).toArray
      val bb = java.nio.ByteBuffer.wrap(h).order(java.nio.ByteOrder.LITTLE_ENDIAN)
      assertTrue(
        h.length == 44,
        new String(h, 0, 4) == "RIFF",
        new String(h, 8, 4) == "WAVE",
        bb.getInt(24) == 16000,     // sample rate
        bb.getShort(22) == 1,       // channels
        bb.getShort(34) == 16,      // bits
        bb.getInt(40) == 32000      // data length
      )
    }
  )
