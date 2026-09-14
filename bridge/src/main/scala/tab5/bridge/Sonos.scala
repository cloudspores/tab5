package tab5.bridge

import zio.*
import zio.http.*
import zio.json.*
import java.net.{DatagramPacket, DatagramSocket, InetAddress, InetSocketAddress, Socket}

/** A Sonos player as the Tab5 sees it. */
final case class Room(
    name: String,
    ip: String,
    uuid: String,
    coordinatorUuid: String,
    volume: Int,
    muted: Boolean,
    state: String,
    playing: Boolean,
    title: String,
    artist: String
) derives JsonEncoder

/** Player identity from the household topology; state is fetched on demand. */
private final case class Player(name: String, ip: String, uuid: String, coordinatorUuid: String)

/**
 * Sonos control over UPnP: discovery (SSDP, then a subnet scan), household topology, and the
 * handful of AVTransport / RenderingControl actions the Tab5 needs.
 */
trait Sonos:
  def rooms: Task[List[Room]]
  def state(room: String): Task[Room]
  def playUrl(room: String, url: String, title: String): Task[String]
  def command(room: String, action: String): Task[String]
  def setVolume(room: String, volume: Int): Task[String]

object Sonos:
  def rooms: RIO[Sonos, List[Room]] = ZIO.serviceWithZIO(_.rooms)
  def state(room: String): RIO[Sonos, Room] = ZIO.serviceWithZIO(_.state(room))
  def playUrl(room: String, url: String, title: String): RIO[Sonos, String] = ZIO.serviceWithZIO(_.playUrl(room, url, title))
  def command(room: String, action: String): RIO[Sonos, String] = ZIO.serviceWithZIO(_.command(room, action))
  def setVolume(room: String, volume: Int): RIO[Sonos, String] = ZIO.serviceWithZIO(_.setVolume(room, volume))

  val live: ZLayer[BridgeConfig & Client, Nothing, Sonos] = ZLayer.fromZIO(
    for
      cfg    <- ZIO.service[BridgeConfig]
      client <- ZIO.service[Client]
      cache  <- Ref.make(Option.empty[(Long, List[Player])])
    yield SonosLive(cfg.sonos, client, cache)
  )

