package tab5.bridge

import zio.*
import zio.http.*
import zio.json.*

/** One chat turn in Ollama's /api/chat format. */
final case class ChatMessage(role: String, content: String) derives JsonCodec

private final case class ChatRequest(model: String, messages: List[ChatMessage], stream: Boolean = false) derives JsonEncoder
private final case class ChatResponse(message: ChatMessage) derives JsonDecoder

/** Text generation on the Spark through Ollama's chat API. */
trait Ollama:
  def chat(system: String, user: String): Task[String]

object Ollama:
  def chat(system: String, user: String): RIO[Ollama, String] = ZIO.serviceWithZIO(_.chat(system, user))

  val live: ZLayer[BridgeConfig & Client, Nothing, Ollama] = ZLayer.fromFunction { (cfg: BridgeConfig, client: Client) =>
    new Ollama:
      def chat(system: String, user: String): Task[String] =
        val body = ChatRequest(cfg.ollama.model, List(ChatMessage("system", system), ChatMessage("user", user))).toJson
        val req = Request
          .post(URL.decode(s"${cfg.ollama.url}/api/chat").toOption.get, Body.fromString(body))
          .addHeader(Header.ContentType(MediaType.application.json))
        for
          resp <- client.batched(req).timeoutFail(new java.io.IOException("ollama timed out"))(120.seconds)
          text <- resp.body.asString
          _    <- ZIO.fail(new java.io.IOException(s"ollama HTTP ${resp.status.code}: ${text.take(200)}")).when(!resp.status.isSuccess)
          out  <- ZIO.fromEither(text.fromJson[ChatResponse]).mapError(e => new java.io.IOException(s"ollama response: $e"))
        yield out.message.content.trim
  }
