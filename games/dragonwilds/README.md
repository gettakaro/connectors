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
cp TakaroDragonwilds/libtakaro-dragonwilds.so TakaroDragonwilds/takaro.cfg data/dragonwilds-plugin/
cp TakaroDragonwilds/env.example .env   # Docker only: the game server settings
```

Keep the library **outside the Steam tree**: SteamCMD `validate` deletes files it does not know. The
archive also holds `docker-compose.example.yml`, `INSTALL.md` (upgrade and rollback), the
`scripts/drain-legacy.py` upgrade tool and the licenses.

### 3. Configure

Open `takaro.cfg` next to the library (`data/dragonwilds-plugin/takaro.cfg`), paste the registration
token after `TAKARO_REGISTRATION_TOKEN=` and save. That is all: leave `TAKARO_IDENTITY_TOKEN` empty
(the connector creates it), and the server registers as `Dragonwilds (<8 characters>)` unless you set
`TAKARO_SERVER_NAME`. Saving the file is enough, also while the server runs: the connector picks the
change up within a few seconds, no restart.

Every key can also be set in the **game process** environment (`.env` with the Compose example);
a non-empty environment variable wins over `takaro.cfg` and needs a restart to change.

| Key | Required | What to put there |
|---|---|---|
| `TAKARO_REGISTRATION_TOKEN` | yes | The Generic game server's registration token. |
| `TAKARO_IDENTITY_TOKEN` | no | Leave empty. An install that already has one must keep it. |
| `TAKARO_SERVER_NAME` | no | Server name shown in Takaro; unique in your Takaro domain. |
| `TAKARO_WS_URL` | no | Default `wss://connect.takaro.io/`. |
| `TAKARO_SENDER_NAME` | no | Sender name for broadcasts; default is the server name. |
| `TAKARO_STATE_DIR` | Docker: yes | A persistent, writable directory, e.g. `/opt/takaro-state`. Default `<server>/RSDragonwilds/Binaries/Linux/takaro`. |
| `DRAGONWILDS_LOG_FILE` | no | The server log; found from the binary by default. |
| `DRAGONWILDS_LOG_TAIL` / `DRAGONWILDS_LOG_EVENTS` / `DRAGONWILDS_LOG_RATE` | no | `auto` / `filtered` / `40` (log lines per 30 s) by default. |
| `TAKARO_PLUGIN_TOKEN` | no | Enables loopback diagnostics on `127.0.0.1:18890`. |
| `TAKARO_CA_FILE` | no | Extra trusted CA file; certificate checks always stay on. |
| `TAKARO_TICK_BUDGET_US` | no | Game-thread time per tick for the connector, default `500`. |
| `TAKARO_SYM_PATH` | no | Only if the `.sym` file is not next to the binary. |

Never commit or share a filled-in `takaro.cfg` or `.env`.

### 4. Load the plugin into the server

Preload the library **on the game binary only**. SteamCMD is 32-bit and fails with a 64-bit preload,
so never set `LD_PRELOAD` for the whole container, user or service:

```bash
LD_PRELOAD=/opt/takaro/libtakaro-dragonwilds.so ./RSDragonwildsServer.sh -log
```

With Docker, mount `data/dragonwilds-plugin` (read-only is fine) at `/opt/takaro` and `data/dragonwilds-state`
at `/opt/takaro-state`; the image's entrypoint must apply `LD_PRELOAD="${TAKARO_PLUGIN_SO}"` to the
server launch line only (see `docker-compose.example.yml`). Start only the game service.

### 5. Verify

- `grep libtakaro /proc/<server pid>/maps` lists `libtakaro-dragonwilds.so`.
- The server console shows `[Takaro] connected to Takaro as "..."`, and Takaro shows the game server
  as **online** within a minute of the world loading.

Without a token, or when Takaro refuses the token or the server name, the console shows a
`[Takaro]` banner naming the file to fix (in Docker, `/opt/takaro/takaro.cfg` is
`data/dragonwilds-plugin/takaro.cfg`). Details: `plugin.log` in the plugin data directory
(`<server>/RSDragonwilds/Binaries/Linux/takaro/`).

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
upgrade the connector, stop the server, replace `libtakaro-dragonwilds.so`, keep your `takaro.cfg`,
`.env` and the state directory, and start again. If `takaro.cfg` was overwritten by the new empty
one, the connector takes the token, identity and name from its saved copy in the state directory.

