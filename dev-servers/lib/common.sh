#!/usr/bin/env bash
# Shared helpers and the single game registry for dev-servers.
# Sourced by every script in dev-servers/scripts/.

# shellcheck disable=SC2034  # these are consumed by the scripts that source this file
DS_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd -- "${DS_DIR}/.." && pwd)"
DS_DATA="${DS_DIR}/_data"
DS_COMPOSE_DIR="${DS_DIR}/compose"
DS_TEMPLATES="${DS_DIR}/templates"
DS_ENV_FILE="${DS_DIR}/.env"

# ── Game registry ────────────────────────────────────────────────────────────
# id | compose | profile | services | ram_gb | disk_gb | kind | description | datadir
# profile "-" means the game owns its whole compose file (stop uses `down`);
# otherwise the game is one profile inside a shared file (stop targets services).
# Order is the install order: cheap and fast first, so failures surface early.
ds_registry() {
    cat <<'REG'
terraria|terraria.yml|-|terraria|1|1|plugin|TShock server + Takaro events plugin (Takaro connects over TShock REST)|terraria
minecraft-paper|minecraft.yml|paper|paper|3|2|connector|Paper 1.21.x + Takaro Paper plugin|minecraft/paper
minecraft-neoforge|minecraft.yml|neoforge|neoforge|3|2|connector|NeoForge 1.21.x + Takaro NeoForge mod|minecraft/neoforge
minecraft-fabric|minecraft.yml|fabric|fabric|3|2|connector|Fabric (catalog target) + Takaro Fabric mod|minecraft/fabric
minecraft-fabric-26.1.2|minecraft.yml|fabric-26-1-2|fabric-26-1-2|3|2|connector|Fabric 26.1.2 (catalog target) + Takaro Fabric mod|minecraft/fabric-26.1.2
valheim|valheim.yml|-|valheim|4|6|connector|Valheim + BepInEx + Takaro Valheim plugin|valheim
dayz|dayz.yml|-|dayz dayz-takaro|6|4|sidecar|DayZ (Linux, app 223350) + @TakaroIntegration mod + Takaro TypeScript sidecar|dayz
dragonwilds|dragonwilds.yml|-|dragonwilds dragonwilds-takaro|4|8|sidecar|RuneScape: Dragonwilds (Linux, app 4019830) + Takaro LD_PRELOAD plugin + TypeScript sidecar|dragonwilds-dev
rust|rust.yml|-|rust|8|12|connector|Rust + Carbon + TakaroConnector.cs|rust
7d2d|7d2d.yml|-|7d2d|8|32|connector|7 Days to Die + Takaro mod|7d2d
zomboid|zomboid.yml|-|zomboid|8|16|connector|Project Zomboid B42 + Takaro javaagent|zomboid
palworld|palworld.yml|-|palworld palworld-bridge|12|6|bridge|Palworld + third-party Takaro bridge (REST only)|palworld
conan-exiles|conan-exiles.yml|-|conan-exiles conan-bridge|12|6|sidecar|Conan Exiles + Takaro TypeScript sidecar|conan-exiles
REG
}

ds_game_ids() { ds_registry | cut -d'|' -f1; }

# ds_field <game-id> <1-based field number>
ds_field() {
    local id="$1" n="$2" line
    line="$(ds_registry | awk -F'|' -v g="$1" '$1==g {print; exit}')"
    [ -n "$line" ] || return 1
    printf '%s' "$line" | cut -d'|' -f"$n"
}

ds_is_game() { ds_registry | cut -d'|' -f1 | grep -qx -- "$1"; }

ds_compose_file() { printf '%s/%s' "$DS_COMPOSE_DIR" "$(ds_field "$1" 2)"; }
ds_profile()      { ds_field "$1" 3; }
ds_services()     { ds_field "$1" 4; }
ds_ram_gb()       { ds_field "$1" 5; }
ds_disk_gb()      { ds_field "$1" 6; }
ds_kind()         { ds_field "$1" 7; }
ds_description()  { ds_field "$1" 8; }
ds_data_subdir()  { ds_field "$1" 9; }
ds_owns_file()    { [ "$(ds_profile "$1")" = "-" ]; }

