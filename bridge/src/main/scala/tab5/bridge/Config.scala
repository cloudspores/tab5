package tab5.bridge

import zio.*
import zio.config.*
import zio.config.magnolia.*
import zio.config.typesafe.*

/** Ollama endpoint on the Spark and the model used for chat and translation. */
final case class OllamaConfig(url: String, model: String)

/** Sonos discovery settings. */
final case class SonosConfig(scanSubnet: String, topologyTtlSeconds: Int)

/** Whole-service configuration, loaded from application.conf with environment overrides. */
final case class BridgeConfig(port: Int, ollama: OllamaConfig, sonos: SonosConfig)

object BridgeConfig:
  private val descriptor: Config[BridgeConfig] = deriveConfig[BridgeConfig].mapKey(toKebabCase)

  /** Layer that reads application.conf (HOCON) from the classpath. */
  val layer: Layer[Throwable, BridgeConfig] =
    ZLayer.fromZIO(
      ZIO.config(descriptor).provideLayer(Runtime.setConfigProvider(ConfigProvider.fromResourcePath()))
    )
