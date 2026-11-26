#!/usr/bin/env bash
# Runs the plugin's unit tests (Maven, pinned image) against the default catalog target.
#   games/hytale/scripts/test.sh [--target <id>]
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
. "$script_dir/lib-target.sh"
hytale_parse_target_flag "$@"
hytale_resolve_target "$TARGET"
cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/hytale"
mkdir -p "$cache/m2"
server_version="${HYTALE_DEP_HYTALE_SERVER_COORDINATE##*:}"
stage="$project_root/_data/build/test-${HYTALE_FP16}"
rm -rf "$stage"
mkdir -p "$stage"
cp -r "$project_root/mod" "$stage/mod"
rm -rf "$stage/mod/target"
cp -r "$project_root/LICENSE" "$project_root/THIRD-PARTY-NOTICES.md" "$project_root/assets" "$stage/"
# Maven Central rate-limits shared CI runners (HTTP 429): retry with a pause instead of failing.
retry_args=(-Daether.connector.http.retryHandler.count=6 -Daether.connector.http.retryHandler.interval=10000
    -Daether.connector.http.retryHandler.serviceUnavailable=429,503)
args=(-B -ntp -s ci-settings.xml "${retry_args[@]}" -Dhytale.server.version="$server_version" test)
if [ -n "${MVN:-}" ]; then
    (cd "$stage/mod" && "$MVN" -Dmaven.repo.local="$cache/m2" "${args[@]}")
else
    docker run --rm -e OWNER="$(id -u):$(id -g)" -v "$stage:/stage" -v "$cache/m2:/m2" -w /stage/mod \
        "$HYTALE_TOOLCHAIN" bash -c 'trap "chown -R $OWNER /stage /m2" EXIT; mvn -Dmaven.repo.local=/m2 "$@"' \
        mvn "${args[@]}"
fi
server_jar="$cache/m2/com/hypixel/hytale/Server/$server_version/Server-$server_version.jar"
echo "$HYTALE_DEP_HYTALE_SERVER_SHA256  $server_jar" | sha256sum -c --quiet - \
    || { echo "the Hytale server jar Maven resolved does not match the catalog pin" >&2; exit 1; }