# Services from the registry that can actually be started right now: a service whose
# compose `build:` context does not exist on disk yet (a connector source tree another
# lane has not created) is skipped instead of failing the whole start.
ds_startable_services() {
    local id="$1" file svc want
    file="$(ds_compose_file "$id")"
    want="$(ds_services "$id")"
    for svc in $want; do
        if ds_service_build_context_missing "$file" "$svc"; then
            ds_warn "skipping service ${svc}: its build context does not exist yet" >&2
            continue
        fi
        printf '%s ' "$svc"
    done
}

# True when <service> in <compose file> has a build context that is not on disk.
ds_service_build_context_missing() {
    python3 - "$1" "$2" <<'PYEOF'
import os, re, sys
path, svc = sys.argv[1], sys.argv[2]
base = os.path.dirname(os.path.abspath(path))
lines = open(path).read().splitlines()
in_svc = False
for line in lines:
    m = re.match(r'^  ([A-Za-z0-9_.-]+):\s*$', line)
    if m:
        in_svc = (m.group(1) == svc)
        continue
    if in_svc:
        b = re.match(r'^    build:\s*(\S.*?)\s*$', line)
        if b:
            ctx = b.group(1).strip('"\'')
            sys.exit(0 if not os.path.isdir(os.path.join(base, ctx)) else 1)
sys.exit(1)
PYEOF
}

ds_die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
ds_warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
ds_info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
ds_ok()   { printf '\033[32m  ok\033[0m %s\n' "$*"; }

ds_validate_game() {
    ds_is_game "$1" || ds_die "unknown game '$1'. Known: $(ds_game_ids | tr '\n' ' ')"
}

