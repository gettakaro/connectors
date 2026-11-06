#!/usr/bin/env bash
# Fills <dest> with what native/build.bat compiles against, for one catalog target:
#   <dest>/AsaApi/   AsaApi source at the pinned commit (headers)
#   <dest>/Lib/      AsaApi.lib from the pinned release zip
#   <dest>/fmt/      fmt at the pinned release
#
#   games/ark-ascended/scripts/fetch-deps.sh --target <id> --dest <dir>
#
# Every download is checked against the catalog's sha256. This runs on the Windows build runner, which
# has no takaro-maint, so it reads the target record directly; it needs bash, curl, jq and Python.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../../.." && pwd)

target="" dest=""
while [ $# -gt 0 ]; do
    case "$1" in
        --target) shift; target="${1:?--target needs a catalog target id}" ;;
        --dest) shift; dest="${1:?--dest needs a directory}" ;;
        *) echo "unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$target" ] && [ -n "$dest" ] || { echo "usage: fetch-deps.sh --target <id> --dest <dir>" >&2; exit 2; }

record="$repo_root/catalog/ark-ascended/targets/$target.json"
[ -f "$record" ] || { echo "no catalog target $target" >&2; exit 2; }
python_bin=$(command -v python || command -v python3)

mkdir -p "$dest/downloads"
fetch() { # fetch <dep name> <file>
    local url sha file="$dest/downloads/$2"
    # jq on Windows ends its lines with CRLF.
    url=$(jq -er --arg d "$1" '.build.deps[$d].resolvedCoordinate' "$record" | tr -d '\r')
    sha=$(jq -er --arg d "$1" '.build.deps[$d].sha256' "$record" | tr -d '\r')
    curl -fsSL --retry 3 "$url" -o "$file"
    echo "$sha  $file" | sha256sum -c --quiet -
}
fetch asaapi asaapi.zip
fetch asaapi-source asaapi-source.tar.gz
fetch fmt fmt.zip

"$python_bin" -I - "$dest" <<'PY'
import os, shutil, sys, tarfile, zipfile

dest = sys.argv[1]
downloads = os.path.join(dest, "downloads")


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as out:
        out.write(data)


def stripped(name):
    return name.split("/", 1)[1] if "/" in name else ""


for folder in ("AsaApi", "Lib", "fmt"):
    shutil.rmtree(os.path.join(dest, folder), ignore_errors=True)

with zipfile.ZipFile(os.path.join(downloads, "asaapi.zip")) as archive:
    write(os.path.join(dest, "Lib", "AsaApi.lib"), archive.read("Lib/AsaApi.lib"))

with tarfile.open(os.path.join(downloads, "asaapi-source.tar.gz")) as archive:
    for member in archive.getmembers():
        relative = stripped(member.name)
        if member.isfile() and relative:
            write(os.path.join(dest, "AsaApi", *relative.split("/")), archive.extractfile(member).read())

with zipfile.ZipFile(os.path.join(downloads, "fmt.zip")) as archive:
    for name in archive.namelist():
        relative = stripped(name)
        if relative and not name.endswith("/"):
            write(os.path.join(dest, "fmt", *relative.split("/")), archive.read(name))
PY
