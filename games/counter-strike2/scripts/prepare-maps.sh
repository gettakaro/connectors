#!/usr/bin/env bash
set -euo pipefail

server_root=${1:?usage: prepare-maps.sh <cs2-server-root> <Source2Viewer-CLI> [map ...]}
viewer=${2:?usage: prepare-maps.sh <cs2-server-root> <Source2Viewer-CLI> [map ...]}
shift 2
[[ -x "$viewer" ]] || { echo "Source2Viewer-CLI is not executable: $viewer" >&2; exit 2; }
pak="$server_root/game/csgo/pak01_dir.vpk"
[[ -f "$pak" ]] || { echo "CS2 package not found: $pak" >&2; exit 2; }
target="$server_root/game/csgo/addons/counterstrikesharp/data/TakaroCs2/maps"
mkdir -p "$target"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

if (( $# )); then
  maps=("$@")
else
  mapfile -t maps < <(find "$server_root/game/csgo/maps" -maxdepth 1 -type f -name '*.vpk' -printf '%f\n' | sed 's/\.vpk$//' | sort)
fi

for map in "${maps[@]}"; do
  [[ "$map" =~ ^[A-Za-z0-9_]+$ ]] || { echo "invalid map name: $map" >&2; exit 2; }
  map_pak="$server_root/game/csgo/maps/$map.vpk"
  [[ -f "$map_pak" ]] || { echo "map package not found: $map_pak" >&2; exit 2; }
  rm -rf "$tmp/map"
  mkdir -p "$tmp/map"
  if ! "$viewer" -i "$pak" -f "panorama/images/overheadmaps/${map}_radar_psd.vtex_c" -d -o "$tmp/map" >/dev/null ||
     ! "$viewer" -i "$pak" -f "resource/overviews/$map.txt" -o "$tmp/map" >/dev/null ||
     ! "$viewer" -i "$map_pak" --vpk_extensions vents_c -d -o "$tmp/map" >/dev/null; then
    echo "map $map assets could not be extracted; skipped" >&2
    continue
  fi
  radar="$tmp/map/panorama/images/overheadmaps/${map}_radar_psd.png"
  overview="$tmp/map/resource/overviews/$map.txt"
  entities="$tmp/map/maps/$map/entities"
  if [[ ! -s "$radar" || ! -s "$overview" || ! -d "$entities" ]]; then
    echo "map $map has no complete radar, overview and entity source; skipped" >&2
    continue
  fi
  mapfile -d '' entity_files < <(find "$entities" -type f -name '*.vents' -print0 | sort -z)
  if (( ! ${#entity_files[@]} )); then
    echo "map $map has no entity data; skipped" >&2
    continue
  fi
  cp "$radar" "$target/$map.png"
  cp "$overview" "$target/$map.txt"
  cat "${entity_files[@]}" > "$target/$map.vents"
  echo "prepared $map"
done
