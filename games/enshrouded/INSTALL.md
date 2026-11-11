# Install and operate the Enshrouded connector

`dbghelp.dll` is loaded by `enshrouded_server.exe` in place of the system dbghelp. It hooks the game
and holds the connection to Takaro itself, so the whole connector is that one file plus its
configuration. Players use the unmodified game client.

It is built for Enshrouded 1.0 (game build **1287369**, Steam build 26430253; 1285601 and 1283531 also fully supported; 1273043 and 1267041 also known, without the item catalogue and `giveItem`) and tested under
GE-Proton11-9 in `mornedhels/enshrouded-server:dev-proton`. Older Proton images cannot start the 1.0
server. On a server still on build 1024233 the new DLL turns the item/creature catalogue and
`giveItem` off (degraded) because their tables are the 1.0 tables; use plugin 0.7.x there.

## Clean installation

1. Create a Takaro game server of type **Generic** and copy its registration token.
2. Check the release zip against the release's `SHA256SUMS`, unzip it, and check the files against
   `TakaroEnshrouded/SHA256SUMS` (`cd TakaroEnshrouded && sha256sum -c SHA256SUMS`). Keep
   `THIRD-PARTY.md` and `licenses/` with your installation record.
3. Stop the game server. Copy `dbghelp.dll` next to `enshrouded_server.exe`.
4. The configuration is one file, `takaro/plugin.json` next to `enshrouded_server.exe`. The
   connector creates it at the first start (all keys, an empty `registrationToken` and a unique
   `identityToken`); you can also create it before:

   ```json
   {
     "registrationToken": "<your registration token>",
     "identityToken": "",
     "name": "",
     "url": "wss://connect.takaro.io/",
     "protocol": 1,
     "serverChatName": "",
     "token": "",
     "mapDir": "",
     "caFile": ""
   }
   ```

   Paste the registration token into `registrationToken` and save. An empty `identityToken` gets a
   unique one written in (the file keeps its owner and permissions); an empty `name` means the
   server's own name plus the start of the identity (Takaro needs a different name for every game
   server in a domain). Optionally set `token` (a long random secret) for the diagnostics endpoint.
   Keep the file readable only by the account the server runs as: it holds a secret. The
   environment variables `TAKARO_REGISTRATION_TOKEN`, `TAKARO_IDENTITY_TOKEN`,
   `TAKARO_SERVER_NAME`, `TAKARO_WS_URL`, `TAKARO_SERVER_CHAT_NAME`, `TAKARO_CA_FILE` and `TAKARO_PLUGIN_TOKEN` override the
   file when they are set (a changed variable needs a restart). `caFile`/`TAKARO_CA_FILE` is only
   for a Takaro behind your own certificate authority; certificate and host-name checks always stay
   on, and a CA file that cannot be read stops the connection instead of falling back.
5. Under Wine/Proton set `WINEDLLOVERRIDES=dbghelp=n,b` on the game server process (see
   "Linux: Wine and Proton" below; without it the connector is never loaded). The Docker
   example in the repository does this, pins the image by digest, and mounts
   `server/enshrouded-updater` over the image's updater so the game is never updated underneath the
   plugin. Turn automatic game updates off however you host the server.
6. Start the server. The server console (stdout, where a panel or `docker logs` shows it) and
   `takaro/plugin.log` show:

   ```
   [Takaro] connecting to wss://connect.takaro.io/ as "<name>"
   [Takaro] connected to Takaro as "<name>"; the server shows as reachable in the Takaro dashboard
   ```

   Takaro shows the server online. Without a token, or when Takaro refuses the token or the name, a
   block of `*` lines names the problem and the file to edit. `plugin.log` names where each setting
   came from (`native: config registrationToken from plugin.json`), never its value.

`url`, `registrationToken`, `identityToken`, `name` and `protocol` in `takaro/plugin.json` apply without a
restart: the connector reads the file every 5 seconds and, when one of them changed, reconnects at
once; an emptied token disconnects. A half-saved or broken file is ignored (the console says so)
until it parses again. `serverChatName` also applies within a few seconds, without a reconnect. The other
keys (`caFile`, `token`, `mapDir`) are read at startup.

