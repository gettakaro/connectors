#!/usr/bin/env bash
# Package the exact-hash native connector and its Generic sidecar for one catalog target.
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
mkdir -p "$stage/TakaroArkNative" "$stage/TakaroArkSidecar"

# The catalog-pinned Node Bookworm image includes g++; both outputs use one immutable
# toolchain rather than whatever compiler or npm happens to be installed on the host.
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -e npm_config_cache=/tmp/npm-cache -v "$repo_root:$repo_root" \
  -w "$repo_root" "$ARK_TOOLCHAIN" bash -lc '
    set -euo pipefail
    g++ -std=c++20 -O2 -fPIC -shared -pthread games/ark/mod/src/native.cpp \
      -o games/ark/_data/build/libtakaro-ark-native.so -ldl
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/gate_http_test.cpp \
      -o games/ark/_data/build/gate_http_test
    games/ark/_data/build/gate_http_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/inventory_bindings_test.cpp \
      -o games/ark/_data/build/inventory_bindings_test
    games/ark/_data/build/inventory_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/action_bindings_test.cpp \
      -o games/ark/_data/build/action_bindings_test
    games/ark/_data/build/action_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/death_bindings_test.cpp \
      -o games/ark/_data/build/death_bindings_test
    games/ark/_data/build/death_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/console_bindings_test.cpp \
      -o games/ark/_data/build/console_bindings_test
    games/ark/_data/build/console_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/catalog_bindings_test.cpp \
      -o games/ark/_data/build/catalog_bindings_test
    games/ark/_data/build/catalog_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/moderation_bindings_test.cpp \
      -o games/ark/_data/build/moderation_bindings_test
    games/ark/_data/build/moderation_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/list_bans_bindings_test.cpp \
      -o games/ark/_data/build/list_bans_bindings_test
    games/ark/_data/build/list_bans_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/entity_bindings_test.cpp \
      -o games/ark/_data/build/entity_bindings_test
    games/ark/_data/build/entity_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/engine_exec_bindings_test.cpp \
      -o games/ark/_data/build/engine_exec_bindings_test
    games/ark/_data/build/engine_exec_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/engine_exec_capture_test.cpp \
      -o games/ark/_data/build/engine_exec_capture_test
    games/ark/_data/build/engine_exec_capture_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/log_tail_test.cpp \
      -o games/ark/_data/build/log_tail_test
    games/ark/_data/build/log_tail_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/save_bindings_test.cpp \
      -o games/ark/_data/build/save_bindings_test
    games/ark/_data/build/save_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/location_bindings_test.cpp \
      -o games/ark/_data/build/location_bindings_test
    games/ark/_data/build/location_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/world_bootstrap_test.cpp \
      -o games/ark/_data/build/world_bootstrap_test
    games/ark/_data/build/world_bootstrap_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/general_console_bindings_test.cpp \
      -o games/ark/_data/build/general_console_bindings_test
    games/ark/_data/build/general_console_bindings_test
    g++ -std=c++20 -Wall -Wextra -Werror -pthread games/ark/mod/tests/shutdown_request_bindings_test.cpp \
      -o games/ark/_data/build/shutdown_request_bindings_test
    games/ark/_data/build/shutdown_request_bindings_test
    cd games/ark/sidecar
    npm ci --no-audit --no-fund
    npm run typecheck
    npm test
    npm run build'

cp "$project_root/_data/build/libtakaro-ark-native.so" "$stage/TakaroArkNative/"
sed "s/@EXPECTED_SHA256@/${ARK_SERVER_EXE_SHA256}/g" \
  "$script_dir/launch-exact.sh.in" > "$stage/TakaroArkNative/launch.sh"
cp -R "$project_root/sidecar/dist" "$stage/TakaroArkSidecar/dist"
cp "$project_root/sidecar/package.json" "$project_root/sidecar/package-lock.json" \
  "$stage/TakaroArkSidecar/"
cp "$project_root/sidecar/Dockerfile.release" "$stage/TakaroArkSidecar/Dockerfile"
cp "$project_root/sidecar/.env.example" "$stage/TakaroArkSidecar/env.example"
cat > "$stage/TakaroArkNative/README.txt" <<EOF
Takaro ARK native ${version}; exact ShooterGameServer build ${ARK_REVISION} only.
Start the dedicated game with: ./launch.sh /path/to/ark/install 'TheIsland?listen?...' -server -log.
The launcher verifies the exact executable hash, sets LD_PRELOAD only for ShooterGameServer,
and preserves every supplied server argument.
Set a long random ARK_NATIVE_TOKEN, shared with the sidecar. Never preload SteamCMD.
The native library checks the executable SHA-256 and refuses unknown builds.
EOF
cat > "$stage/TakaroArkNative/uninstall-manifest.json" <<EOF
{"ownedPaths":["TakaroArk/TakaroArkNative"],"preservePaths":["ShooterGame/Saved",".takaro","TakaroArk/TakaroArkSidecar"]}
EOF
cat > "$stage/TakaroArkSidecar/README.txt" <<EOF
Takaro ARK sidecar ${version}; run beside the game in its loopback network namespace.
Set TAKARO_REGISTRATION_TOKEN, TAKARO_IDENTITY_TOKEN and ARK_NATIVE_TOKEN.
Persist the cursor directory; run npm ci --omit=dev and node dist/index.js.
EOF
cat > "$stage/TakaroArkSidecar/uninstall-manifest.json" <<EOF
{"ownedPaths":["TakaroArk/TakaroArkSidecar"],"preservePaths":["ShooterGame/Saved",".takaro","TakaroArk/TakaroArkNative"]}
EOF
for role in Native Sidecar; do
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
done

for spec in "Native:${ARK_ARTIFACT_SERVER_PLUGIN}" "Sidecar:${ARK_ARTIFACT_SIDECAR}"; do
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
done
