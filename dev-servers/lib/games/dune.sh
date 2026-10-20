#!/usr/bin/env bash
# Dune: Awakening — our Compose expansion of Funcom's self-hosted battlegroup, plus the
# Takaro TypeScript sidecar and an optional LD_PRELOAD plugin in the map server.
#
# Sourced by dev-servers/lib/common.sh. It registers the game in the shared registry and
# defines the steps install.sh, deploy-connector.sh and verify-connectors.sh dispatch to.
#
# It installs last on purpose: 4.9 GB of Steam package that unpacks into a 10.8 GB server
# image is the most expensive thing in this rig, so every cheaper game fails first.

ds_register 130 'dune|dune.yml|-|postgres admin-rmq game-rmq text-router director gateway survival dune-takaro|24|60|sidecar|Dune: Awakening self-hosted battlegroup (Steam tool 4754530) + Takaro TypeScript sidecar (RabbitMQ + Postgres); optional LD_PRELOAD plugin|dune-dev'

DUNE_SRC="${DUNE_SRC:-${REPO_ROOT}/games/dune}"
DUNE_RIG_LOCK="${DUNE_RIG_LOCK:-${DS_DATA}/dune-rig.lock}"

# The app, the depot and the manifest are written down once, in catalog/dune/targets/, and
# read from there. The rig does the download and the `docker load` itself because the shared
# install path has no seam for an image-bundle depot yet, but it must not hold a second copy
# of the pins while it waits for one.
dune_target_pins() {
    local env_file key value
    env_file="$(ds_target_env_file dune)"
    mkdir -p "$(dirname "$env_file")"
    if ! ds_maint targets resolve --game dune --format env --prefix DUNE_PIN --out "$env_file" >/dev/null; then
        ds_die "could not resolve the Dune catalog target; the rig will not download a build it cannot name."
    fi
    # Parsed, never sourced: nothing generated is executed.
    while IFS='=' read -r key value; do
        case "$key" in DUNE_PIN_*) export "${key}=${value}" ;; esac
    done < "$env_file"
}

# A docker-save archive records the exact image reference it will load. Read that identity from
# the pinned bytes instead of copying Funcom's current tag into the rig a second time.
dune_tar_image_ref() {
    local archive="$1"
    tar -xOf "$archive" manifest.json 2>/dev/null \
        | jq -er '.[0].RepoTags | if length == 1 then .[0] else error("expected one RepoTag") end'
}

# Characters in one Dune database; 0 when the schema is not there yet.
dune_db_characters() {
    local n
    n="$(docker exec takaro-dev-dune-postgres psql -U dune -d "$1" -Atc 'select count(*) from dune.player_state' 2>/dev/null)"
    printf '%s' "${n:-0}"
}

# A new code branch renames the database the battlegroup expects. Carry the world over by renaming
# the previous branch's database (updatedb then applies the new branch's patches to it) rather than
# letting db-init create an empty one beside it. Anything still connected to the old name is stopped
# first, because Postgres refuses to rename a database in use.
dune_migrate_db_name() {
    local psql=(docker exec takaro-dev-dune-postgres psql -U dune -d postgres -Atc)
    local old
    old="$("${psql[@]}" "select datname from pg_database where datname like 'dune\\_sb\\_%' and datname <> '${DUNE_DB_NAME}'")"
    if [ -n "$("${psql[@]}" "select 1 from pg_database where datname='${DUNE_DB_NAME}'")" ]; then
        # Already on the new name. An older database beside it means a partial upgrade created the
        # new one empty; continuing would silently start a fresh world.
        [ -z "$old" ] || [ "$(dune_db_characters "$DUNE_DB_NAME")" -gt 0 ] \
            || ds_die "${DUNE_DB_NAME} has no characters but ${old//$'\n'/, } exists; drop the empty ${DUNE_DB_NAME} and re-run install to carry the world over"
        return 0
    fi
    [ -n "$old" ] || return 0
    [ "$(printf '%s\n' "$old" | wc -l)" -eq 1 ] \
        || ds_die "several old Dune databases (${old//$'\n'/, }); rename the one to keep to ${DUNE_DB_NAME} by hand"
    ds_info "Renaming database ${old} -> ${DUNE_DB_NAME} for the new code branch..."
    ds_compose dune --profile sidecar stop survival director text-router gateway dune-takaro >/dev/null 2>&1 || true
    "${psql[@]}" "ALTER DATABASE \"${old}\" RENAME TO \"${DUNE_DB_NAME}\"" >/dev/null \
        || ds_die "could not rename ${old} to ${DUNE_DB_NAME}"
    ds_ok "database ${old} renamed to ${DUNE_DB_NAME}"
}

