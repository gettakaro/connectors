# Install and operate the native Dragonwilds connector

The Linux dedicated server loads `libtakaro-dragonwilds.so` into `RSDragonwildsServer-Linux-Shipping`
with `LD_PRELOAD`. The library connects directly to Takaro. Players use the unmodified game client.

## Clean installation

1. Create a Takaro **Generic** game server and copy its registration token. Stop the Dragonwilds
   server. Client and server must run the same game version.
2. Verify the release archive against the top-level `SHA256SUMS`, extract it, and verify
   `TakaroDragonwilds/SHA256SUMS` inside it. Keep `licenses/` and `THIRD-PARTY.md` with the install.
3. Put `libtakaro-dragonwilds.so` outside Steam's installation tree, for example
   `data/dragonwilds-plugin/`, mounted read-only at `/opt/takaro`. SteamCMD `validate` removes unknown
   files from the game tree.
4. Create a writable, persistent state directory, for example `data/dragonwilds-state`, mounted at
   `/opt/takaro-state`. Copy `env.example` to `.env` and set `TAKARO_REGISTRATION_TOKEN`,
   `TAKARO_IDENTITY_TOKEN`, `TAKARO_STATE_DIR` and `DRAGONWILDS_LOG_FILE`.
5. Apply `LD_PRELOAD` to the **game binary only**:

   ```bash
   LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
   ```

   Never export it globally or pass it to SteamCMD, which is 32-bit.
6. Start the one game service. Confirm the game PID maps `libtakaro-dragonwilds.so` and Takaro shows
   the server online. With `TAKARO_PLUGIN_TOKEN` set, authenticated diagnostics answer on
   `127.0.0.1:18890/health` inside the game's network namespace; the Takaro connection does not use it.

## Upgrade from the 0.2.x sidecar

The 0.2.x sidecar and the native connector must never use the same Takaro identity at once.

1. Schedule a maintenance window. Close new player ingress with a firewall or edge rule that survives
   recreation of the game container, and keep a timed mechanism that restores ingress.
2. Write an executable `fence-check` script that exits zero only while that rule is active, and a
   private `fence-proof.json`:

   ```json
   {"method":"edge firewall rule blocking new Dragonwilds UDP ingress","gamePort":7777,"expiresAtUtc":"<UTC within the next 30 minutes>","restoreCommand":"operator command that removes the rule"}
   ```

3. Wait until nobody is online, then run the drain with your container names, game port and the host
   paths of both state directories:

   ```bash
   install -d -m 700 ./dragonwilds-migration
   python3 TakaroDragonwilds/scripts/drain-legacy.py --evidence-dir ./dragonwilds-migration \
     --fence-proof ./fence-proof.json --fence-check ./fence-check \
     --game-container dragonwilds --sidecar-container dragonwilds-takaro --game-port 7777 \
     --legacy-state-dir ./data/dragonwilds-sidecar --native-state-dir ./data/dragonwilds-state
   ```

   It checks the fence at every sample and needs an empty plugin player list, no pending sidecar
   events, and the sidecar cursor equal to the plugin's latest event for three samples five seconds
   apart. It stops the sidecar, checks the plugin's event ring once more, stops the game, removes the
   sidecar container, and copies `event-cursor.json`, `online-players.json`, `known-players.json` and
   `timed-bans.json` into the native state directory. If the ring moved after the sidecar stopped, it
   restarts the sidecar and refuses the cutover.
4. Read `drain-report.json`. It states `exactBarrier: false`: an event between the last ring read and
   the game stop cannot be excluded. Keep the archived samples.
5. Remove the sidecar service from your Compose file so nothing recreates it. Replace the plugin,
   add the native settings, and start only the game. Reopen ingress after Takaro shows the server
   online, and record the cleanup.

If the game and the sidecar are already stopped, copy the state on its own:

```bash
python3 TakaroDragonwilds/scripts/drain-legacy.py --evidence-dir ./dragonwilds-migration --import-only \
  --legacy-state-dir ./data/dragonwilds-sidecar --native-state-dir ./data/dragonwilds-state
```

The import checks every file's format and refuses to overwrite different files or a state directory
the native connector has already used. Alternatively, point `TAKARO_STATE_DIR` at the old sidecar
state directory; the native connector reads the same files in place. The game's own ban list and the
plugin's `bans.json` stay where they are.

## Rollback

Stop admissions and let the native connector deliver its queued events (`/health` queues at zero).
Stop the game, keep a copy of the native state directory, then restore the 0.2.x plugin and sidecar
with the same Takaro identity and their old state directory. Never run both connectors at once, and
never copy an older `timed-bans.json` over a newer one.
