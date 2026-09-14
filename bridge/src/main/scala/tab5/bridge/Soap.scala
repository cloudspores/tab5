package tab5.bridge

import scala.xml.{Elem, Node, XML}

/**
 * UPnP SOAP helpers for Sonos. A Sonos player exposes services under
 * http://<ip>:1400/<service>/Control; each action is a small SOAP envelope.
 */
object Soap:

  /** Build the envelope for one action. Argument values are XML-escaped. */
  def envelope(service: String, action: String, args: Seq[(String, String)]): String =
    val body = args.map { case (k, v) => s"<$k>${escape(v)}</$k>" }.mkString
    s"""<?xml version="1.0" encoding="utf-8"?>
       |<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
       |<s:Body><u:$action xmlns:u="urn:schemas-upnp-org:service:$service:1">$body</u:$action></s:Body>
       |</s:Envelope>""".stripMargin

  /** The SOAPACTION header value for an action. */
  def actionHeader(service: String, action: String): String =
    s""""urn:schemas-upnp-org:service:$service:1#$action""""

  /** Extract a named element's text from a SOAP response body, or "" when absent. */
  def field(responseXml: String, name: String): String =
    (XML.loadString(responseXml) \\ name).headOption.map(_.text).getOrElse("")

  /** Parse an escaped XML payload embedded as text (Sonos returns DIDL-Lite and topology this way). */
  def inner(escapedXml: String): Option[Elem] =
    if escapedXml.isBlank || escapedXml == "NOT_IMPLEMENTED" then None
    else scala.util.Try(XML.loadString(escapedXml)).toOption

  /** DIDL-Lite metadata that makes a plain stream URL show a title on the Sonos app. */
  def radioMetadata(title: String): String =
    s"""<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" xmlns:r="urn:schemas-rinconnetworks-com:metadata-1-0/" xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/">
       |<item id="R:0/0/0" parentID="R:0/0" restricted="true"><dc:title>${escape(title)}</dc:title><upnp:class>object.item.audioItem.audioBroadcast</upnp:class>
       |<desc id="cdudn" nameSpace="urn:schemas-rinconnetworks-com:metadata-1-0/">SA_RINCON65031_</desc></item></DIDL-Lite>""".stripMargin

  /** Sonos plays http streams as internet radio when given this scheme. */
  def radioUri(url: String): String =
    url.replaceFirst("^https?://", "x-rincon-mp3radio://")

  def escape(s: String): String =
    s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;")

  /** Text of the first matching element in a node, or "" */
  def text(n: Node, name: String): String = (n \\ name).headOption.map(_.text).getOrElse("")