install_dune() {
    # There is no SteamCMD path: Steam tool app 4754530 refuses `login anonymous` in
    # steamcmd ("missing license for depot (No subscription)") even though depot 4754532 IS
    # anonymously readable. DepotDownloader does read it, so that is what we use. The depot
    # is not a game install — it is a bundle of container image tarballs plus Funcom's k3s
    # setup scripts.
    dune_target_pins
    local tool="${DS_DATA}/dune-dev/tool"
    local appid="${DUNE_PIN_STEAM_APP}" depotid manifest
    depotid="${DUNE_PIN_STEAM_DEPOTS%%:*}"
    manifest="${DUNE_PIN_STEAM_DEPOTS##*:}"

    mkdir -p "${DS_DATA}/dune-dev"/{tool,dd,logs,postgres,rmq-admin,rmq-game,saved,sidecar,tls} \
             "${DS_DATA}/dune-plugin"

    # ── 1. the Steam package (5.2 GB) ────────────────────────────────────────
    if [ -f "${tool}/images/battlegroup/server.tar" ]; then
        ds_ok "Steam package already present (${tool})"
    else
        ds_info "Downloading Steam tool ${appid} depot ${depotid} manifest ${manifest} (~5.2 GB, DepotDownloader, anonymous)..."
        docker run --rm -v "${DS_DATA}/dune-dev/dd:/out" -w /out mcr.microsoft.com/dotnet/sdk:9.0 bash -c "
            set -e
            if [ ! -x ./DepotDownloader ]; then
                apt-get update -qq && apt-get install -y -qq unzip curl >/dev/null
                curl -sSL -o dd.zip https://github.com/SteamRE/DepotDownloader/releases/latest/download/DepotDownloader-linux-x64.zip
                unzip -oq dd.zip && chmod +x DepotDownloader
            fi
            ./DepotDownloader -app ${appid} -depot ${depotid} -manifest ${manifest} -dir /out/tool -max-downloads 4
        " 2>&1 | tee "${DS_DATA}/dune-dev/logs/depotdownloader.txt" \
            || ds_die "DepotDownloader failed; see ${DS_DATA}/dune-dev/logs/depotdownloader.txt"
        # It runs as root inside the container.
        ds_fix_ownership "${DS_DATA}/dune-dev/dd"
        [ -f "${DS_DATA}/dune-dev/dd/tool/images/battlegroup/server.tar" ] \
            || ds_die "the depot downloaded but images/battlegroup/server.tar is missing"
        rm -rf "${tool}" && mv "${DS_DATA}/dune-dev/dd/tool" "${tool}"
        ds_ok "${tool}"
    fi

    # The bytes the catalog declared, or this is not that build.
    local target_file="${REPO_ROOT}/catalog/dune/targets/${DUNE_PIN_TARGET}.json"
    local relative declared actual
    while IFS=$'\t' read -r relative declared; do
        [ -f "${tool}/${relative}" ] \
            || ds_die "the pinned Dune package is missing ${relative}; delete ${tool} and re-run install."
        actual="$(sha256sum "${tool}/${relative}" | cut -d' ' -f1)"
        [ "$declared" = "$actual" ] \
            || ds_die "${relative} is not the build ${target_file} declares:
  declared ${declared}
  actual   ${actual}
  Delete ${tool} and re-run install to fetch the pinned manifest again."
    done < <(jq -r '.inputs.server.files | to_entries[] | [.key, .value.sha256] | @tsv' "$target_file")
    ds_ok "all declared package files match the pinned target ${DUNE_PIN_TARGET}"

    local server_image gateway_image director_image text_router_image rabbitmq_image db_utils_image postgres_image
    server_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server.tar")" \
        || ds_die "server.tar does not contain exactly one Docker image reference"
    gateway_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server-gateway.tar")" \
        || ds_die "server-gateway.tar does not contain exactly one Docker image reference"
    director_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server-bg-director.tar")" \
        || ds_die "server-bg-director.tar does not contain exactly one Docker image reference"
    text_router_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server-text-router.tar")" \
        || ds_die "server-text-router.tar does not contain exactly one Docker image reference"
    rabbitmq_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server-rabbitmq.tar")" \
        || ds_die "server-rabbitmq.tar does not contain exactly one Docker image reference"
    db_utils_image="$(dune_tar_image_ref "${tool}/images/battlegroup/server-db-utils.tar")" \
        || ds_die "server-db-utils.tar does not contain exactly one Docker image reference"
    postgres_image="$(dune_tar_image_ref "${tool}/images/prerequisites/igw-postgres.tar")" \
        || ds_die "igw-postgres.tar does not contain exactly one Docker image reference"

    local tag="${server_image##*:}"
    {
        printf 'DUNE_SERVER_IMAGE=%s\n' "$server_image"
        printf 'DUNE_GATEWAY_IMAGE=%s\n' "$gateway_image"
        printf 'DUNE_DIRECTOR_IMAGE=%s\n' "$director_image"
        printf 'DUNE_TEXT_ROUTER_IMAGE=%s\n' "$text_router_image"
        printf 'DUNE_RABBITMQ_IMAGE=%s\n' "$rabbitmq_image"
        printf 'DUNE_DB_UTILS_IMAGE=%s\n' "$db_utils_image"
        printf 'DUNE_POSTGRES_IMAGE=%s\n' "$postgres_image"
        printf 'DUNE_IMAGE_TAG=%s\n' "$tag"
    } >> "$(ds_target_env_file dune)"

    local shipped_tag
    shipped_tag="$(tr -d '[:space:]' < "${tool}/images/battlegroup/version.txt" 2>/dev/null || true)"
    if [ -n "$shipped_tag" ] && [ "$shipped_tag" != "$tag" ]; then
        ds_die "the package version '${shipped_tag}' disagrees with the server image tag '${tag}'."
    fi

    # ── 2. docker load, idempotent ───────────────────────────────────────────
    local archive image_ref
    while IFS=$'\t' read -r archive image_ref; do
        if docker image inspect "$image_ref" >/dev/null 2>&1; then
            ds_ok "${image_ref} already loaded"
        else
            ds_info "docker load ${image_ref} ..."
            docker load -i "$archive" >/dev/null \
                || ds_die "docker load failed for ${archive}"
        fi
    done <<EOF
${tool}/images/battlegroup/server.tar	${server_image}
${tool}/images/battlegroup/server-gateway.tar	${gateway_image}
${tool}/images/battlegroup/server-bg-director.tar	${director_image}
${tool}/images/battlegroup/server-text-router.tar	${text_router_image}
${tool}/images/battlegroup/server-rabbitmq.tar	${rabbitmq_image}
${tool}/images/battlegroup/server-db-utils.tar	${db_utils_image}
EOF
    if docker image inspect "$postgres_image" >/dev/null 2>&1; then
        ds_ok "${postgres_image} already loaded"
    else
        ds_info "docker load igw-postgres ..."
        docker load -i "${tool}/images/prerequisites/igw-postgres.tar" >/dev/null \
            || ds_die "docker load failed for igw-postgres.tar"
    fi

    # The director and text-router ignore Database__database and connect to the database named
    # after the server image's code branch (perforce-keywords.json: //seabass/sb-1.5.15.0/...
    # -> dune_sb_1_5_15_0). A hard-coded name crash-loops both on the first build of a new
    # branch, so the name is read from the image every install.
    local db_branch
    db_branch="$(docker run --rm --entrypoint cat "$server_image" \
        /home/dune/server/DuneSandbox/Config/perforce-keywords.json 2>/dev/null \
        | sed -n 's|.*//seabass/\(sb-[0-9][0-9.]*\)/.*|\1|p' | head -1)"
    [ -n "$db_branch" ] || ds_die "could not read the code branch from ${server_image}'s perforce-keywords.json"
    DUNE_DB_NAME="dune_$(printf '%s' "$db_branch" | tr '.-' '__')"
    export DUNE_DB_NAME
    printf 'DUNE_DB_NAME=%s\n' "$DUNE_DB_NAME" >> "$(ds_target_env_file dune)"
    # start.sh exports .env into the shell, and a shell variable beats compose's --env-file, so a
    # DUNE_DB_NAME left in .env from an earlier build would win over the target file.
    if grep -qE '^DUNE_DB_NAME=' "$DS_ENV_FILE" 2>/dev/null; then
        ds_persist_env DUNE_DB_NAME "$DUNE_DB_NAME"
    fi
    ds_ok "database ${DUNE_DB_NAME} (code branch ${db_branch})"

    # ── 3. the game-broker TLS pair ──────────────────────────────────────────
    # The image ships /etc/rabbitmq/{cacert,cert,key}.pem, but it is a Funcom test cert for
    # *.funcom.com that expired 2022-04-22 — unusable. The broker runs verify_none, so a
    # self-signed pair is fine; it just has to cover every address a client might dial.
    local tls="${DS_DATA}/dune-dev/tls"
    if [ -s "${tls}/server.crt" ] && openssl x509 -checkend 604800 -noout -in "${tls}/server.crt" >/dev/null 2>&1; then
        ds_ok "game-RMQ TLS certificate present and valid"
    else
        local ext_ip lan_ip
        ext_ip="${EXTERNAL_ADDRESS:-$(tailscale ip -4 2>/dev/null | head -1)}"
        lan_ip="$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | grep -v '^100\.' | head -1)"
        ds_info "Minting a self-signed game-RMQ certificate (SANs: ${ext_ip}, ${lan_ip}, $(hostname), game-rmq, localhost)"
        {
            printf '[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=game-rmq\n[v3]\n'
            printf 'subjectAltName=DNS:game-rmq,DNS:%s,DNS:%s.local,DNS:localhost,IP:127.0.0.1' "$(hostname)" "$(hostname)"
            [ -n "$ext_ip" ] && printf ',IP:%s' "$ext_ip"
            [ -n "$lan_ip" ] && printf ',IP:%s' "$lan_ip"
            printf '\nbasicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n'
            printf 'extendedKeyUsage=serverAuth,clientAuth\n'
        } > "${tls}/openssl.cnf"
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
            -keyout "${tls}/server.key" -out "${tls}/server.crt" -config "${tls}/openssl.cnf" >/dev/null 2>&1 \
            || ds_die "openssl could not mint the game-RMQ certificate"
        # verify_none means the CA file only has to be a valid PEM; the cert is its own CA.
        cp -f "${tls}/server.crt" "${tls}/ca.crt"
        # The broker runs as uid 999 (rabbitmq) and reads these read-only.
        chmod 644 "${tls}/server.crt" "${tls}/ca.crt"
        chmod 644 "${tls}/server.key"
        ds_ok "${tls}/server.crt"
    fi

    # ── 4. generated secrets, persisted so a re-run is stable ────────────────
    local k
    for k in DUNE_PG_PASSWORD DUNE_GM_AUTH_TOKEN DUNE_RMQ_HTTP_TOKEN_AUTH_SECRET DUNE_GM_AMQP_PASS; do
        if [ -z "${!k:-}" ]; then
            ds_info "generating ${k}"
            local v; v="$(openssl rand -hex 32)"
            ds_persist_env "$k" "$v"
            export "${k}=${v}"
        fi
    done
    if [ -z "${DUNE_WORLD_UNIQUE_NAME:-}" ]; then
        # Funcom derive this from the FLS token's HostId (sh-<hostid>-<6 letters>). Without
        # a token any stable value works; text-router's service-account regex only requires
        # that it stays constant.
        local wun suffix
        suffix="$(tr -dc '[:lower:]' </dev/urandom | head -c 6)"
        wun="sh-takarodevdune-${suffix}"
        ds_persist_env DUNE_WORLD_UNIQUE_NAME "$wun"
        export DUNE_WORLD_UNIQUE_NAME="$wun"
        ds_ok "world unique name ${wun}"
    fi
    [ -n "${DUNE_FLS_SECRET:-}" ] || ds_warn "DUNE_FLS_SECRET is empty — postgres, both brokers and text-router
  still boot, but director/gateway cannot register the world with FLS and no client can
  join. Get the token from https://account.duneawakening.com and put it in dev-servers/.env."

    # ── 5. build our map-server layer ────────────────────────────────────────
    ds_info "Building the Takaro map-server layer (our entrypoint on top of seabass-server)..."
    ds_compose dune build survival || ds_die "could not build takaro-dev-dune:${tag}"

    # ── 6. schema + partitions ───────────────────────────────────────────────
    ds_info "Starting postgres and bootstrapping the schema (85 SQL files, Funcom's own updatedb)..."
    ds_compose dune up -d postgres
    ds_wait_for_condition dune \
        "docker exec takaro-dev-dune-postgres pg_isready -U dune -d postgres >/dev/null 2>&1" \
        300 "postgres accepting connections" \
        || ds_die "postgres never became ready"
    dune_migrate_db_name
    ds_compose dune run --rm db-init || ds_die "db-init failed — see the output above"
    ds_ok "schema ${DUNE_DB_NAME}.dune initialised"

    # ── 7. the GM publisher account ──────────────────────────────────────────
    ds_info "Starting the game broker and provisioning the 'fls' publisher account..."
    ds_compose dune up -d game-rmq
    "${DS_DIR}/images/dune/provision-gm-user.sh" takaro-dev-dune-game-rmq "${DUNE_GM_AMQP_PASS}" \
        "${DUNE_GM_AMQP_USER:-fls}" || ds_die "could not provision the GM publisher account"

    ds_fix_ownership "${DS_DATA}/dune-dev/saved"

    # The plugin and sidecar images are built separately; a no-op until those trees exist.
    "${DS_DIR}/scripts/deploy-connector.sh" dune
}

