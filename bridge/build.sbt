// Tab5 bridge: the LAN service the Tab5 (and the knobs) talk to. Runs on the DGX Spark.
// Scala 3 + ZIO 2 (zio-http server/client, zio-json). Builds to one JAR (sbt assembly).

val zioVersion = "2.1.24"

ThisBuild / scalaVersion := "3.7.4"
ThisBuild / organization := "tab5"
ThisBuild / version      := "0.1.0"

lazy val bridge = (project in file("."))
  .settings(
    name := "tab5-bridge",
    Compile / mainClass := Some("tab5.bridge.Main"),   // tools.LiveClient is a second entry point
    scalacOptions ++= Seq(
      "-deprecation", "-feature", "-unchecked",
      "-Wunused:all", "-Wvalue-discard", "-Wnonunit-statement",
      "-Xfatal-warnings"
    ),
    libraryDependencies ++= Seq(
      "dev.zio" %% "zio"          % zioVersion,
      "dev.zio" %% "zio-streams"  % zioVersion,
      "dev.zio" %% "zio-http"     % "3.5.1",
      "dev.zio" %% "zio-json"     % "0.7.45",
      "dev.zio" %% "zio-logging"       % "2.5.1",
      "dev.zio" %% "zio-logging-slf4j" % "2.5.1",
      "ch.qos.logback" % "logback-classic" % "1.5.18",
      "dev.zio" %% "zio-config"          % "4.0.5",
      "dev.zio" %% "zio-config-typesafe" % "4.0.5",
      "dev.zio" %% "zio-config-magnolia" % "4.0.5",
      "org.scala-lang.modules" %% "scala-xml" % "2.3.0",
      "dev.zio" %% "zio-test"     % zioVersion % Test,
      "dev.zio" %% "zio-test-sbt" % zioVersion % Test
    ),
    testFrameworks += new TestFramework("zio.test.sbt.ZTestFramework"),
    assembly / assemblyJarName := "tab5-bridge.jar",
    assembly / assemblyMergeStrategy := {
      case PathList("META-INF", "io.netty.versions.properties") => MergeStrategy.first
      case PathList("META-INF", xs @ _*) if xs.lastOption.exists(n => n.endsWith(".SF") || n.endsWith(".DSA") || n.endsWith(".RSA")) => MergeStrategy.discard
      case PathList("META-INF", "MANIFEST.MF") => MergeStrategy.discard
      case "module-info.class" => MergeStrategy.discard
      case x => MergeStrategy.first
    }
  )
