#!/usr/bin/env bash
# Build the bridge JAR locally and (re)deploy it as a container on the Spark.
#   ./deploy.sh            -> spark-4dfb.local
#   ./deploy.sh other.host
set -euo pipefail
cd "$(dirname "$0")"
HOST="${1:-spark-4dfb.local}"
REMOTE_DIR="tab5-bridge"

echo "== building JAR"
sbt -batch assembly | grep -E "error|Assembly|success" || true
test -f target/scala-3.7.4/tab5-bridge.jar

echo "== copying to $HOST:$REMOTE_DIR"
ssh "$HOST" "mkdir -p $REMOTE_DIR/target/scala-3.7.4"
scp -q Dockerfile compose.yaml whisper-server.service "$HOST:$REMOTE_DIR/"
scp -q target/scala-3.7.4/tab5-bridge.jar "$HOST:$REMOTE_DIR/target/scala-3.7.4/"

echo "== starting container"
ssh "$HOST" "cd $REMOTE_DIR && docker compose up -d --build 2>&1 | tail -3"
sleep 4
echo "== health"
curl -s -m 5 "http://$HOST:8765/health" && echo