# ── Dune has TWO independently deployable artefacts ──────────────────────────
# One battlegroup carries a native plugin inside the map server AND a separate sidecar
# container, and the two fail for completely unrelated reasons. Deploying them together was
# a foot-gun (2026-09-21): a plugin build failure skipped the sidecar entirely, and a plugin
# build SUCCESS ran stop.sh on the whole battlegroup — postgres and both brokers included.
#
#   DUNE_DEPLOY_PART=plugin   build the .so, swap it in, recreate ONLY survival
#   DUNE_DEPLOY_PART=sidecar  rebuild + recreate ONLY the sidecar container
#   (unset, or both)          both, in that order, and the plugin half CANNOT block the
#                             sidecar half
deploy_dune() {
    local dest="${DS_DATA}/dune-plugin"
    local plugin="${DUNE_PLUGIN_SRC:-${DUNE_SRC}/mod}"
    local sidecar="${DUNE_SIDECAR_SRC:-${DUNE_SRC}/sidecar}"
    local restart_log="${DUNE_RESTART_LOG:-${DS_DATA}/dune-dev/logs/survival-restarts.log}"
    local part="${DUNE_DEPLOY_PART:-both}"
    local plugin_rc=0

    case "$part" in
        plugin|sidecar|both) ;;
        *) ds_die "unknown DUNE_DEPLOY_PART '${part}' (want plugin, sidecar or both)" ;;
    esac

    mkdir -p "$dest" "$(dirname "$DUNE_RIG_LOCK")" "$(dirname "$restart_log")"

    # ── the optional native plugin ───────────────────────────────────────────
    # The connector carries most of the capability matrix out of process (RabbitMQ +
    # Postgres), so the .so is genuinely optional: entity-killed, death attribution, live
    # location and precise connect/disconnect are the only rows that need it.
    if [ "$part" = plugin ] || [ "$part" = both ]; then
        if ! deploy_dune_plugin "$plugin" "$dest" "$DUNE_RIG_LOCK" "$restart_log"; then
            plugin_rc=$?
            # A plugin failure NEVER blocks the sidecar. The sidecar is the connector; the
            # plugin is four rows of the matrix, and the rig must come up without it.
            ds_warn "the Dune plugin half failed (rc=${plugin_rc}) — the rig keeps running with whatever
  .so was already in place, every cell that needs it degrades, and the sidecar half continues."
        fi
    fi

    # ── the sidecar: its own build, its own container, its own failure mode ──
    if [ "$part" = sidecar ] || [ "$part" = both ]; then
        deploy_dune_sidecar "$sidecar"
    fi

    if [ "$part" = both ] && [ "$plugin_rc" != 0 ]; then
        ds_warn "sidecar deployed; the plugin was NOT updated (see the warning above)"
    fi
}

