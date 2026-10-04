# The image the Windows native connector (winmm.dll) is released from.
#
# The base and the zig tarball are the windows catalog target's `build.toolchain` and
# `build.deps.zig`; scripts/build-release.sh passes both from the resolved record. zip packages the
# release archive, xz-utils unpacks zig, python3 runs the export and timestamp check in build.sh.
ARG TOOLCHAIN
FROM ${TOOLCHAIN}

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl xz-utils zip python3 \
 && rm -rf /var/lib/apt/lists/*

ARG ZIG_URL
ARG ZIG_SHA256
RUN set -eu \
 && curl -fsSL --retry 3 "$ZIG_URL" -o /tmp/zig.tar.xz \
 && printf '%s  /tmp/zig.tar.xz\n' "$ZIG_SHA256" | sha256sum -c - \
 && mkdir -p /opt/zig \
 && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
 && rm -f /tmp/zig.tar.xz \
 && /opt/zig/zig version

ENV ZIG=/opt/zig/zig
