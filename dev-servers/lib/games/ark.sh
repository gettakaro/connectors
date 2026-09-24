#!/usr/bin/env bash
# Exactly pinned ARK Linux server, native LD_PRELOAD connector and Generic sidecar.
ds_register 125 'ark|ark.yml|-|ark ark-sidecar|24|30|sidecar|ARK: Survival Evolved 21241282 + native connector + Takaro sidecar|ark'

ds_target_game_ark() { printf 'ark'; }
ds_target_dest_ark() { printf '%s/server' "$(ds_data_dir ark)"; }

install_ark() {
    local target dest
    target="$(ds_target ark)"
    dest="$(ds_target_dest ark)"
    ds_write_target_env ark
    mkdir -p "$dest" "${DS_DATA}/ark/sidecar-data"
    ds_info "Installing exact ARK Steam target ${target}..."
    ds_maint install --game ark --target "$target" --dest "$dest"
    ds_info "Pulling catalog-pinned runtime image..."
    ds_compose ark pull
    "${DS_DIR}/scripts/deploy-connector.sh" ark
}

deploy_ark() {
    local target tmp
    target="$(ds_target ark)"
    ds_scratch_dir tmp
    ds_info "Building ARK native plugin and sidecar for ${target}..."
    ds_maint build --game ark --target "$target" \
        --version "$("${REPO_ROOT}/scripts/dev-version.sh" ark)" --out "$tmp"
    ds_maint deploy --game ark --target "$target" \
        --dest "$(ds_target_dest ark)" --from "$tmp/build-manifest.json"
    ds_ok "$(ds_target_dest ark)/TakaroArk"
}

ds_source_paths_ark() { echo "games/ark/mod/src games/ark/sidecar/src games/ark/sidecar/package-lock.json games/ark/scripts catalog/ark"; }
ds_success_pattern_ark() { echo "Identified with Takaro"; }
