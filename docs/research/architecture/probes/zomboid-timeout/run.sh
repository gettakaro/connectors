#!/usr/bin/env bash
# Research probe for #298 (ZOM-2). Compiles the production MainThreadQueue with a probe main in a
# JDK container and runs it. Off by default: not referenced by Gradle, CI or the release.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../../../.." && pwd)"
docker run --rm -v "$repo":/repo -w /repo -u "$(id -u):$(id -g)" eclipse-temurin:25-jdk bash -c '
  set -e; src=games/zomboid/mod/agent/src/main/java/io/takaro/zomboid/agent
  mkdir -p /tmp/c && javac -d /tmp/c $src/MainThreadQueue.java $src/AgentLog.java $src/TakaroPaths.java \
    docs/research/architecture/probes/zomboid-timeout/TimeoutProbe.java && java -cp /tmp/c TimeoutProbe'