## What works, what doesn't

✅ = works, ⚠️ = works with a caveat or not yet verified, ❌ = unsupported.

| What | | Notes |
|---|---|---|
| Connection & heartbeat | ✅ | Plugin connects to Takaro itself; no sidecar. |
| Player list | ✅ | Character name and EOS id. |
| Single player lookup | ⚠️ | Works online; Takaro never asks about offline players. |
| Player location | ✅ |  |
| Player inventory | ✅ |  |
| Give an item | ✅ | Appears in the inventory without a relog. |
| Item catalogue | ✅ | 1536 items with in-game names. |
| Entity catalogue | ⚠️ | Only creatures the server has loaded so far. |
| Locations / points of interest | ⚠️ | Lodestones only; Takaro does not use this list. |
| Run a console command | ✅ | Connector command set; unknown commands are refused. |
| Broadcast a message | ✅ | Shows under the receiving player's name. |
| Whisper a player | ✅ |  |
| Teleport a player | ✅ | Module teleports included. |
| Kick a player | ✅ |  |
| Ban a player (timed and permanent) | ✅ | Banned players are dropped about a second after joining. |
| Unban a player | ✅ | Works immediately, also after a restart. |
| Ban list | ✅ | Includes the expiry of timed bans. |
| Shut the server down | ✅ | Saves first, then stops. |
| Player joined event | ✅ |  |
| Player left event | ✅ |  |
| Player chat event | ✅ |  |
| Player death event | ✅ | Names the creature that killed the player. |
| Entity kill event | ✅ | Creature name and held weapon. |
| Log events | ✅ | Passwords removed; rate-limited to stay under Takaro's limit. |
| Map info | ❌ | Takaro has no map info for Generic game servers. |
| Map tiles | ❌ | Takaro has no map tiles for Generic game servers. |
| Modules: chat commands | ✅ |  |
| Modules: hooks | ✅ |  |
| Modules: cronjobs | ✅ |  |
| Modules: teleports (`@settp`, `@tp`, …) | ✅ |  |
| Modules: server messages / onboarding | ✅ |  |
| Shop: buy in game | ✅ |  |
| Shop: order in Takaro and claim in game | ✅ |  |
| Shop: bundle of several items | ✅ |  |
| Shop: order while offline, claim later | ✅ |  |
| Shop: not enough currency | ✅ | Refused, nothing deducted. |
| Economy: currency | ✅ |  |
| Economy: balance in game | ✅ |  |
| Discord: game chat → Discord | ✅ | Use the `chatBridgeNoEcho` module. |
| Discord: Discord → game chat | ⚠️ | Not yet verified with the native connector. |
| Discord: module hook / cronjob posts | ✅ |  |
| Discord: join/leave notices | ✅ |  |
| Discord: no echo of server messages | ✅ | Use the `chatBridgeNoEcho` module. |
| Events while the Takaro connection is down | ✅ | Queued on disk and delivered after reconnect. |
| Reconnects after a server or container restart | ✅ |  |
| No duplicate events after a connector restart | ✅ |  |
| Survives a network drop to Takaro | ✅ |  |
| Timed bans expire on their own | ✅ | Lifted by the connector, also across restarts. |
| Keeps running after a game update breaks a feature | ✅ | That feature reports degraded; the rest keeps working. |

### Known issues

- The server idles until `OwnerId` is set to the owner's EOS Player ID.
- Client and server are version-locked: update both together.
- Upgrading from the 0.2.x sidecar needs a short maintenance window.
- Broadcasts and whispers show under the receiving player's name, with a prefix.
- A banned player connects briefly before the plugin drops them again.
- Whispers are delivered; hiding them from other players is not yet verified.
- The entity catalogue only lists creatures the server has loaded so far.
- The server log holds the world password; the connector redacts it from Takaro.

---

Developers: see [DEVELOPMENT.md](DEVELOPMENT.md).