**Server chat name (optional).** Players see messages from Takaro (module replies, the Discord bridge,
broadcasts) as a notice that starts with the sender name, for example `Takaro: hello`. Takaro passes
the chat name set for the server in Takaro, and that name wins. On a Takaro version that passes no
name, put the name in `serverChatName` instead (a file from an older connector version has no such
key; add it). With neither, the notice shows the text alone. The game has no sender field for a
notice, so the name is written in front of the text, the same way in global chat and in a whisper.

With `token` set, `curl -H "Authorization: Bearer <token>" http://127.0.0.1:18890/health` from the
server's own network (inside the container for Docker) shows each capability's state and, under
`diagnostics.native`, the connection state, the event outbox and the timed bans. The port listens on
loopback only.

The connector keeps its state in `takaro/connector-state/` (`TAKARO_STATE_DIR` moves it): the event
outbox, the online and known players, timed bans and the ban journal. Keep that folder across
restarts and upgrades. A file there that is damaged is never overwritten or treated as empty: the
affected feature reports itself degraded until you fix or remove the file.

## Linux: Wine and Proton

`enshrouded_server.exe` imports `dbghelp.dll`. On Windows the copy next to the program loads before
the system one, which is how the connector gets in. Wine and Proton ship their own `dbghelp.dll` and
by default prefer it over a file of the same name in the game folder, so the connector is skipped:
no `takaro/plugin.log`, no `[Takaro]` lines in the console, and the server runs as if nothing were
installed. It is the same for any `dbghelp.dll` mod.

**Fix: tell Wine to load the connector first.** Set this on the game server process:

```text
WINEDLLOVERRIDES=dbghelp=n,b
```

`n,b` means native (the connector) first, Wine's own copy after it. When the game writes a crash
dump, the connector passes it on to Wine's own dbghelp, so crash reporting keeps working.

| How the server runs | Where to set it |
|---|---|
| Docker Compose | `environment:` of the game service (the [Docker example](docker-compose.example.yml) does this). |
| `docker run` | `-e WINEDLLOVERRIDES="dbghelp=n,b"` |
| Unraid template | Edit the container, add a Variable: key `WINEDLLOVERRIDES`, value `dbghelp=n,b`. |
| Pterodactyl, AMP or another panel | The server's environment variables / startup variables. |
| Your own script | `export WINEDLLOVERRIDES="dbghelp=n,b"` before `wine enshrouded_server.exe` or `proton run`. |

If the image or script already sets `WINEDLLOVERRIDES`, add the connector to that value with a
semicolon instead of replacing it, for example `dbghelp=n,b;mscoree,mshtml=`. Under Proton the
variable works the same way; it is how the connector is tested.

