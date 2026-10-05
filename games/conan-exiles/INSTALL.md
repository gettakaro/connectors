# Installing the Takaro Conan Exiles connector

The connector is one file that runs inside the Conan Exiles dedicated server: a library the
Linux server preloads, or a `winmm.dll` next to the Windows server exe. It connects to Takaro by
itself. There is no sidecar, no RCON and no mod, and players install nothing.

It works only on **Conan Exiles Dedicated Server build 25639945** (Steam app 443030, branch
`public`). On any other build it stays connected but refuses every action, and the log says why.

Download from the latest `conan-exiles-v*` release on
https://github.com/gettakaro/connectors/releases (the `Source code` links there are GitHub's own;
you do not need them):

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-25639945-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-25639945-<version>.zip` |

Each zip holds one folder, `TakaroConanNative/`.

You also need a Takaro game server of type **Generic** and its **registration token**.

## Linux

1. Stop the server.
2. Unzip `TakaroConanNative/` anywhere the server user can read, for example
   `/home/steam/TakaroConanNative/`.
3. Create the config folder and copy two files into it:

   ```bash
   mkdir -p <server>/ConanSandbox/Saved/Config/Takaro
   cp TakaroConanNative/takaro.json.example <server>/ConanSandbox/Saved/Config/Takaro/takaro.json
   cp TakaroConanNative/ca-certificates.crt <server>/ConanSandbox/Saved/Config/Takaro/
   chmod 600 <server>/ConanSandbox/Saved/Config/Takaro/takaro.json
   ```

4. Edit `takaro.json` (see [Configuration](#configuration)).
5. Start the server with the library preloaded. Keep your usual flags:

   ```bash
   LD_PRELOAD=/home/steam/TakaroConanNative/libtakaro-conan-native.so ./ConanSandboxServer.sh -log
   ```

   In Docker, set `LD_PRELOAD` in the server container's environment and mount the folder into
   the container. The library only activates in `ConanSandboxServer-Linux-Shipping`; the launcher
   script and other programs ignore it.

## Windows

1. Stop the server.
2. Copy `TakaroConanNative\winmm.dll` into `ConanSandbox\Binaries\Win64\`, next to
   `ConanSandboxServer-Win64-Shipping.exe`.
3. Create `ConanSandbox\Saved\Config\Takaro\` and copy `TakaroConanNative\takaro.json.example`
   into it as `takaro.json`.
4. Edit `takaro.json` (see [Configuration](#configuration)).
5. Start the server as usual. The DLL only activates in the server exe; anything else that loads
   it gets the normal Windows `winmm.dll` functions and nothing more.

## Configuration

`ConanSandbox/Saved/Config/Takaro/takaro.json`:

| Key | Value |
|---|---|
| `url` | `wss://connect.takaro.io/` |
| `identityToken` | A name you choose for this server, unique in your Takaro domain. Never change it later: Takaro keys the server on it. |
| `registrationToken` | The registration token from Takaro. |
| `name` | The server name Takaro shows. |
| `caFile` | Linux only: `Config/Takaro/ca-certificates.crt` (relative to `ConanSandbox/Saved`). Windows uses the system certificate store. |

Environment variables override the file: `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_WS_URL`, `TAKARO_SERVER_NAME`, `TAKARO_CA_FILE`. `TAKARO_CONAN_NATIVE_DISABLE=1` turns the
connector off without removing it.

**Keep RCON off.** The connector does not use it. In `Game.ini`:

```ini
[RconPlugin]
RconEnabled=0
```

and do not pass `-RconEnabled=1` on the command line.

## Check that it works

- `ConanSandbox/Saved/Logs/TakaroConanNative.log` shows `takaro: identified` within a minute of
  the map loading.
- Takaro shows the game server online, and **Test connection** reports it reachable.
- A bad config does not stop the server: the connector stays off and the log says why
  (`connector off: ...`).

The connector keeps its state (event queue, bans, health) in `ConanSandbox/Saved/Takaro/` and
`ConanSandbox/Saved/Config/Takaro/bans.json`. Keep both when you move or back up the server.

## Upgrading the connector

Stop the server, replace `libtakaro-conan-native.so` (Linux) or `winmm.dll` (Windows) with the new
one, start the server. `takaro.json` and the state folders stay as they are.

## Moving over from the old bridge

The Node.js bridge (`takaro-conan-exiles-bridge-*.zip`) is deprecated. It is still published for
one more release cycle and gets no fixes.

1. Stop the bridge (`npm start` process or its container) and the server. Never run the bridge
   and the native connector against the same Takaro game server at the same time.
2. Install the native connector as above, with the values from the bridge's `TakaroConfig.txt`:
   the same `registrationToken`, and as `identityToken` the bridge's `identityToken`, or its
   `serverName` if `identityToken` is empty (the bridge then identifies with the server name).
   That keeps the same Takaro game server. Players keep their Takaro records: both use the
   Steam64 id as the player id.
3. Turn RCON off (see above) and remove the bridge's log and RCON settings you no longer need.
4. Start the server and check the log line above.

Takaro keeps the item and entity rows the bridge synced (class names such as `PlantFiber`). Takaro
never deletes catalogue rows, so they stay next to the new rows with in-game names. Shop listings
that point at an old row keep working only if you re-create them from the new rows.

## Rollback

- **To the previous native version:** stop the server, put the previous
  `libtakaro-conan-native.so` or `winmm.dll` back, start the server.
- **To the bridge:** stop the server, remove `LD_PRELOAD` (Linux) or delete
  `ConanSandbox\Binaries\Win64\winmm.dll` (Windows), turn RCON back on for the bridge, and start
  the bridge again with its old `TakaroConfig.txt`.
- **Turn it off temporarily:** set `TAKARO_CONAN_NATIVE_DISABLE=1` in the server's environment.

## Removing it

Stop the server, remove `LD_PRELOAD` or delete `winmm.dll`, and delete
`ConanSandbox/Saved/Config/Takaro/` and `ConanSandbox/Saved/Takaro/`.
