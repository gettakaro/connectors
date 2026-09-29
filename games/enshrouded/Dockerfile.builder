# The image the Enshrouded connector is built in.
#
# The plugin (which also holds the Takaro connection) is cross-compiled for Windows with
# zig, so the build never depends on what the host has installed. The base and the zig
# tarball are the catalog target's `build.toolchain` and `build.deps.zig`; the build script
# passes both from the resolved record.
ARG TOOLCHAIN
FROM ${TOOLCHAIN}

# zip is what packages the release archive, xz-utils unpacks zig, git answers the build
# script's revision questions. The base image has none of them.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl xz-utils zip git \
 && rm -rf /var/lib/apt/lists/*

ARG ZIG_URL=https://ziglang.org/download/0.13.0/zig-linux-x86_64-0.13.0.tar.xz
ARG ZIG_SHA256=d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea
RUN set -eu \
 && curl -fsSL "$ZIG_URL" -o /tmp/zig.tar.xz \
 && printf '%s  /tmp/zig.tar.xz\n' "$ZIG_SHA256" | sha256sum -c - \
 && mkdir -p /opt/zig \
 && tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1 \
 && rm -f /tmp/zig.tar.xz \
 && /opt/zig/zig version

ENV ZIG=/opt/zig/zig
WORKDIR /repo
