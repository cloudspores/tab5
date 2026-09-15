package tab5.bridge

import zio.*
import zio.config.*
import zio.config.magnolia.*
import zio.config.typesafe.*

/** Ollama endpoint on the Spark and the model used for chat and translation. */
final case class OllamaConfig(url: String, model: String)

/** Sonos discovery settings. */
final case class SonosConfig(scanSubnet: String, topologyTtlSeconds: Int)

/** Speech: whisper.cpp server for transcription, Piper binary and voices for synthesis. */
final case class SpeechConfig(whisperUrl: String, piperBin: String, voiceDir: String, voiceEn: String, voiceEs: String)

/** Live translator: the fast model used for phrases and the segmentation tunables. */
final case class TranslatorConfig(model: String, live: LiveConfig)

/** Synth relay: the address Sonos fetches the stream from (empty = detect) and the MP3 bitrate. */
final case class SynthConfig(publicHost: String, bitrateKbps: Int)

/** Whole-service configuration, loaded from application.conf with environment overrides. */
final case class BridgeConfig(port: Int, ollama: OllamaConfig, sonos: SonosConfig, speech: SpeechConfig, translator: TranslatorConfig, synth: SynthConfig)

object BridgeConfig:
  private val descriptor: Config[BridgeConfig] = deriveConfig[BridgeConfig].mapKey(toKebabCase)

  /** Layer that reads application.conf (HOCON) from the classpath. */
  val layer: Layer[Throwable, BridgeConfig] =
    ZLayer.fromZIO(
      ZIO.config(descriptor).provideLayer(Runtime.setConfigProvider(ConfigProvider.fromResourcePath()))
    )
