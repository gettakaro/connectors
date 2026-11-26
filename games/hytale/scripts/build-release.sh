#!/usr/bin/env bash
# Builds the Hytale release jar for one catalog target:
#   takaro-hytale-<target>-<version>.jar, copied by the operator into the server's mods/ folder.
#
#   games/hytale/scripts/build-release.sh <version> <out-dir> [--target <id>]
#
# The plugin is compiled and unit-tested with Maven inside the pinned Temurin 25 image, against
# com.hypixel.hytale:Server from Hypixel's Maven repository (compile scope "provided": it is never
# shipped). The server jar Maven resolved must match the SHA-256 pinned in the catalog target.
# The jar Maven produces is then repacked with fixed timestamps and a sorted entry order, and gets
# the target identity (META-INF/takaro-target.json plus Takaro-* manifest attributes), so the same
# commit always builds the same bytes. Maven's local repository is cached under
# $TAKARO_MAINT_CACHE/hytale/m2. Set MVN to build with a local Maven instead of the pinned image
# (development only).
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "$script_dir/.." && pwd)
repo_root=$(cd -- "$project_root/../.." && pwd)
. "$script_dir/lib-target.sh"

usage="usage: build-release.sh <version> <out-dir> [--target id]"
version="${1:?$usage}"
out_dir="${2:?$usage}"
shift 2
hytale_parse_target_flag "$@"
hytale_resolve_target "$TARGET"
case "$version" in *[!A-Za-z0-9._+-]*|"") echo "invalid version '$version'" >&2; exit 2;; esac
mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
epoch="${SOURCE_DATE_EPOCH:-$(git -C "$repo_root" log -1 --format=%ct)}"
revision="${TAKARO_SOURCE_REVISION:-$(git -C "$repo_root" rev-parse HEAD)}"
stamp_iso=$(date -u -d "@$epoch" +%Y-%m-%dT%H:%M:%SZ)

cache="${TAKARO_MAINT_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/takaro-maint}/hytale"
mkdir -p "$cache/m2"

# The coordinate the catalog pins must be the one the pom compiles against.
server_version="${HYTALE_DEP_HYTALE_SERVER_COORDINATE##*:}"
pom_version=$(sed -n 's:.*<hytale.server.version>\(.*\)</hytale.server.version>.*:\1:p' "$project_root/mod/pom.xml")
if [ "$server_version" != "$pom_version" ]; then
    echo "catalog pins Hytale Server $server_version but mod/pom.xml compiles against $pom_version" >&2
    exit 2
fi

stage="$project_root/_data/build/stage-${HYTALE_FP16}"
rm -rf "$stage"
mkdir -p "$stage"
cp -r "$project_root/mod" "$stage/mod"
rm -rf "$stage/mod/target"
cp -r "$project_root/LICENSE" "$project_root/THIRD-PARTY-NOTICES.md" "$project_root/assets" "$stage/"

# Maven Central rate-limits shared CI runners (HTTP 429): retry with a pause instead of failing.
retry_args=(-Daether.connector.http.retryHandler.count=6 -Daether.connector.http.retryHandler.interval=10000
    -Daether.connector.http.retryHandler.serviceUnavailable=429,503)
mvn_args=(-B -ntp -s ci-settings.xml "${retry_args[@]}" -Drevision="$version" -Dproject.build.outputTimestamp="$stamp_iso" -Dhytale.server.version="$server_version" package)
echo "== takaro-hytale $version (Maven, Hytale Server $server_version)"
if [ -n "${MVN:-}" ]; then
    (cd "$stage/mod" && "$MVN" -Dmaven.repo.local="$cache/m2" "${mvn_args[@]}")
else
    docker run --rm -e OWNER="$(id -u):$(id -g)" -v "$stage:/stage" -v "$cache/m2:/m2" -w /stage/mod \
        "$HYTALE_TOOLCHAIN" bash -c 'trap "chown -R $OWNER /stage /m2" EXIT; mvn -Dmaven.repo.local=/m2 "$@"' \
        mvn "${mvn_args[@]}"
fi

server_jar="$cache/m2/com/hypixel/hytale/Server/$server_version/Server-$server_version.jar"
echo "$HYTALE_DEP_HYTALE_SERVER_SHA256  $server_jar" | sha256sum -c --quiet - \
    || { echo "the Hytale server jar Maven resolved does not match the catalog pin" >&2; exit 1; }

echo "== package"
name="${HYTALE_ARTIFACT_PLUGIN/\{version\}/$version}"
HYTALE_VERSION="$version" HYTALE_EPOCH="$epoch" HYTALE_REVISION_SHA="$revision" \
python3 -I - "$stage/mod/target/takaro-hytale-$version.jar" "$out_dir/$name" <<'PY'
import json, os, sys, time, zipfile
source, dest = sys.argv[1:3]
stamp = time.gmtime(max(int(os.environ["HYTALE_EPOCH"]), 315532800))[:6]
identity = {
    "target": os.environ["HYTALE_TARGET"], "fingerprint": os.environ["HYTALE_FINGERPRINT"],
    "connectorVersion": os.environ["HYTALE_VERSION"], "sourceRevision": os.environ["HYTALE_REVISION_SHA"],
    "game": "hytale", "platform": os.environ["HYTALE_PLATFORM"], "revision": os.environ["HYTALE_REVISION"],
}
manifest = "".join(f"{k}: {v}\r\n" for k, v in (
    ("Manifest-Version", "1.0"),
    ("Implementation-Title", "takaro-hytale"),
    ("Implementation-Version", identity["connectorVersion"]),
    ("Takaro-Target", identity["target"]),
    ("Takaro-Target-Fingerprint", identity["fingerprint"]),
    ("Takaro-Connector-Version", identity["connectorVersion"]),
    ("Takaro-Source-Revision", identity["sourceRevision"]),
    ("Takaro-Game-Version", identity["revision"]),
)) + "\r\n"

with zipfile.ZipFile(source) as built:
    entries = {i.filename: built.read(i) for i in built.infolist() if not i.is_dir()}
entries.pop("META-INF/MANIFEST.MF", None)
for required in ("manifest.json", "dev/takaro/hytale/TakaroPlugin.class", "dev/takaro/hytale/version.properties",
                 "META-INF/LICENSE", "META-INF/THIRD-PARTY-NOTICES.md", "dev/takaro/hytale/icon-256.png"):
    if required not in entries:
        sys.exit(f"the built jar has no {required}")
if json.loads(entries["manifest.json"])["Version"] != identity["connectorVersion"]:
    sys.exit("manifest.json does not carry the release version")
leaked = sorted(n for n in entries if n.startswith("com/hypixel/"))
if leaked:
    sys.exit(f"refusing to ship Hytale server classes: {leaked[:3]}")

def write(archive, name, data):
    info = zipfile.ZipInfo(name, stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    archive.writestr(info, data)

tmp = dest + ".part"
with zipfile.ZipFile(tmp, "w") as archive:
    write(archive, "META-INF/MANIFEST.MF", manifest.encode())
    write(archive, "META-INF/takaro-target.json", (json.dumps(identity, sort_keys=True) + "\n").encode())
    for name in sorted(entries):
        write(archive, name, entries[name])
os.replace(tmp, dest)
print(f"{os.path.basename(dest)}: {len(entries) + 2} entries")
PY
echo "$out_dir/$name"
