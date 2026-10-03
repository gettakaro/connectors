#!/usr/bin/env bash
# Linux build entry point kept at this path for scripts/build-release.sh; the build lives in
# platform/linux/build.sh (buster toolchain, glibc <= 2.28 guard).
exec "$(dirname "$0")/platform/linux/build.sh" "$@"
