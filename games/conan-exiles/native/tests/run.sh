#!/usr/bin/env bash
# Every host test. Default runs inside the buster build container (via platform/linux/build.sh
# --tests); --native uses the toolchain of the current machine (the container calls it that way).
#   events_test   lane L2c: event payloads + whitelist, log tail, hook dispatch table
#   unit_test     core logic (protocol, config, outbox, heartbeat, pins, adapter, registry, text)
#   pins_oracle   the signature scan over the real 25639945 binary (CONAN_SERVER_BINARY; skipped
#                 when not given)
#   drift_test    capabilities.json / pins.json equal the compiled tables
#   wire_test     the production Takaro half against a fake Takaro (TLS WebSocket), incl. a 20 s outage
#   so_test       the real dist/libtakaro-conan-native.so preloaded into a stand-in server
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "${1:-}" != "--native" ]; then
  exec platform/linux/build.sh --tests
fi
CXX=${CXX:-g++}
PREFIX=${TAKARO_NATIVE_PREFIX:-/opt/takaro-native}
FLAGS=(-std=c++17 -O1 -g -Wall -Wextra -Werror -Icore -Iplatform/linux "-I$PREFIX/include" -pthread)
CORE=(core/*.cpp core/*/*.cpp platform/linux/fileio_posix.cpp platform/linux/elfscan.cpp)
LWS=("$PREFIX/lib/libwebsockets.a" "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" -ldl)
mkdir -p tests/build
[ -f dist/libtakaro-conan-native.so ] || { echo "build the library first (platform/linux/build.sh)" >&2; exit 1; }

echo "== unit_test"
"$CXX" "${FLAGS[@]}" tests/unit_test.cpp "${CORE[@]}" -o tests/build/unit_test
./tests/build/unit_test

echo "== events_test"
"$CXX" "${FLAGS[@]}" tests/events_test.cpp "${CORE[@]}" -o tests/build/events_test
./tests/build/events_test

echo "== pins_oracle"
"$CXX" "${FLAGS[@]}" -O2 tests/pins_oracle.cpp core/pins/pins.cpp core/common.cpp platform/linux/elfscan.cpp \
    -o tests/build/pins_oracle
if [ -n "${CONAN_SERVER_BINARY:-}" ]; then
  ./tests/build/pins_oracle "$CONAN_SERVER_BINARY"
else
  echo "SKIP pins_oracle: set CONAN_SERVER_BINARY to a 25639945 ConanSandboxServer-Linux-Shipping"
fi

echo "== harness + drift_test"
"$CXX" "${FLAGS[@]}" tests/harness.cpp "${CORE[@]}" platform/linux/transport_lws.cpp "${LWS[@]}" \
    -o tests/build/harness
python3 tests/drift_test.py tests/build/harness

echo "== wire_test"
python3 tests/wire_test.py tests/build/harness

echo "== so_test"
gcc -O1 -Wall -Werror tests/fake_server.c -o tests/build/fake_server
python3 tests/so_test.py dist/libtakaro-conan-native.so tests/build/fake_server
echo "ALL TESTS PASSED"
