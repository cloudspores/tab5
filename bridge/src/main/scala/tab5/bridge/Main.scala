package tab5.bridge

import zio.*
import zio.http.*
import zio.http.netty.NettyConfig
import zio.http.netty.client.NettyClientDriver
import zio.logging.backend.SLF4J

/**
 * Entry point. Wires configuration, the HTTP client, the Sonos and Ollama services, and serves the
 * routes. Runs on the DGX Spark as a container; the Tab5 and the knobs find it by mDNS name.
 */
object Main extends ZIOAppDefault:

  override val bootstrap: ZLayer[ZIOAppArgs, Any, Any] = Runtime.removeDefaultLoggers >>> SLF4J.slf4j

  override def run: ZIO[Any, Throwable, Unit] =
    val program = for
      cfg <- ZIO.service[BridgeConfig]
      _   <- ZIO.logInfo(s"tab5-bridge ${BridgeVersion.current} on :${cfg.port}, ollama ${cfg.ollama.url} (${cfg.ollama.model})")
      // Audio uploads (a few seconds of 16 kHz PCM) exceed zio-http's 128 KB default request size.
      server = ZLayer.succeed(Server.Config.default.port(cfg.port).disableRequestStreaming(32 * 1024 * 1024)) >>> Server.live
      _   <- Server.serve(Api.routes(cfg)).provideSomeLayer[Sonos & Ollama & Speech & SynthRelay](server)
    yield ()
    program.provide(BridgeConfig.layer, httpClient, Sonos.live, Ollama.live, Speech.live, SynthRelay.live)

  /**
   * HTTP client with a long idle timeout: the first call to a large model waits for Ollama to load
   * it (tens of seconds), which the default 50 s idle timeout would cut off.
   */
  private val httpClient: ZLayer[Any, Throwable, Client] =
    ZLayer.make[Client](
      ZLayer.succeed(ZClient.Config.default.idleTimeout(5.minutes).connectionTimeout(10.seconds).disabledConnectionPool),
      ZLayer.succeed(NettyConfig.default),
      NettyClientDriver.live,
      DnsResolver.default,
      Client.customized
    )