# ── Environment ──────────────────────────────────────────────────────────────
ds_load_env() {
    if [ ! -f "$DS_ENV_FILE" ]; then
        ds_die "missing ${DS_ENV_FILE}
  cp dev-servers/.env.example dev-servers/.env && chmod 600 dev-servers/.env"
    fi
    local mode
    mode="$(stat -c '%a' "$DS_ENV_FILE")"
    case "$mode" in
        600|400) ;;
        *) ds_warn ".env is mode ${mode}; it holds your registration token. chmod 600 dev-servers/.env" ;;
    esac
    # Parsed, not sourced: values may contain spaces, and nothing in .env should
    # ever be executed. Quote-stripping matches docker compose's --env-file.
    local line key value
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        [ "${line#*=}" != "$line" ] || continue
        key="${line%%=*}"
        value="${line#*=}"
        key="${key#export }"
        key="${key#"${key%%[![:space:]]*}"}"
        key="${key%"${key##*[![:space:]]}"}"
        case "$key" in
            [A-Za-z_]*) ;;
            *) continue ;;
        esac
        case "$value" in
            \"*\") value="${value:1:${#value}-2}" ;;
            \'*\') value="${value:1:${#value}-2}" ;;
        esac
        export "${key}=${value}"
    done < "$DS_ENV_FILE"
    DEV_UID="${DEV_UID:-$(id -u)}"
    DEV_GID="${DEV_GID:-$(id -g)}"
    export DEV_UID DEV_GID
}

ds_require_token() {
    [ -n "${TAKARO_REGISTRATION_TOKEN:-}" ] \
        || ds_die "TAKARO_REGISTRATION_TOKEN is empty in dev-servers/.env.
  Get one from the Takaro dashboard: https://docs.takaro.io/advanced/adding-support-for-a-new-game"
}

# ── Compose ──────────────────────────────────────────────────────────────────
# ds_compose <game-id> <docker compose args...>
ds_compose() {
    local id="$1"; shift
    local -a args=(--env-file "$DS_ENV_FILE" -f "$(ds_compose_file "$id")")
    local profile other
    profile="$(ds_profile "$id")"
    [ "$profile" = "-" ] || args+=(--profile "$profile")
    # Games driven by a catalog target keep their resolved values in
    # _data/.targets/<game>.env. Every game sharing this compose file contributes one,
    # because a single `docker compose` call sees the whole file.
    if [ -d "${DS_DATA}/.targets" ]; then
        for other in $(ds_game_ids); do
            [ -f "${DS_DATA}/.targets/${other}.env" ] || continue
            [ "$(ds_compose_file "$other")" = "$(ds_compose_file "$id")" ] || continue
            args+=(--env-file "${DS_DATA}/.targets/${other}.env")
        done
    fi
    ( cd "$DS_COMPOSE_DIR" && docker compose "${args[@]}" "$@" )
}

# Running containers for a game, one id per line.
ds_running_containers() {
    local id="$1" svc out=""
    for svc in $(ds_services "$id"); do
        out="$(ds_compose "$id" ps -q --status running "$svc" 2>/dev/null || true)"
        [ -n "$out" ] && printf '%s\n' "$out"
    done
}

ds_is_running() { [ -n "$(ds_running_containers "$1")" ]; }

ds_marker() { printf '%s/.markers/%s' "$DS_DATA" "$1"; }
ds_is_installed() { [ -f "$(ds_marker "$1")" ]; }

ds_data_dir() { printf '%s/%s' "$DS_DATA" "$(ds_data_subdir "$1")"; }

# ── Utilities ────────────────────────────────────────────────────────────────
# Render ${VAR} placeholders in a template using the current environment.
# Only substitutes variables, never executes anything from the template.
ds_render() {
    local template="$1" target="$2"
    [ -f "$template" ] || ds_die "missing template ${template}"
    mkdir -p "$(dirname "$target")"
    python3 -c '
import os, sys, string
src, dst = sys.argv[1], sys.argv[2]
with open(src) as fh:
    out = string.Template(fh.read()).substitute(os.environ)
with open(dst, "w") as fh:
    fh.write(out)
' "$template" "$target"
    chmod 600 "$target"
}

# Some game images run as root and leave root-owned files in _data. Hand them
# back so the host user can manage _data without sudo.
ds_fix_ownership() {
    local path="$1"
    [ -d "$path" ] || return 0
    if [ -n "$(find "$path" ! -user "$(id -u)" -print -quit 2>/dev/null)" ]; then
        ds_info "Reclaiming ownership of $(basename "$path") data"
        docker run --rm -v "${path}:/target" alpine:latest \
            chown -R "$(id -u):$(id -g)" /target >/dev/null
    fi
}

# ds_wait_for_file <game> <host path> <timeout seconds> <what>
ds_wait_for_file() {
    local id="$1" path="$2" timeout="$3" what="$4" waited=0
    ds_info "Waiting for ${what} (up to $((timeout / 60)) min)..."
    while [ "$waited" -lt "$timeout" ]; do
        [ -e "$path" ] && { ds_ok "${what} ready"; return 0; }
        if ! ds_is_running "$id"; then
            ds_warn "${id} container exited before ${what} appeared"
            ds_compose "$id" logs --tail 30 || true
            return 1
        fi
        sleep 5
        waited=$((waited + 5))
        [ $((waited % 120)) -eq 0 ] && ds_info "  still waiting (${waited}s)..."
    done
    ds_warn "timed out waiting for ${what}"
    return 1
}

# ds_wait_for_condition <game> <shell condition> <timeout secs> <what>
# Use when "a file exists" is too weak a signal — e.g. SteamCMD writes the game
# binary long before it finalises the install manifest.
ds_wait_for_condition() {
    local id="$1" condition="$2" timeout="$3" what="$4" waited=0
    ds_info "Waiting for ${what} (up to $((timeout / 60)) min)..."
    while [ "$waited" -lt "$timeout" ]; do
        if eval "$condition"; then ds_ok "${what} ready"; return 0; fi
        if ! ds_is_running "$id"; then
            ds_warn "${id} container exited before ${what}"
            ds_compose "$id" logs --tail 30 || true
            return 1
        fi
        sleep 10
        waited=$((waited + 10))
        [ $((waited % 300)) -eq 0 ] && ds_info "  still waiting (${waited}s)..."
    done
    ds_warn "timed out waiting for ${what}"
    return 1
}

# True once SteamCMD reports an app fully installed (StateFlags 4).
ds_steam_app_ready() {
    local manifest="$1"
    [ -f "$manifest" ] || return 1
    grep -qE '"StateFlags"[[:space:]]+"4"' "$manifest"
}

ds_free_disk_gb() { df -BG --output=avail "$DS_DIR" | tail -1 | tr -dc '0-9'; }

# Run a build command in a toolchain container when the host lacks the tool.
# The repo is mounted at its own absolute path so relative paths and realpath
# inside the connectors' own build scripts keep working unchanged.
# Usage: ds_toolchain_run <image> <workdir> <command...>
ds_toolchain_run() {
    local image="$1" workdir="$2"
    shift 2
    local cache="${DS_DATA}/.toolcache"
    mkdir -p "${cache}/home" "${cache}/gradle" "${cache}/nuget"
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "${REPO_ROOT}:${REPO_ROOT}" \
        -v "${cache}/home:/tmp/buildhome" \
        -v "${cache}/gradle:/tmp/gradle-home" \
        -v "${cache}/nuget:/tmp/nuget" \
        -w "$workdir" \
        -e HOME=/tmp/buildhome \
        -e GRADLE_USER_HOME=/tmp/gradle-home \
        -e NUGET_PACKAGES=/tmp/nuget \
        -e DOTNET_CLI_TELEMETRY_OPTOUT=1 \
        -e DOTNET_NOLOGO=1 \
        "$image" "$@"
}

# ── Connector source fingerprints ────────────────────────────────────────────
# Source files that determine a connector's built artifact. Build outputs and
# runtime data are excluded so they cannot cause false positives.
ds_source_paths() {
    case "$1" in
        rust)               echo "games/rust/mod games/rust/version.txt" ;;
        minecraft-paper)    echo "games/minecraft/mod/core games/minecraft/mod/paper games/minecraft/mod/gradle games/minecraft/mod/build.gradle.kts games/minecraft/mod/settings.gradle.kts" ;;
        minecraft-neoforge) echo "games/minecraft/mod/core games/minecraft/mod/neoforge games/minecraft/mod/gradle games/minecraft/mod/build.gradle.kts games/minecraft/mod/settings.gradle.kts" ;;
        minecraft-fabric)   echo "games/minecraft/mod/core games/minecraft/mod/fabric games/minecraft/mod/targets games/minecraft/mod/buildSrc games/minecraft/mod/gradle games/minecraft/mod/build.gradle.kts games/minecraft/mod/settings.gradle.kts catalog/minecraft" ;;
        minecraft-fabric-26.1.2) echo "games/minecraft/mod/core games/minecraft/mod/fabric games/minecraft/mod/targets games/minecraft/mod/buildSrc games/minecraft/mod/gradle games/minecraft/mod/build.gradle.kts games/minecraft/mod/settings.gradle.kts catalog/minecraft" ;;
        7d2d)               echo "games/7d2d/mod/src games/7d2d/mod/Takaro.csproj games/7d2d/mod/ModInfo.xml games/7d2d/version.txt" ;;
        zomboid)            echo "games/zomboid/mod/core games/zomboid/mod/agent games/zomboid/mod/gradle games/zomboid/mod/build.gradle.kts games/zomboid/mod/settings.gradle.kts games/zomboid/version.txt" ;;
        valheim)            echo "games/valheim/mod/src games/valheim/version.txt" ;;
        terraria)           echo "games/terraria/mod/src games/terraria/version.txt" ;;
        dragonwilds)        echo "games/dragonwilds/mod games/dragonwilds/sidecar/src games/dragonwilds/version.txt" ;;
        conan-exiles)       echo "games/conan-exiles/bridge/src games/conan-exiles/bridge/package.json games/conan-exiles/bridge/tsconfig.json" ;;
        *)                  echo "" ;;
    esac
}

