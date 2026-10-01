# The image the Dragonwilds release is built in.
#
# The base is the catalog target's `build.toolchain` (a Debian Bookworm image, so the plugin
# links against the same glibc as the dedicated server). The four libraries the plugin links
# statically are the target's `build.deps`: the build script passes each URL and SHA-256 from
# the resolved record, and every archive is checked before it is unpacked.
ARG TOOLCHAIN
FROM ${TOOLCHAIN}

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      binutils build-essential bzip2 ca-certificates cmake curl git make openssl perl pkg-config python3 xz-utils \
 && rm -rf /var/lib/apt/lists/*

ARG OPENSSL_URL
ARG OPENSSL_SHA256
ARG LIBWEBSOCKETS_URL
ARG LIBWEBSOCKETS_SHA256
ARG PCRE2_URL
ARG PCRE2_SHA256
ARG NLOHMANN_JSON_URL
ARG NLOHMANN_JSON_SHA256
ARG PREFIX=/opt/takaro-native
WORKDIR /tmp/sources

RUN set -eu \
 && curl -fsSL --retry 3 -o openssl.tar.gz "$OPENSSL_URL" \
 && printf '%s  openssl.tar.gz\n' "$OPENSSL_SHA256" | sha256sum -c - \
 && mkdir openssl && tar -xzf openssl.tar.gz -C openssl --strip-components=1 \
 && cd openssl \
 && CFLAGS='-O2 -fPIC -fvisibility=hidden' ./Configure linux-x86_64 no-shared no-tests no-module \
      --prefix="$PREFIX" --libdir=lib \
 && make -j4 build_sw && make install_sw \
 && cd .. && rm -rf openssl openssl.tar.gz

RUN set -eu \
 && curl -fsSL --retry 3 -o lws.tar.gz "$LIBWEBSOCKETS_URL" \
 && printf '%s  lws.tar.gz\n' "$LIBWEBSOCKETS_SHA256" | sha256sum -c - \
 && mkdir lws && tar -xzf lws.tar.gz -C lws --strip-components=1 \
 && cmake -S lws -B lws-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_C_FLAGS='-fvisibility=hidden' \
      -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=TRUE \
      -DLWS_WITH_SHARED=OFF -DLWS_WITH_STATIC=ON -DLWS_WITH_SERVER=OFF \
      -DLWS_WITH_HTTP2=OFF -DLWS_WITH_ZLIB=OFF -DLWS_WITHOUT_EXTENSIONS=ON \
      -DLWS_WITHOUT_TESTAPPS=ON \
 && cmake --build lws-build -j4 && cmake --install lws-build \
 && rm -rf lws lws-build lws.tar.gz

RUN set -eu \
 && curl -fsSL --retry 3 -o pcre2.tar.bz2 "$PCRE2_URL" \
 && printf '%s  pcre2.tar.bz2\n' "$PCRE2_SHA256" | sha256sum -c - \
 && mkdir pcre2 && tar -xjf pcre2.tar.bz2 -C pcre2 --strip-components=1 \
 && cd pcre2 \
 && CFLAGS='-O2 -fPIC -fvisibility=hidden' ./configure --disable-shared --enable-static --prefix="$PREFIX" \
 && make -j4 && make install \
 && cd .. && rm -rf pcre2 pcre2.tar.bz2

RUN set -eu \
 && curl -fsSL --retry 3 -o json.tar.xz "$NLOHMANN_JSON_URL" \
 && printf '%s  json.tar.xz\n' "$NLOHMANN_JSON_SHA256" | sha256sum -c - \
 && tar -xJf json.tar.xz \
 && mkdir -p "$PREFIX/include" && cp -a json/include/nlohmann "$PREFIX/include/" \
 && rm -rf json json.tar.xz

ENV TAKARO_NATIVE_PREFIX=${PREFIX}
WORKDIR /repo
