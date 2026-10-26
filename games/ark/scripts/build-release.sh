#!/usr/bin/env bash
# Package the exact-hash native connector (it holds the Takaro WebSocket itself) for one catalog target.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"
version="${1:?usage: build-release.sh <version> <out-dir> [--target id]}"
out_dir="${2:?usage: build-release.sh <version> <out-dir> [--target id]}"
shift 2
ark_parse_target_flag "$@"
ark_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
stage="$project_root/_data/build/stage-${ARK_FP16}"
rm -rf "$stage"
mkdir -p "$stage/TakaroArkNative"

# The catalog-pinned Bookworm image is the one immutable toolchain: its g++ and its
# static OpenSSL (libssl-dev 3.0.x) are linked into the plugin, so the artifact does
# not depend on the host's libssl/libcrypto. Unit and contract tests run first.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e SOURCE_DATE_EPOCH="$epoch" \
  -e SSL_SHA256="${ARK_DEP_LIBSSL_A_SHA256:?}" -e CRYPTO_SHA256="${ARK_DEP_LIBCRYPTO_A_SHA256:?}" \
  -v "$repo_root:$repo_root" -w "$repo_root" "$ARK_TOOLCHAIN" bash -lc '
    set -euo pipefail
    # The cataloged static OpenSSL archives, byte for byte.
    printf "%s  %s\n" "$SSL_SHA256" /usr/lib/x86_64-linux-gnu/libssl.a \
      "$CRYPTO_SHA256" /usr/lib/x86_64-linux-gnu/libcrypto.a | sha256sum -c --quiet
    out=games/ark/_data/build
    mkdir -p "$out"
    ssl=(-Wl,-Bstatic -lssl -lcrypto -Wl,-Bdynamic)
    for test in games/ark/mod/tests/*_test.cpp; do
      name=$(basename "${test%.cpp}")
      g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
        "$test" -o "$out/$name" "${ssl[@]}" -ldl
      "$out/$name"
    done
    g++ -std=c++20 -O2 -fPIC -shared -pthread -Wall -Wextra -Werror \
      -fvisibility=hidden -fvisibility-inlines-hidden -fno-gnu-unique \
      -ffile-prefix-map="$PWD"=. -Wl,--build-id=none \
      games/ark/mod/src/native.cpp -o "$out/libtakaro-ark-native.so" \
      "${ssl[@]}" -Wl,--exclude-libs,ALL -Wl,--version-script=games/ark/mod/exports.map -ldl
    # Only the Steam interposer may be exported, and LD_PRELOAD must not see an
    # unresolvable strong symbol (it would fail the whole game process).
    exported=$(nm -D --defined-only "$out/libtakaro-ark-native.so" | awk "{print \$3}" | grep -vx "_init\|_fini" || true)
    if [ "$exported" != "SteamGameServer_RunCallbacks" ]; then
      echo "unexpected exported symbols: $exported" >&2; exit 1
    fi
    undefined=$(nm -D --undefined-only "$out/libtakaro-ark-native.so" | awk "\$1 == \"U\" {print \$2}" | grep -vE "@(GLIBC|GCC|CXXABI|GLIBCXX)_" || true)
    if [ -n "$undefined" ]; then
      echo "undefined non-runtime symbols: $undefined" >&2; exit 1
    fi'

cp "$project_root/_data/build/libtakaro-ark-native.so" "$stage/TakaroArkNative/"
sed "s/@EXPECTED_SHA256@/${ARK_SERVER_EXE_SHA256}/g" \
  "$script_dir/launch-exact.sh.in" > "$stage/TakaroArkNative/launch.sh"
cp "$project_root/mod/takaro.cfg.example" "$stage/TakaroArkNative/takaro.cfg.example"
cp "$project_root/assets/icon-256.png" "$stage/TakaroArkNative/icon-256.png"
cat > "$stage/TakaroArkNative/README.txt" <<EOF
Takaro ARK native ${version}; exact ShooterGameServer build ${ARK_REVISION} only.
Copy takaro.cfg.example to takaro.cfg in this folder and set TAKARO_REGISTRATION_TOKEN.
Start the dedicated game with: ./launch.sh /path/to/ark/install 'TheIsland?listen?...' -server -log.
The launcher verifies the exact executable hash, sets LD_PRELOAD only for ShooterGameServer,
and preserves every supplied server argument. The plugin connects to Takaro itself
(outbound WebSocket, no inbound port, no sidecar). Never preload SteamCMD.
Connector state (identity, ban metadata, cursor, status.json) lives in
<ark install>/.takaro/ark-native unless TAKARO_STATE_DIR is set.
EOF
cat > "$stage/TakaroArkNative/uninstall-manifest.json" <<EOF
{"ownedPaths":["TakaroArk/TakaroArkNative"],"preservePaths":["ShooterGame/Saved",".takaro","TakaroArk/TakaroArkNative/takaro.cfg"]}
EOF
role=Native
  folder="TakaroArk${role}"
  ARK_STAGE_FOLDER="$stage/$folder" ARK_CONNECTOR_VERSION="$version" \
  ARK_SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}" \
  python3 - <<'PY'
import json, os
from pathlib import Path
folder=Path(os.environ['ARK_STAGE_FOLDER'])
record={'target':os.environ['ARK_TARGET'],'fingerprint':os.environ['ARK_FINGERPRINT'],
        'connectorVersion':os.environ['ARK_CONNECTOR_VERSION'],
        'sourceRevision':os.environ['ARK_SOURCE_REVISION'],'game':'ark',
        'platform':'linux','revision':os.environ['ARK_REVISION']}
(folder/'takaro-target.json').write_text(json.dumps(record,sort_keys=True)+'\n')
PY

spec="Native:${ARK_ARTIFACT_SERVER_PLUGIN}"
  role="${spec%%:*}"
  name="${spec#*:}"
  name="${name/\{version\}/$version}"
  ARK_ZIP_STAGE="$stage" ARK_ZIP_FOLDER="TakaroArk${role}" ARK_ZIP_OUT="$out_dir/$name" \
    ARK_ZIP_EPOCH="$epoch" python3 - <<'PY'
import os, time, zipfile
from pathlib import Path
stage=Path(os.environ['ARK_ZIP_STAGE']); folder=os.environ['ARK_ZIP_FOLDER']
stamp=time.gmtime(max(int(os.environ['ARK_ZIP_EPOCH']),315532800))[:6]
with zipfile.ZipFile(os.environ['ARK_ZIP_OUT'],'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for path in sorted((stage/folder).rglob('*')):
        if not path.is_file(): continue
        entry=zipfile.ZipInfo(path.relative_to(stage).as_posix(),stamp)
        entry.compress_type=zipfile.ZIP_DEFLATED
        permissions = 0o755 if path.name == 'launch.sh' and folder == 'TakaroArkNative' else 0o644
        entry.external_attr=((0o100000 | permissions) << 16)
        archive.writestr(entry,path.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
PY
  ARK_META_PATH="$out_dir/$name.meta.json" ARK_CONNECTOR_VERSION="$version" \
    ARK_SOURCE_REVISION="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}" python3 - <<'PY'
import json,os
from pathlib import Path
Path(os.environ['ARK_META_PATH']).write_text(json.dumps({
  'target':os.environ['ARK_TARGET'],'fingerprint':os.environ['ARK_FINGERPRINT'],
  'connectorVersion':os.environ['ARK_CONNECTOR_VERSION'],
  'sourceRevision':os.environ['ARK_SOURCE_REVISION'],
  'game':'ark','platform':'linux','revision':os.environ['ARK_REVISION']},sort_keys=True)+'\n')
PY
  sha256sum "$out_dir/$name"
