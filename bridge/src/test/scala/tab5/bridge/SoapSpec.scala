package tab5.bridge

import zio.test.*

object SoapSpec extends ZIOSpecDefault:
  def spec = suite("Soap")(
    test("radio URIs use the Sonos internet-radio scheme") {
      assertTrue(
        Soap.radioUri("http://ice1.somafm.com/groovesalad-128-mp3") == "x-rincon-mp3radio://ice1.somafm.com/groovesalad-128-mp3",
        Soap.radioUri("https://stream.example/x") == "x-rincon-mp3radio://stream.example/x"
      )
    },
    test("envelope carries the action, service and escaped arguments") {
      val e = Soap.envelope("AVTransport", "SetAVTransportURI", Seq("InstanceID" -> "0", "CurrentURI" -> "a&b"))
      assertTrue(
        e.contains("<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"),
        e.contains("<CurrentURI>a&amp;b</CurrentURI>"),
        Soap.actionHeader("AVTransport", "Play") == "\"urn:schemas-upnp-org:service:AVTransport:1#Play\""
      )
    },
    test("fields are read from a SOAP response and escaped payloads are parsed") {
      val resp =
        """<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>
          |<u:GetTransportInfoResponse xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"><CurrentTransportState>PLAYING</CurrentTransportState></u:GetTransportInfoResponse>
          |</s:Body></s:Envelope>""".stripMargin
      val didl = "&lt;DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" xmlns:r=\"urn:schemas-rinconnetworks-com:metadata-1-0/\"&gt;&lt;item&gt;&lt;r:streamContent&gt;Artist - Song&lt;/r:streamContent&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;"
      val unescaped = didl.replace("&lt;", "<").replace("&gt;", ">")
      assertTrue(
        Soap.field(resp, "CurrentTransportState") == "PLAYING",
        Soap.inner(unescaped).map(x => Soap.text(x, "streamContent")).contains("Artist - Song"),
        Soap.inner("NOT_IMPLEMENTED").isEmpty
      )
    }
  )
