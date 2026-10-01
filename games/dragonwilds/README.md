# Takaro RuneScape: Dragonwilds Connector

The RuneScape: Dragonwilds **Linux dedicated server** loads one native `libtakaro-dragonwilds.so`
connector. It connects directly to Takaro; there is no sidecar. Players install nothing and keep the
vanilla game client. The library must be preloaded on the game binary's launch line.

## Install

### 1. Prepare the server

You need a Dragonwilds **Linux dedicated server** (Steam app 4019830) whose launch command you can
change, with its `RSDragonwildsServer-Linux-Shipping.sym` file next to the server binary (SteamCMD
downloads it; `app_update 4019830 validate` restores it). Create a Takaro **Generic** game server and
copy its registration token. Client and server must run the same game version.

### 2. Download and copy

Download `takaro-dragonwilds-plugin.tar.gz` and `SHA256SUMS` from the latest release at
<https://takaro.io/connectors/dragonwilds>. Do not use the "Source code" archives. Check and unpack:

```bash
sha256sum -c SHA256SUMS --ignore-missing
tar -xzf takaro-dragonwilds-plugin.tar.gz
mkdir -p data/dragonwilds-plugin data/dragonwilds-state
cp TakaroDragonwilds/libtakaro-dragonwilds.so data/dragonwilds-plugin/
cp TakaroDragonwilds/env.example .env
```

Keep the library **outside the Steam tree**: SteamCMD `validate` deletes files it does not know. The
archive also holds `docker-compose.example.yml`, `INSTALL.md` (upgrade and rollback), the
`scripts/drain-legacy.py` upgrade tool and the licenses.

### 3. Configure

Set these in the **game process** environment (`.env` with the Compose example):

| Key | Required | What to put there |
|---|---|---|
| `TAKARO_REGISTRATION_TOKEN` | yes | The Generic game server's registration token. |
| `TAKARO_IDENTITY_TOKEN` | yes | A stable name for this server, e.g. `my-dragonwilds`. |
| `TAKARO_WS_URL` | no | Default `wss://connect.takaro.io/`. |
| `TAKARO_SERVER_NAME` | no | Server name shown in Takaro. |
| `TAKARO_SENDER_NAME` | no | Sender name for broadcasts; default is the server name. |
| `TAKARO_STATE_DIR` | yes | A persistent, writable directory, e.g. `/opt/takaro-state`. |
| `DRAGONWILDS_LOG_FILE` | yes | The server log, `<server>/RSDragonwilds/Saved/Logs/RSDragonwilds.log`. |
| `DRAGONWILDS_LOG_TAIL` / `DRAGONWILDS_LOG_EVENTS` | no | `auto` / `filtered` by default. |
| `TAKARO_PLUGIN_TOKEN` | no | Enables loopback diagnostics on `127.0.0.1:18890`. |
| `TAKARO_CA_FILE` | no | Extra trusted CA file; certificate checks always stay on. |
| `TAKARO_TICK_BUDGET_US` | no | Game-thread time per tick for the connector, default `500`. |
| `TAKARO_SYM_PATH` | no | Only if the `.sym` file is not next to the binary. |

Never commit or share a filled-in `.env`.

### 4. Load the plugin into the server

Preload the library **on the game binary only**. SteamCMD is 32-bit and fails with a 64-bit preload,
so never set `LD_PRELOAD` for the whole container, user or service:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount `data/dragonwilds-plugin` read-only at `/opt/takaro` and `data/dragonwilds-state`
at `/opt/takaro-state`; the image's entrypoint must apply `LD_PRELOAD="${TAKARO_PLUGIN_SO}"` to the
server launch line only (see `docker-compose.example.yml`). Start only the game service.

### 5. Verify

- `grep libtakaro /proc/<server pid>/maps` lists `libtakaro-dragonwilds.so`.
- Takaro shows the game server as **online** within a minute of the world loading.

If it stays offline, re-check the registration token, then `<server>/RSDragonwilds/Binaries/Linux/takaro/plugin.log`.

### 6. Upgrade from the 0.2.x sidecar

The old sidecar and the native connector must never run with the same Takaro identity at once. Close
new joins with a firewall rule, wait until nobody is online, then drain and import the sidecar's state:

```bash
install -d -m 700 ./dragonwilds-migration
python3 TakaroDragonwilds/scripts/drain-legacy.py --evidence-dir ./dragonwilds-migration \
  --fence-proof ./fence-proof.json --fence-check ./fence-check \
  --game-container dragonwilds --sidecar-container dragonwilds-takaro --game-port 7777 \
  --legacy-state-dir ./data/dragonwilds-sidecar --native-state-dir ./data/dragonwilds-state
```

