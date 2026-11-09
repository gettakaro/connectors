#!/usr/bin/env bash
# Prepares everything the plugin is compile-checked against, for one catalog target:
# the pinned server assemblies (from the pinned Steam depot manifests, the Managed subset
# only) and the pinned Carbon assemblies (from the pinned release asset, by sha256).
#
# Usage: setup-environment.sh [--target <catalog target id>] [--force]
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(cd -- "${SCRIPT_DIR}/.." && pwd)
# shellcheck source=lib-target.sh
. "${SCRIPT_DIR}/lib-target.sh"

rust_parse_target_flag "$@"
rust_resolve_target "${TARGET}"

cd "${PROJECT_ROOT}"
echo "Setting up the Rust build environment for ${RUST_TARGET} (${RUST_FP16})..."

GAME_REFS="./_data/rust-binaries/${RUST_REFERENCES_KEY}"
CARBON_REFS="./_data/carbon-refs/${RUST_FP16}"
mkdir -p ./_data

# Exactly the assemblies build.references selects, from the pinned depot manifests.
# Never the whole 6 GB depot set, and never a branch head.
"${TAKARO_MAINT}" steam references \
    --game rust --target "${RUST_TARGET}" --dest "${GAME_REFS}" --reuse-by-manifests ${FORCE:+--force}

# The one assembly whose hash decides whether this plugin can be built at all.
printf '%s  %s\n' "${RUST_ASSEMBLY_CSHARP_SHA256}" "${GAME_REFS}/Assembly-CSharp.dll" \
    | sha256sum --check --status \
    || { echo "error: ${GAME_REFS}/Assembly-CSharp.dll is not the one ${RUST_TARGET} pins" >&2; exit 5; }

# Carbon's own assemblies, from the pinned release asset. A directory left over from
# another fingerprint is refused rather than compiled against, and so is one whose
# assemblies no longer hash to what the record beside them says.
MARKER="${CARBON_REFS}/.takaro/carbon-references.json"
if [ -f "${MARKER}" ] && [ -z "${FORCE}" ]; then
    CARBON_REFS="${CARBON_REFS}" RUST_FINGERPRINT="${RUST_FINGERPRINT}" \
        python3 - "${MARKER}" <<'CHECK'
import hashlib, json, os, pathlib, sys

marker = pathlib.Path(sys.argv[1])
root = pathlib.Path(os.environ["CARBON_REFS"])
wanted = os.environ["RUST_FINGERPRINT"]
try:
    record = json.loads(marker.read_text(encoding="utf-8"))
except (OSError, ValueError) as exc:
    print(f"error: {marker} is not a readable Carbon reference record ({exc})", file=sys.stderr)
    print("       Corrupt Carbon reference cache. Pass --force to replace it.", file=sys.stderr)
    raise SystemExit(5)

recorded = str(record.get("fingerprint") or "")
if recorded != wanted:
    print(f"error: {root} holds the Carbon references for {recorded[:16]}; this target is {wanted[:16]}.", file=sys.stderr)
    print("       Stale Carbon reference cache. Pass --force to replace it.", file=sys.stderr)
    raise SystemExit(7)

# The record names every assembly the compile links against, with its sha256 and size, so the
# cache is trusted only once the bytes on disk are still those assemblies -- and only if
# nothing has been added beside them. In CI this directory comes back from actions/cache.
entries = record.get("files")
if not entries:
    print(f"error: {marker} records no assemblies, so it cannot vouch for {root}", file=sys.stderr)
    print("       Corrupt Carbon reference cache. Pass --force to replace it.", file=sys.stderr)
    raise SystemExit(5)

problems = []
recorded_names = set()
for entry in entries:
    name = pathlib.PurePosixPath(str(entry.get("path") or "")).name
    recorded_names.add(name)
    path = root / name
    if not name or not path.is_file():
        problems.append(f"{entry.get('path')!r} is recorded but is not in the directory")
        continue
    payload = path.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != str(entry["sha256"]) or len(payload) != int(entry["size"]):
        problems.append(
            f"{name} is {digest[:16]} ({len(payload)} bytes), not the recorded "
            f"{str(entry['sha256'])[:16]} ({entry['size']} bytes)"
        )
for found in sorted(p.name for p in root.glob("*.dll") if p.is_file()):
    if found not in recorded_names:
        problems.append(f"{found} is in the directory but not in the record")

if problems:
    print(f"error: {root} is not the Carbon reference set its own record describes:", file=sys.stderr)
    for problem in problems:
        print(f"       {problem}", file=sys.stderr)
    print("       Altered Carbon reference cache. Pass --force to replace it.", file=sys.stderr)
    raise SystemExit(5)

print(f"verified {len(entries)} Carbon assemblies against {marker.name}")
CHECK
    echo "Carbon references for ${RUST_TARGET} are already in ${CARBON_REFS}"
else
    ARCHIVE="./_data/${RUST_CARBON_ASSET}"
    echo "Downloading ${RUST_CARBON_ASSET} (${RUST_CARBON_SHA256:0:16}...)"
    curl -fsSL "${RUST_CARBON_DOWNLOAD_URL}" -o "${ARCHIVE}"
    printf '%s  %s\n' "${RUST_CARBON_SHA256}" "${ARCHIVE}" \
        | sha256sum --check --status \
        || { echo "error: ${RUST_CARBON_ASSET} is not the archive ${RUST_TARGET} pins" >&2; exit 5; }

    rm -rf "${CARBON_REFS}"
    mkdir -p "${CARBON_REFS}"
    # Only carbon/managed/*.dll: the top-level framework assemblies the plugin links
    # against. The hooks, modules and native libraries are runtime-only.
    tar -xzf "${ARCHIVE}" -C "${CARBON_REFS}" --strip-components=2 \
        --wildcards --no-wildcards-match-slash --no-anchored 'carbon/managed/*.dll'
    rm -f "${ARCHIVE}"
    mkdir -p "${CARBON_REFS}/.takaro"
    CARBON_REFS="${CARBON_REFS}" RUST_FINGERPRINT="${RUST_FINGERPRINT}" \
    RUST_CARBON_ASSET="${RUST_CARBON_ASSET}" RUST_CARBON_SHA256="${RUST_CARBON_SHA256}" \
        python3 - <<'PY' > "${MARKER}"
import hashlib, json, os, pathlib
root = pathlib.Path(os.environ["CARBON_REFS"])
files = []
for path in sorted(p for p in root.glob("*.dll") if p.is_file()):
    payload = path.read_bytes()
    files.append({"path": path.name, "sha256": hashlib.sha256(payload).hexdigest(), "size": len(payload)})
print(json.dumps({
    "schemaVersion": 1,
    "fingerprint": os.environ["RUST_FINGERPRINT"],
    "asset": os.environ["RUST_CARBON_ASSET"],
    "sha256": os.environ["RUST_CARBON_SHA256"],
    "files": files,
}, indent=2))
PY
    echo "Carbon references: $(find "${CARBON_REFS}" -maxdepth 1 -name '*.dll' | wc -l) assemblies in ${CARBON_REFS}"
fi

echo "Environment ready: ${GAME_REFS} + ${CARBON_REFS}"
echo "Compile-check the plugin with: ./scripts/compile-check.sh --target ${RUST_TARGET}"
