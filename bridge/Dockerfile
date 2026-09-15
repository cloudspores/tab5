# Runtime image for the Tab5 bridge. The JAR is built on the developer machine (sbt assembly) and
# copied in; the Spark only needs a JRE. Host networking is required for SSDP multicast (Sonos discovery).
FROM eclipse-temurin:21-jre
# ffmpeg encodes the synth relay to MP3 for Sonos
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY target/scala-3.7.4/tab5-bridge.jar /app/tab5-bridge.jar
ENV TAB5_PORT=8765
ENTRYPOINT ["java", "-XX:MaxRAMPercentage=25", "-jar", "/app/tab5-bridge.jar"]