# SHA-256 over the current contents of a connector's source tree.
ds_source_fingerprint() {
    local game="$1" paths existing=() p
    read -r -a paths <<< "$(ds_source_paths "$game")"
    [ ${#paths[@]} -gt 0 ] || { echo "n/a"; return; }
    for p in "${paths[@]}"; do
        [ -e "${REPO_ROOT}/${p}" ] && existing+=("${REPO_ROOT}/${p}")
    done
    [ ${#existing[@]} -gt 0 ] || { echo "n/a"; return; }
    find "${existing[@]}" -type f \
        -not -path "*/build/*" -not -path "*/bin/*" -not -path "*/obj/*" \
        -not -path "*/_data/*" -not -path "*/node_modules/*" -print0 2>/dev/null \
        | sort -z | xargs -0 sha256sum 2>/dev/null | sha256sum | cut -d" " -f1
}

# Record the current source fingerprint as "what is deployed right now".
ds_record_fingerprint() {
    local game="$1" fp
    fp="$(ds_source_fingerprint "$game")"
    [ "$fp" = "n/a" ] && return 0
    mkdir -p "${DS_DATA}/.fingerprints"
    printf "%s" "$fp" > "${DS_DATA}/.fingerprints/${game}"
}

# True when the host can build natively; otherwise callers fall back to Docker.
ds_have() { command -v "$1" >/dev/null 2>&1; }

# Write a value back into dev-servers/.env so generated credentials survive
# re-runs. Replaces the key in place if present, appends it otherwise.
ds_persist_env() {
    local key="$1" value="$2"
    [ -w "$DS_ENV_FILE" ] || { ds_warn "cannot write ${DS_ENV_FILE}; set ${key} by hand"; return 0; }
    if grep -qE "^${key}=" "$DS_ENV_FILE"; then
        python3 -c '
import sys, re
path, key, value = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path) as fh:
    lines = fh.readlines()
with open(path, "w") as fh:
    for line in lines:
        fh.write(f"{key}={value}\n" if re.match(rf"^{re.escape(key)}=", line) else line)
' "$DS_ENV_FILE" "$key" "$value"
    else
        printf '%s=%s\n' "$key" "$value" >> "$DS_ENV_FILE"
    fi
}

# ── Catalog targets ──────────────────────────────────────────────────────────
# A game whose server build is pinned by catalog/<game>/targets/<id>.json is installed,
# built and deployed through the maintenance command, so the rig, CI and a release all
# resolve the same bytes. Games without a target keep their own install path.

ds_maint() { "${REPO_ROOT}/maintenance/bin/takaro-maint" "$@"; }

# ds_target <rig-game-id> -> the catalog target id driving it, empty when there is none.
ds_target() {
    ds_maint targets list --rig-game "$1" --format json 2>/dev/null \
        | python3 -c 'import json,sys
try:
    targets = json.load(sys.stdin)["targets"]
except Exception:
    targets = []
print(targets[0]["id"] if targets else "")'
}

# The catalog game a rig game belongs to (minecraft-fabric -> minecraft).
ds_target_game() { printf '%s' "${1%%-*}"; }

ds_target_env_file() { printf '%s/.targets/%s.env' "$DS_DATA" "$1"; }

# Env key prefix for a rig game: minecraft-fabric -> MC_FABRIC.
ds_target_prefix() {
    case "$1" in
        minecraft-paper)    printf 'MC_PAPER' ;;
        minecraft-neoforge) printf 'MC_NEOFORGE' ;;
        minecraft-fabric)   printf 'MC_FABRIC' ;;
        minecraft-fabric-26.1.2) printf 'MC_FABRIC_26_1_2' ;;
        *) printf '%s' "$1" | tr '[:lower:]-' '[:upper:]_' ;;
    esac
}

# Resolve the target into the env file compose reads.
ds_write_target_env() {
    local game="$1" target
    target="$(ds_target "$game")"
    [ -n "$target" ] || return 0
    mkdir -p "${DS_DATA}/.targets"
    ds_maint targets resolve \
        --game "$(ds_target_game "$game")" \
        --target "$target" \
        --format env \
        --prefix "$(ds_target_prefix "$game")" \
        --out "$(ds_target_env_file "$game")"
}

# Refuse to start a target-driven game whose data dir does not hold that target.
ds_preflight_target() {
    local game="$1" target
    target="$(ds_target "$game")"
    [ -n "$target" ] || return 0
    if [ ! -f "$(ds_target_env_file "$game")" ]; then
        ds_die "${game} is driven by catalog target ${target} but has no resolved environment.
  dev-servers/scripts/install.sh ${game}"
    fi
    if ! ds_maint ledger check \
        --game "$(ds_target_game "$game")" \
        --target "$target" \
        --dest "$(ds_data_dir "$game")" >/dev/null; then
        ds_die "${game} does not hold catalog target ${target} (or its files changed).
  dev-servers/scripts/install.sh ${game}"
    fi
}
