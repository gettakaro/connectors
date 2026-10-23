#!/usr/bin/env bash
# Research probe for #298 (C7 Terraria transport gate). Builds with the .NET 9 SDK image and runs
# on the cached TShock image's runtime. Off by default: no connector build or CI references it.
# Usage: run.sh [wss-url] (default: the Takaro staging connector endpoint)
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="$(mktemp -d)"
tshock="${TSHOCK_IMAGE:-ghcr.io/pryaxis/tshock@sha256:70e59a8e6b4c79b5fad469d320962c1fc98625ed3a955510a2f2641dbff2f7e7}"
docker run --rm -v "$here":/src:ro -v "$out":/out -u "$(id -u):$(id -g)" -e HOME=/tmp -e DOTNET_CLI_TELEMETRY_OPTOUT=1 \
  mcr.microsoft.com/dotnet/sdk:9.0 bash -c 'cp -r /src /tmp/p && cd /tmp/p && dotnet publish -c Release -o /out -v q --nologo' >/dev/null || exit 2
docker run --rm -v "$out":/probe:ro --entrypoint dotnet "$tshock" /probe/WssProbe.dll "$@"
