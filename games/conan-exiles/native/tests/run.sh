#!/usr/bin/env bash
# Every host test. Default runs inside the buster build container (via platform/linux/build.sh
# --tests); --native uses the toolchain of the current machine (the container calls it that way).
#   unit_test     core logic (protocol, config, outbox, heartbeat, pins, adapter, registry, text)
#   l2b_test      the mutation actions against a fake game and against fake UE objects built from
#                 the reflection-dump fixture (tests/fixtures/l2b-reflection-25639945.json)
#   pins_oracle   the signature scan over the real 25639945 binary (CONAN_SERVER_BINARY; skipped
#                 when not given); pins_oracle_pe the same over the Windows exe (CONAN_SERVER_BINARY_WIN)
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
CORE=(core/*.cpp core/*/*.cpp platform/linux/fileio_posix.cpp platform/linux/elfscan.cpp platform/linux/selfmem_posix.cpp)
LWS=("$PREFIX/lib/libwebsockets.a" "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a" -ldl)
mkdir -p tests/build
[ -f dist/libtakaro-conan-native.so ] || { echo "build the library first (platform/linux/build.sh)" >&2; exit 1; }

echo "== unit_test"
"$CXX" "${FLAGS[@]}" tests/unit_test.cpp "${CORE[@]}" -o tests/build/unit_test
./tests/build/unit_test

echo "== reads_test"
"$CXX" "${FLAGS[@]}" tests/reads_test.cpp "${CORE[@]}" -o tests/build/reads_test
./tests/build/reads_test
echo "== l2b_test (mutations: fake game + fake UE objects from the 25639945 dump fixture)"
"$CXX" "${FLAGS[@]}" tests/l2b_test.cpp "${CORE[@]}" -o tests/build/l2b_test
./tests/build/l2b_test

echo "== pins_oracle"
"$CXX" "${FLAGS[@]}" -O2 tests/pins_oracle.cpp core/pins/pins.cpp core/common.cpp platform/linux/elfscan.cpp \
    -o tests/build/pins_oracle
if [ -n "${CONAN_SERVER_BINARY:-}" ]; then
  ./tests/build/pins_oracle "$CONAN_SERVER_BINARY"
else
  echo "SKIP pins_oracle: set CONAN_SERVER_BINARY to a 25639945 ConanSandboxServer-Linux-Shipping"
fi

echo "== pins_oracle_pe"
"$CXX" "${FLAGS[@]}" -O2 tests/pins_oracle_pe.cpp core/pins/pins.cpp core/common.cpp -o tests/build/pins_oracle_pe
if [ -n "${CONAN_SERVER_BINARY_WIN:-}" ]; then
  ./tests/build/pins_oracle_pe "$CONAN_SERVER_BINARY_WIN"
else
  echo "SKIP pins_oracle_pe: set CONAN_SERVER_BINARY_WIN to a 25639945 ConanSandboxServer-Win64-Shipping.exe"
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