**When the variable cannot be set** (some images overwrite it in their start script), store the same
setting once in the server's Wine prefix. Run it as the user the server runs as, with the server's
`WINEPREFIX` (for Docker: `docker exec -it -u <user> <container> ...` with the image's prefix),
then restart the server:

```bash
WINEPREFIX=/path/to/prefix wine reg add 'HKCU\Software\Wine\AppDefaults\enshrouded_server.exe\DllOverrides' /v dbghelp /d native,builtin /f
```

The `AppDefaults` key applies to `enshrouded_server.exe` only. A prefix that is recreated (a new
container volume, a reinstall) loses it; the variable does not have that problem.

**Check that it loaded.** Start the server once with `WINEDEBUG=+loaddll` (added to the same
environment). A `Loaded` line for the `dbghelp.dll` in the server folder ending in `: native` means
the connector loaded. A line for `C:\windows\system32\dbghelp.dll` ending in `: builtin` instead
means Wine used its own copy: the setting did not reach the server process. Remove `WINEDEBUG` again afterwards; it makes the log large.

**Wine versions.** Wine development releases from 11.8 on usually load a `dbghelp.dll` like this one
without the setting (Wine now prefers a DLL in the game folder that does not claim to be from
Microsoft). Older Wine, including stable Wine 11.0, needs it. Set it on every Wine and Proton
version; it does no harm. The connector is tested under GE-Proton11-9; plain Wine is set up the
same way but is not part of the test run.

## Map

The connector speaks Takaro protocol 1 and declares one map, `embervale` ("Embervale"): x runs to the
right (east) and z upwards (north), from 0 to 10240 m on both axes. The dedicated server ships no map
picture, so Takaro first shows a grid with the player dots. To get a real map, upload one image on the
game server's map page in the Takaro dashboard ("upload map"). Leave the bounds empty: the connector
already declared them.

The image must show the **whole world, north up, square, edge to edge**: its top-left pixel is the
north-west corner of the world (x 0, z 10240) and its bottom-right pixel the south-east corner
(x 10240, z 0). PNG, JPEG or WebP, at most 8192 px per side and 32 MB; Takaro stretches it over the
world, so any square size works (8192 px gives about 1.25 m per pixel). The in-game map screen cannot
be exported as one picture; `mod/tools/map-tiles/render_tiles.py --image map.jpg` draws exactly this
image from the map data of your own game client install (see its README). Do not share an image made
from the game's assets publicly.

Takaro protocol 1 is proven on Takaro staging and not yet on takaro.io; today's takaro.io is a Takaro
without protocol 1. Set `"protocol": 0` in `takaro/plugin.json` (or `TAKARO_PROTOCOL=0`, which needs
a restart) to connect with protocol 0 only; a saved change reconnects within a few seconds. A Takaro
without protocol 1 is detected and gets protocol 0 automatically. On protocol 0 the map comes from a tile pack in `takaro/map/` instead (`map.json` plus
`<z>/<x>/<y>.png` in a `pack-…` folder named by `takaro/map/current`; `TAKARO_MAP_DIR` or `mapDir` in
`plugin.json` moves it), also rendered from your client with `render_tiles.py`. Without one,
protocol 0 shows no map.

## Upgrading

Stop the server, replace only `dbghelp.dll` with the new one, start the server. Your config in
`takaro/plugin.json` and the state in `takaro/connector-state/` are never touched. Events that had not
been delivered are sent after the restart.

## Upgrade from 0.5.0 (plugin + sidecar)

0.6.0 drops the sidecar: the DLL connects to Takaro itself. The sidecar and the new DLL must never
be connected with the same identity at the same time.

1. **Drain.** Pick a quiet moment, keep new players out (for example a temporary password), and wait
   until nobody is online. Leave the sidecar running meanwhile so it delivers the last events.
2. **Stop the sidecar** and make sure nothing restarts it (with Compose: `docker compose stop` it,
   then remove its service from the compose file). Then stop the game server.
3. **Move the settings.** Put the sidecar's registration token and identity into
   `takaro/plugin.json` (or into the game server's environment as `TAKARO_REGISTRATION_TOKEN` and
   `TAKARO_IDENTITY_TOKEN`). Use the **same identity** the sidecar used, so Takaro keeps the same game
   server. The old shared secret (`TAKARO_PLUGIN_TOKEN`) is now only needed for the diagnostics
   endpoint.
4. **Carry the state over.** Copy the sidecar's `event-cursor.json` and `online-players.json` (its
   data folder, `data/enshrouded-sidecar/` in the old compose example) into
   `takaro/connector-state/`. The connector imports the cursor once, at its first start.
5. **Replace the DLL** with the 0.6.0 `dbghelp.dll`, start the server, and check `plugin.log` for
   `imported sidecar event cursor` and `native: identified with Takaro`. Confirm the server is online
   in Takaro and that no sidecar process or container is left running. Then let players back in.

Timed bans are new in 0.6.0: bans made under 0.5.0 were permanent and stay permanent.

## Rollback to 0.5.0

Plan it like the upgrade, in reverse:

1. Keep new players out and wait until nobody is online. If you set `token`, check that
   `diagnostics.native.outbox.pending` in `/health` is `0`, so every event reached Takaro.
2. Check the ban list in Takaro. 0.5.0 does not know timed bans: a timed ban still active at the
   rollback stays in force until someone unbans the player. Lift or re-issue those bans first.
   (A rollback with an active timed ban has not been tested.)
3. Stop the game server. Put the 0.5.0 `dbghelp.dll` from `takaro-enshrouded-plugin.zip` of the
   `enshrouded-v0.5.0` release back, set its `TAKARO_PLUGIN_TOKEN` again, and start the 0.5.0 sidecar
   exactly as before, with the same identity. Do not start both connectors at once.
4. Keep `takaro/connector-state/` untouched for a later re-upgrade; it is not read by 0.5.0.

Re-upgrading later is the "Upgrade from 0.5.0" procedure again. The cursor is imported only once, so
drain before the switch rather than relying on it.
