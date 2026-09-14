package tab5.bridge

import zio.Chunk

/**
 * Energy-based voice activity segmentation for 16 kHz mono PCM.
 *
 * A pure state machine: feed it audio, get segments back. Speech starts after a few frames above
 * an adaptive noise floor and ends after a stretch of quiet; a short pre-roll is kept so the first
 * syllable is not clipped, and overlong segments are cut so translation can start.
 */
final case class Vad(
    frameMs: Int = 20,
    startFrames: Int = 3,             // consecutive loud frames to open a segment
    silenceMs: Int = 600,             // quiet after speech that closes a segment
    maxSegmentMs: Int = 10000,        // hard cap so a monologue still gets translated in pieces
    preRollMs: Int = 240,
    minAbsolute: Double = 350.0,      // never trigger below this RMS (idle mic noise)
    ratio: Double = 3.0               // trigger at this multiple of the noise floor
):
  import Vad.*

  private val frameBytes = 16000 * frameMs / 1000 * 2
  private val preRollFrames = preRollMs / frameMs
  private val silenceFrames = silenceMs / frameMs
  private val maxFrames = maxSegmentMs / frameMs

  /** Feed a chunk; returns the updated state and any events (segment boundaries). */
  def feed(state: State, pcm: Chunk[Byte]): (State, List[Event]) =
    var st = state.copy(pending = state.pending ++ pcm)
    val events = List.newBuilder[Event]
    while st.pending.length >= frameBytes do
      val frame = st.pending.take(frameBytes)
      st = st.copy(pending = st.pending.drop(frameBytes))
      val level = Audio.rms(frame)
      // Noise floor: follows quiet quickly, rises slowly, and only while nobody is speaking, so a
      // long sentence cannot lift the floor above itself. Never below a small constant.
      val floor =
        if level < st.noise then st.noise * 0.9 + level * 0.1
        else if !st.inSpeech then st.noise * 0.995 + level * 0.005
        else st.noise
      st = st.copy(noise = math.max(50.0, floor))
      val loud = level > math.max(minAbsolute, st.noise * ratio)
      if !st.inSpeech then
        val roll = (st.preRoll :+ frame).takeRight(preRollFrames)
        val run = if loud then st.loudRun + 1 else 0
        st = st.copy(preRoll = roll, loudRun = run)
        if run >= startFrames then
          st = st.copy(inSpeech = true, segment = roll.foldLeft(Chunk.empty[Byte])(_ ++ _), quietRun = 0, frames = roll.size, loudRun = 0)
          events += Event.Start
      else
        st = st.copy(segment = st.segment ++ frame, frames = st.frames + 1, quietRun = if loud then 0 else st.quietRun + 1)
        if st.quietRun >= silenceFrames then
          // Pause: close the phrase, keeping half the trailing quiet so the last word is not cut.
          events += Event.End(st.segment.dropRight((st.quietRun - silenceFrames / 2) * frameBytes))
          st = st.copy(inSpeech = false, segment = Chunk.empty, quietRun = 0, frames = 0, preRoll = Vector.empty)
        else if st.frames >= maxFrames then
          // Overlong: cut here and carry straight on with a new phrase; the speaker has not paused.
          events += Event.End(st.segment)
          events += Event.Start
          st = st.copy(segment = Chunk.empty, quietRun = 0, frames = 0)
    (st, events.result())

object Vad:
  final case class State(
      pending: Chunk[Byte] = Chunk.empty,
      noise: Double = 300.0,
      preRoll: Vector[Chunk[Byte]] = Vector.empty,
      loudRun: Int = 0,
      inSpeech: Boolean = false,
      segment: Chunk[Byte] = Chunk.empty,
      quietRun: Int = 0,
      frames: Int = 0
  )
  enum Event:
    case Start
    case End(pcm: Chunk[Byte])
