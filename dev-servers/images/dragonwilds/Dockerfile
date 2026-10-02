# RuneScape: Dragonwilds dedicated server for the Takaro dev rig.
#
# Identical to the upstream image except for ONE change: the line in
# /home/ubuntu/entrypoint-wrapper.sh that launches the game binary is prefixed with
# LD_PRELOAD="${TAKARO_PLUGIN_SO:-}" so the Takaro native plugin is injected into the
# game process — and ONLY into the game process.
#
# Why not a blanket LD_PRELOAD on the container: the same wrapper shells out to
# SteamCMD, which runs 32-bit binaries. A global LD_PRELOAD of our 64-bit .so would
# make every steamcmd child spew "cannot open shared object file: wrong ELF class"
# warnings. Prefixing only the launch line keeps SteamCMD untouched.
#
# TAKARO_PLUGIN_SO may be empty or point at a file that does not exist yet (the
# plugin is a read-only bind mount that L1 fills in later). Both cases are benign:
# the dynamic loader prints one warning and starts the process anyway.
#
# Pinned by digest so a silent upstream :latest push can never change the wrapper
# under us (the sed below would then fail the build, which is the point).
FROM andyaltsys/dragonwilds-dedicated-server@sha256:2d9450565e7e86ea6c16c4250cf9d9b51a950aaa23909737ce78ab13f576b6f9

USER root

# Fail loudly if the upstream wrapper changed shape: exactly one launch line must match.
RUN set -eux; \
    f=/home/ubuntu/entrypoint-wrapper.sh; \
    pat='^\([[:space:]]*\)\./RSDragonwildsServer-Linux-Shipping '; \
    n="$(grep -c "$pat" "$f")"; \
    echo "launch lines matched: $n"; \
    [ "$n" = "1" ] || { echo "ERROR: expected exactly 1 server launch line in $f, found $n"; grep -n 'RSDragonwildsServer-Linux-Shipping' "$f"; exit 1; }; \
    sed -i "s|$pat|\\1LD_PRELOAD=\"\${TAKARO_PLUGIN_SO:-}\" ./RSDragonwildsServer-Linux-Shipping |" "$f"; \
    grep -n 'LD_PRELOAD' "$f"; \
    [ "$(grep -c 'LD_PRELOAD="\${TAKARO_PLUGIN_SO:-}" \./RSDragonwildsServer-Linux-Shipping ' "$f")" = "1" ] || { echo "ERROR: patch did not apply exactly once"; exit 1; }; \
    bash -n "$f"

ENV TAKARO_PLUGIN_SO=""

# Second change: the stock player monitor adds a player by platform name on join but removes
# by character name on quit, so the count never dropped and the idle-gated update and daily
# backup waited forever. Track players by network connection instead (see the fragment).
COPY player-tracking.fragment patch-player-tracking.pl /tmp/takaro-patch/
RUN set -eux; \
    LC_ALL=C perl /tmp/takaro-patch/patch-player-tracking.pl /home/ubuntu/entrypoint-wrapper.sh /tmp/takaro-patch/player-tracking.fragment; \
    grep -q 'NotifyAcceptedConnection' /home/ubuntu/entrypoint-wrapper.sh; \
    bash -n /home/ubuntu/entrypoint-wrapper.sh; \
    rm -rf /tmp/takaro-patch