private final case class SonosLive(cfg: SonosConfig, client: Client, cache: Ref[Option[(Long, List[Player])]]) extends Sonos:

  private val Port = 1400

  // ------------------------------------------------------------------ discovery

  /** SSDP M-SEARCH for ZonePlayers; returns the IPs that answered within the timeout. */
  private def ssdp: Task[List[String]] = ZIO.attemptBlocking {
    val msg =
      "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n"
    val socket = new DatagramSocket()
    try
      socket.setSoTimeout(2500)
      val bytes = msg.getBytes("UTF-8")
      socket.send(new DatagramPacket(bytes, bytes.length, InetAddress.getByName("239.255.255.250"), 1900))
      val found = scala.collection.mutable.LinkedHashSet[String]()
      val buf   = new Array[Byte](2048)
      val deadline = java.lang.System.currentTimeMillis() + 2500
      while java.lang.System.currentTimeMillis() < deadline do
        try
          val p = new DatagramPacket(buf, buf.length)
          socket.receive(p)
          found += p.getAddress.getHostAddress
        catch case _: java.net.SocketTimeoutException => ()
      found.toList
    finally socket.close()
  }

  /** Fallback when multicast is blocked: try port 1400 on every host of the configured /24. */
  private def scan: Task[List[String]] =
    val prefix = cfg.scanSubnet.takeWhile(_ != '/').split('.').take(3).mkString(".")
    ZIO
      .foreachPar((1 to 254).toList) { i =>
        val ip = s"$prefix.$i"
        ZIO.attemptBlocking {
          val s = new Socket()
          try { s.connect(new InetSocketAddress(ip, Port), 400); Some(ip) }
          catch case _: Exception => None
          finally s.close()
        }.orElseSucceed(None)
      }
      .withParallelism(64)
      .map(_.flatten)

  /** Ask one player for the whole household. Hidden members of stereo pairs are skipped. */
  private def topology(ip: String): Task[List[Player]] =
    soap(ip, "ZoneGroupTopology", "GetZoneGroupState", Nil).map { body =>
      val escaped = Soap.field(body, "ZoneGroupState")
      Soap.inner(escaped).toList.flatMap { xml =>
        (xml \\ "ZoneGroup").flatMap { g =>
          val coord = (g \ "@Coordinator").text
          (g \ "ZoneGroupMember").filter(m => (m \ "@Invisible").text != "1").map { m =>
            val loc = (m \ "@Location").text
            val memberIp = loc.stripPrefix("http://").takeWhile(_ != ':')
            Player((m \ "@ZoneName").text, memberIp, (m \ "@UUID").text, coord)
          }
        }.toList
      }
    }

  /** Discovered players, cached for the configured TTL. */
  private def players: Task[List[Player]] =
    for
      now    <- Clock.currentTime(java.util.concurrent.TimeUnit.SECONDS)
      cached <- cache.get
      result <- cached match
        case Some((ts, ps)) if now - ts < cfg.topologyTtlSeconds && ps.nonEmpty => ZIO.succeed(ps)
        case _ =>
          for
            seeds <- ssdp.flatMap(s => if s.nonEmpty then ZIO.succeed(s) else ZIO.logInfo("sonos: multicast found nothing, scanning subnet") *> scan)
            ps    <- ZIO.firstSuccessOf(topology(seeds.headOption.getOrElse("0.0.0.0")), seeds.drop(1).map(topology))
                       .catchAll(e => ZIO.logWarning(s"sonos: topology failed: ${e.getMessage}").as(Nil))
            _     <- ZIO.logInfo(s"sonos: ${ps.size} rooms: ${ps.map(_.name).sorted.mkString(", ")}")
            _     <- cache.set(Some((now, ps)))
          yield ps
    yield result

  private def find(room: String): Task[Player] =
    players.flatMap { ps =>
      val low = room.trim.toLowerCase
      ZIO
        .fromOption(ps.find(_.name.toLowerCase == low).orElse(ps.find(_.name.toLowerCase.contains(low))))
        .orElseFail(new NoSuchElementException(s"No Sonos room called $room"))
    }

  /** Transport actions go to the group coordinator; volume goes to the player itself. */
  private def coordinatorOf(p: Player): Task[Player] =
    players.map(ps => ps.find(_.uuid == p.coordinatorUuid).getOrElse(p))

  // ------------------------------------------------------------------ SOAP transport

  private def soap(ip: String, service: String, action: String, args: Seq[(String, String)]): Task[String] =
    val path = service match
      case "ZoneGroupTopology" => "/ZoneGroupTopology/Control"
      case "RenderingControl"  => "/MediaRenderer/RenderingControl/Control"
      case _                   => "/MediaRenderer/AVTransport/Control"
    // Sonos closes the connection after every response; say so up front so no pooled socket is reused.
    val req = Request
      .post(URL.decode(s"http://$ip:$Port$path").toOption.get, Body.fromString(Soap.envelope(service, action, args)))
      .addHeader(Header.ContentType(MediaType.text.xml, charset = Some(java.nio.charset.StandardCharsets.UTF_8)))
      .addHeader(Header.Custom("SOAPACTION", Soap.actionHeader(service, action)))
      .addHeader(Header.Connection.Close)
    for
      resp <- client.batched(req).timeoutFail(new java.io.IOException(s"$action timed out on $ip"))(8.seconds)
      body <- resp.body.asString
      _    <- ZIO.fail(new java.io.IOException(s"$action on $ip: HTTP ${resp.status.code} ${body.take(200)}"))
                .when(!resp.status.isSuccess)
    yield body

  private def av(p: Player, action: String, args: (String, String)*): Task[String] =
    soap(p.ip, "AVTransport", action, ("InstanceID", "0") +: args)

  private def rc(p: Player, action: String, args: (String, String)*): Task[String] =
    soap(p.ip, "RenderingControl", action, ("InstanceID", "0") +: ("Channel", "Master") +: args)

  // ------------------------------------------------------------------ API

  private def roomState(p: Player): Task[Room] =
    for
      coord  <- coordinatorOf(p)
      tinfo  <- av(coord, "GetTransportInfo").map(Soap.field(_, "CurrentTransportState"))
                  .catchAll(e => ZIO.logWarning(s"sonos: ${p.name} transport: ${e.getMessage}").as("UNKNOWN"))
      vol    <- rc(p, "GetVolume").map(Soap.field(_, "CurrentVolume")).map(_.toIntOption.getOrElse(0)).orElseSucceed(0)
      mute   <- rc(p, "GetMute").map(Soap.field(_, "CurrentMute") == "1").orElseSucceed(false)
      meta   <- av(coord, "GetPositionInfo").map(Soap.field(_, "TrackMetaData")).orElseSucceed("")
      didl    = Soap.inner(meta)
      // Radio streams put the live text in r:streamContent; tracks use dc:title / dc:creator.
      stream  = didl.map(x => Soap.text(x, "streamContent")).getOrElse("")
      title   = if stream.nonEmpty then stream else didl.map(x => Soap.text(x, "title")).getOrElse("")
      artist  = if stream.nonEmpty then "" else didl.map(x => Soap.text(x, "creator")).getOrElse("")
    yield Room(p.name, p.ip, p.uuid, p.coordinatorUuid, vol, mute, tinfo, tinfo == "PLAYING", title, artist)

  def rooms: Task[List[Room]] =
    players.flatMap(ps => ZIO.foreachPar(ps.sortBy(_.name.toLowerCase))(roomState).withParallelism(4))

  def state(room: String): Task[Room] = find(room).flatMap(roomState)

  def playUrl(room: String, url: String, title: String): Task[String] =
    for
      p     <- find(room)
      coord <- coordinatorOf(p)
      _     <- av(coord, "SetAVTransportURI", "CurrentURI" -> Soap.radioUri(url), "CurrentURIMetaData" -> Soap.radioMetadata(title))
      _     <- av(coord, "Play", "Speed" -> "1")
    yield s"Playing ${if title.nonEmpty then title else url} on ${p.name}."

  def command(room: String, action: String): Task[String] =
    for
      p     <- find(room)
      coord <- coordinatorOf(p)
      msg   <- action.toLowerCase match
        case "play"   => av(coord, "Play", "Speed" -> "1").as("Playing.")
        case "pause"  => av(coord, "Pause").as("Paused.")
        case "stop"   => av(coord, "Stop").as("Stopped.")
        case "next"   => av(coord, "Next").as("Skipped.")
        case "mute"   => rc(p, "SetMute", "DesiredMute" -> "1").as(s"${p.name} muted.")
        case "unmute" => rc(p, "SetMute", "DesiredMute" -> "0").as(s"${p.name} unmuted.")
        case other    => ZIO.fail(new IllegalArgumentException(s"Unknown Sonos action $other"))
    yield msg

  def setVolume(room: String, volume: Int): Task[String] =
    for
      p <- find(room)
      v  = volume.max(0).min(100)
      _ <- rc(p, "SetVolume", "DesiredVolume" -> v.toString)
    yield s"${p.name} volume $v."