deploy_dune_plugin() {
    local plugin="$1" dest="$2" rig_lock="$3" restart_log="$4"
    if [ ! -f "${plugin}/Dockerfile.build" ] && [ ! -f "${plugin}/build.sh" ]; then
        ds_warn "Dune plugin source not present yet (${plugin}) — nothing to build."
        return 0
    fi

    # The BUILD happens OUTSIDE the rig lock: it takes ~30 s in a container and holding a
    # 900 s lock while compiling would block every other lane's deploy for no reason.
    ds_info "Building libtakaro-dune.so..."
    local built=1
    if [ -f "${plugin}/build.sh" ]; then
        ( cd "$plugin" && bash ./build.sh ) || built=0
    else
        if docker build -f "${plugin}/Dockerfile.build" -t takaro-dune-plugin-build:dev "$plugin"; then
            docker run --rm --user "$(id -u):$(id -g)" -v "${plugin}:/src" -w /src \
                takaro-dune-plugin-build:dev || built=0
        else
            built=0
        fi
    fi
    local so="${plugin}/dist/libtakaro-dune.so"
    if [ "$built" = "0" ] || [ ! -f "$so" ]; then
        ds_warn "libtakaro-dune.so did not build"
        return 1
    fi

    # LD_PRELOAD fails the WHOLE process when a preloaded object has an unresolvable
    # symbol (VEIN, 2026-09-17: five minutes of `Restarting (127)`). Refuse to ship one.
    local nm_out undef
    if ds_have nm; then
        nm_out="$(nm -D --undefined-only "$so" 2>/dev/null || true)"
    else
        nm_out="$(docker run --rm -v "$(dirname "$so")":/o:ro debian:bookworm bash -lc \
            "apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq binutils >/dev/null 2>&1 && nm -D --undefined-only /o/$(basename "$so")" 2>/dev/null || true)"
    fi
    if [ -z "$nm_out" ]; then
        ds_warn "could not read the dynamic symbol table of ${so} — refusing to deploy unchecked"
        return 1
    fi
    undef="$(printf '%s\n' "$nm_out" | awk '$1 == "U" { print $2 }' | grep -vE 'GLIBC|GCC|CXXABI' || true)"
    if [ -n "$undef" ]; then
        printf '%s\n' "$undef" >&2
        ds_warn "libtakaro-dune.so has undefined symbols outside the C/C++ runtime — LD_PRELOAD would crash-loop the map server"
        return 1
    fi
    ds_ok "nm -D: no undefined symbols outside the C/C++ runtime"
    cp "$so" "${dest}/libtakaro-dune.so.new"

    if ! ds_is_running dune; then
        # Nothing to restart: stage the file and let the next start pick it up.
        mv -f "${dest}/libtakaro-dune.so.new" "${dest}/libtakaro-dune.so"
        chmod 644 "${dest}/libtakaro-dune.so"
        ds_ok "${dest}/libtakaro-dune.so (staged; the rig is not running)"
        return 0
    fi

    # Swap + recreate survival as ONE command under the rig lock, so no other lane can
    # observe a half-swapped file or race the recreate.
    ds_info "Swapping the plugin and recreating ONLY survival, under the rig lock..."
    local label="${DUNE_RESTART_LABEL:-plugin swap via DUNE_DEPLOY_PART=plugin}"
    # The lock is held on a file DESCRIPTOR rather than through `flock -c "<script>"`, so
    # the swap and the recreate happen in this same shell while the lock is held once — no
    # quoting of a nested script, no re-entry, and no chance of the two halves running
    # under different lock holders.
    local rc=0
    exec 9>>"$rig_lock" || { ds_warn "cannot open the rig lock ${rig_lock}"; return 1; }
    if ! flock -w 900 9; then
        exec 9>&-
        ds_warn "timed out after 900 s waiting for the Dune rig lock — another lane holds it"
        return 1
    fi
    mv -f "${dest}/libtakaro-dune.so.new" "${dest}/libtakaro-dune.so" || rc=1
    chmod 644 "${dest}/libtakaro-dune.so" || rc=1
    if [ "$rc" = 0 ]; then
        printf '%s %s\n' "$(date -Is)" "$label" >> "$restart_log"
        ds_compose dune up -d --force-recreate --no-deps survival || rc=1
    fi
    exec 9>&-
    if [ "$rc" != 0 ]; then ds_warn "plugin swap under the rig lock failed"; return 1; fi
    ds_ok "${dest}/libtakaro-dune.so (survival recreated; postgres, both brokers, gateway and text-router untouched)"
    return 0
}

