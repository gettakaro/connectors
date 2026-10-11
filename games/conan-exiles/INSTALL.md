# Installing the Takaro Conan Exiles connector

The connector is one file that runs inside the Conan Exiles dedicated server: a library the
Linux server preloads, or a `winmm.dll` next to the Windows server exe. It connects to Takaro by
itself. There is no sidecar, no RCON and no mod, and players install nothing.

It works only on **Conan Exiles Dedicated Server build 25792439** (Steam app 443030, branch
`public`) and the previous builds 25738716 and 25639945. On any other build it stays connected but refuses every action, and the log says why.

Download from the latest `conan-exiles-v*` release on
https://github.com/gettakaro/connectors/releases (the `Source code` links there are GitHub's own;
you do not need them):

| Server | Download |
|---|---|
| Linux | `takaro-conan-exiles-native-linux-25792439-<version>.zip` |
| Windows | `takaro-conan-exiles-native-windows-25792439-<version>.zip` |

Each zip holds one folder, `TakaroConanNative/`.

You also need a Takaro game server of type **Generic** and its **registration token**.

## Linux

1. Stop the server.
2. Unzip `TakaroConanNative/` anywhere the server user can read, for example
   `/home/steam/TakaroConanNative/`.
3. Copy the config into the server and paste your registration token into it:

   ```bash
   mkdir -p <server>/ConanSandbox/Saved/Config/Takaro
   cp TakaroConanNative/takaro.json <server>/ConanSandbox/Saved/Config/Takaro/takaro.json
   chmod 600 <server>/ConanSandbox/Saved/Config/Takaro/takaro.json
   ```

   Set `registrationToken` in that file. Leave `identityToken` empty.
4. Start the server with the library preloaded. Keep your usual flags:

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
3. Create `ConanSandbox\Saved\Config\Takaro\`, copy `TakaroConanNative\takaro.json` into it and set
   `registrationToken` in it. Leave `identityToken` empty.
4. Start the server as usual. The DLL only activates in the server exe; anything else that loads
   it gets the normal Windows `winmm.dll` functions and nothing more.

## Configuration

`ConanSandbox/Saved/Config/Takaro/takaro.json`. If it is missing, the connector creates it on the
first start. Saving the file is enough: the connector applies a changed token, identity, name or
URL within a few seconds, no restart needed.

| Key | Value |
|---|---|
| `registrationToken` | The registration token from Takaro. |
| `identityToken` | Leave empty: the connector fills in a unique one on the first start. Never change it later: Takaro keys the server on it. |
| `name` | The server name Takaro shows. Empty: `Conan Exiles (<first 8 identity characters>)`. Names are unique per Takaro domain. |
| `url` | `wss://connect.takaro.io/` |
| `caFile` | Optional, Linux: a PEM bundle (relative to `ConanSandbox/Saved`). Default: `ca-certificates.crt` next to the library. Windows uses the system certificate store. |

Environment variables override the file: `TAKARO_IDENTITY_TOKEN`, `TAKARO_REGISTRATION_TOKEN`,
`TAKARO_WS_URL`, `TAKARO_SERVER_NAME`, `TAKARO_CA_FILE`. A changed environment variable needs a
restart. `TAKARO_CONAN_NATIVE_DISABLE=1` turns the connector off without removing it.

**Keep RCON off.** The connector does not use it. In `Game.ini`:

```ini
[RconPlugin]
RconEnabled=0
```

and do not pass `-RconEnabled=1` on the command line.

## Check that it works

- The server console shows `Takaro: connected to Takaro as "<name>"` within a minute of the start
  (also in `ConanSandbox/Saved/Logs/TakaroConanNative.log` as `takaro: identified`).
- Takaro shows the game server online, and **Test connection** reports it reachable.
- Not connected: the server console shows a `****` block that says why (no token, a token Takaro
  refused, a name already taken, a file that is not valid JSON) and which file to fix. Fix it and
  save; no restart needed.

The connector keeps its state (event queue, bans, health) in `ConanSandbox/Saved/Takaro/` and
`ConanSandbox/Saved/Config/Takaro/bans.json`. Keep both when you move or back up the server.

## Upgrading the connector

Stop the server, replace `libtakaro-conan-native.so` and `ca-certificates.crt` (Linux) or
`winmm.dll` (Windows) with the new ones, start the server. Do not copy the new `takaro.json` over
yours. If you did, the connector restores the token, identity and name from
`ConanSandbox/Saved/Takaro/state/saved-settings.json` and keeps the same Takaro server.

## Rollback

- **To the previous native version:** stop the server, put the previous
  `libtakaro-conan-native.so` or `winmm.dll` back, start the server.
- **Turn it off temporarily:** set `TAKARO_CONAN_NATIVE_DISABLE=1` in the server's environment.

## Removing it

Stop the server, remove `LD_PRELOAD` or delete `winmm.dll`, and delete
`ConanSandbox/Saved/Config/Takaro/` and `ConanSandbox/Saved/Takaro/`.