It stops the sidecar and the game only when every event is delivered, removes the sidecar container,
and copies the cursor, player lists and timed bans into the native state directory. Then delete the
sidecar service from your Compose file, replace the plugin, add the settings above, and start the game.
`INSTALL.md` explains the fence files, a manual import and rollback.

**After a game update** you normally do nothing: the plugin re-reads the new `.sym` file on start. To
upgrade the connector, stop the server, replace `libtakaro-dragonwilds.so`, keep the state directory,
and start again.

## What works, what doesn't

The native connector has not been re-tested end to end yet; every row below awaits that run.
✅ = works, ⚠️ = works with a caveat or not yet verified, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ⚠️ | Pending native re-test. |
| Player list | ⚠️ | Pending native re-test. |
| Single player lookup | ⚠️ | Pending native re-test. |
| Player location | ⚠️ | Pending native re-test. |
| Player inventory | ⚠️ | Pending native re-test. |
| Give an item | ⚠️ | Pending native re-test. |
| Item catalogue | ⚠️ | Pending native re-test. |
| Entity catalogue | ⚠️ | Pending native re-test. |
| Locations / points of interest | ⚠️ | Pending native re-test. |
| Run a console command | ⚠️ | Pending native re-test. |
| Broadcast a message | ⚠️ | Pending native re-test. |
| Whisper a player | ⚠️ | Pending native re-test. |
| Teleport a player | ⚠️ | Pending native re-test. |
| Kick a player | ⚠️ | Pending native re-test. |
| Ban a player (timed and permanent) | ⚠️ | Pending native re-test. |
| Unban a player | ⚠️ | Pending native re-test. |
| Ban list | ⚠️ | Pending native re-test. |
| Shut the server down | ⚠️ | Pending native re-test. |
| Player joined event | ⚠️ | Pending native re-test. |
| Player left event | ⚠️ | Pending native re-test. |
| Player chat event | ⚠️ | Pending native re-test. |
| Player death event | ⚠️ | Pending native re-test. |
| Entity kill event | ⚠️ | Pending native re-test. |
| Log events | ⚠️ | Pending native re-test. |
| Map info | ❌ | Takaro has no map info for Generic game servers. |
| Map tiles | ❌ | Takaro has no map tiles for Generic game servers. |
| Modules: chat commands | ⚠️ | Pending native re-test. |
| Modules: hooks | ⚠️ | Pending native re-test. |
| Modules: cronjobs | ⚠️ | Pending native re-test. |
| Modules: teleports (`@settp`, `@tp`, …) | ⚠️ | Pending native re-test. |
| Modules: server messages / onboarding | ⚠️ | Pending native re-test. |
| Shop: buy in game | ⚠️ | Pending native re-test. |
| Shop: order in Takaro and claim in game | ⚠️ | Pending native re-test. |
| Shop: bundle of several items | ⚠️ | Pending native re-test. |
| Shop: order while offline, claim later | ⚠️ | Pending native re-test. |
| Shop: not enough currency | ⚠️ | Pending native re-test. |
| Economy: currency | ⚠️ | Pending native re-test. |
| Economy: balance in game | ⚠️ | Pending native re-test. |
| Discord: game chat → Discord | ⚠️ | Pending native re-test. |
| Discord: Discord → game chat | ⚠️ | Pending native re-test. |
| Discord: module hook / cronjob posts | ⚠️ | Pending native re-test. |
| Discord: join/leave notices | ⚠️ | Pending native re-test. |
| Discord: no echo of server messages | ⚠️ | Pending native re-test; use the `chatBridgeNoEcho` module. |
| Events while the Takaro connection is down | ⚠️ | Pending native re-test. |
| Reconnects after a server or container restart | ⚠️ | Pending native re-test. |
| No duplicate events after a connector restart | ⚠️ | Pending native re-test. |
| Survives a network drop to Takaro | ⚠️ | Pending native re-test. |
| Timed bans expire on their own | ⚠️ | Pending native re-test. |
| Keeps running after a game update breaks a feature | ⚠️ | Pending native re-test. |

### Known issues

- The server idles until `OwnerId` is set to the owner's EOS Player ID.
- Client and server are version-locked: update both together.
- Upgrading from the 0.2.x sidecar needs a short maintenance window.
- Broadcasts and whispers show under the receiving player's name, with a prefix.
- A banned player connects briefly before the plugin drops them again.
- The entity catalogue only lists creatures the server has loaded so far.
- The server log holds the world password; the connector redacts it from Takaro.
- A game update can switch off one feature; the rest keeps working.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
