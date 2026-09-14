package tab5.bridge

import zio.Chunk
import java.nio.{ByteBuffer, ByteOrder}

/** Small PCM utilities: WAV parsing, resampling, level measurement. All audio is 16-bit signed. */
object Audio:

  /** Parsed WAV: sample rate, channel count, and interleaved PCM bytes. */
  final case class Wav(sampleRate: Int, channels: Int, pcm: Chunk[Byte])

  /** Parse a RIFF/WAVE file with PCM data. Handles extra chunks (LIST etc.) before "data". */
  def parseWav(bytes: Chunk[Byte]): Either[String, Wav] =
    val b = ByteBuffer.wrap(bytes.toArray).order(ByteOrder.LITTLE_ENDIAN)
    if bytes.length < 12 || new String(bytes.take(4).toArray) != "RIFF" || new String(bytes.slice(8, 12).toArray) != "WAVE" then
      Left("not a WAV file")
    else
      var pos = 12
      var rate = 0; var ch = 0; var bits = 0
      var data: Option[Chunk[Byte]] = None
      while pos + 8 <= bytes.length && data.isEmpty do
        val id = new String(bytes.slice(pos, pos + 4).toArray)
        val size = b.getInt(pos + 4)
        if id == "fmt " then
          ch = b.getShort(pos + 10); rate = b.getInt(pos + 12); bits = b.getShort(pos + 22)
        else if id == "data" then
          val len = math.min(size, bytes.length - pos - 8)
          data = Some(bytes.slice(pos + 8, pos + 8 + len))
        pos += 8 + size + (size & 1)
      (data, bits) match
        case (Some(d), 16) if rate > 0 && ch > 0 => Right(Wav(rate, ch, d))
        case (None, _)                           => Left("no data chunk")
        case (_, other)                          => Left(s"unsupported bit depth $other")

  /** Samples of a mono 16-bit little-endian buffer. */
  def samples(pcm: Chunk[Byte]): Array[Short] =
    val b = ByteBuffer.wrap(pcm.toArray).order(ByteOrder.LITTLE_ENDIAN)
    Array.tabulate(pcm.length / 2)(i => b.getShort(i * 2))

  def bytes(s: Array[Short]): Chunk[Byte] =
    val b = ByteBuffer.allocate(s.length * 2).order(ByteOrder.LITTLE_ENDIAN)
    s.foreach(b.putShort)
    Chunk.fromArray(b.array())

  /** Mix interleaved channels down to mono. */
  def toMono(pcm: Chunk[Byte], channels: Int): Chunk[Byte] =
    if channels <= 1 then pcm
    else
      val s = samples(pcm)
      bytes(Array.tabulate(s.length / channels)(i => ((0 until channels).map(c => s(i * channels + c).toInt).sum / channels).toShort))

  /** Linear-interpolation resampling of mono PCM. Good enough for speech. */
  def resample(pcm: Chunk[Byte], from: Int, to: Int): Chunk[Byte] =
    if from == to then pcm
    else
      val in = samples(pcm)
      val outLen = (in.length.toLong * to / from).toInt
      val out = new Array[Short](outLen)
      val step = from.toDouble / to
      var i = 0
      while i < outLen do
        val pos = i * step
        val k = pos.toInt
        val frac = pos - k
        val a = in(math.min(k, in.length - 1)).toDouble
        val b = in(math.min(k + 1, in.length - 1)).toDouble
        out(i) = (a + (b - a) * frac).round.toShort
        i += 1
      bytes(out)

  /** Root-mean-square level of mono PCM, 0..32767. */
  def rms(pcm: Chunk[Byte]): Double =
    val s = samples(pcm)
    if s.isEmpty then 0.0 else math.sqrt(s.foldLeft(0.0)((acc, v) => acc + v.toDouble * v) / s.length)

  /** Silence of the given length at 16 kHz mono. */
  def silence(ms: Int): Chunk[Byte] = Chunk.fill(16000 * ms / 1000 * 2)(0.toByte)