deploy_dune_sidecar() {
    local sidecar="$1"
    if [ ! -d "$sidecar" ]; then
        ds_warn "sidecar source not present yet (${sidecar}) — skipping"
        return 0
    fi
    ds_info "Rebuilding the Dune sidecar image..."
    DUNE_SIDECAR_SRC="$sidecar" ds_compose dune --profile sidecar build dune-takaro \
        || ds_die "Dune sidecar image build failed"
    if ! ds_is_running dune; then
        ds_ok "sidecar image built (the rig is not running, so nothing was recreated)"
        return 0
    fi
    # `--no-deps` is the whole point: without it compose would touch postgres and the brokers.
    DUNE_SIDECAR_SRC="$sidecar" ds_compose dune --profile sidecar up -d --force-recreate --no-deps dune-takaro \
        || ds_die "could not recreate takaro-dev-dune-sidecar"
    ds_ok "takaro-dev-dune-sidecar recreated (survival and the infrastructure untouched)"
}

# The catalog target is part of what the deployed artefacts are built from, so a re-pin
# counts as a source change.
ds_source_paths_dune() { echo "games/dune/mod games/dune/sidecar/src games/dune/version.txt catalog/dune"; }
ds_success_pattern_dune() { echo "Identified with Takaro (gameServerId="; }
