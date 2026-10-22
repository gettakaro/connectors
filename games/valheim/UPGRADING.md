# Upgrading the Takaro Valheim Connector

The normal upgrade is in the [README](README.md#6-upgrading): stop the server, replace the
`BepInEx/plugins/TakaroValheim/` folder, delete `BepInEx/cache/chainloader_typeloader.dat` and
start again. From 4.3.0 on, the only config file is `BepInEx/config/com.takaro.valheim.cfg`, and
an upgrade never touches it. Coming from an older version, do the step for your version first.

## From 4.2.x

4.2.x read its settings from `BepInEx/plugins/TakaroValheim/takaro.cfg`. 4.3.0 and later do not
read that file, and deleting the plugin folder deletes it.

4.2.x already copied the registration token and the server's identity into
`com.takaro.valheim.cfg`, so the server reconnects as the same server in Takaro. Every other
setting in that file is a default. Before you delete the folder:

1. Open `BepInEx/plugins/TakaroValheim/takaro.cfg` and note every setting you changed:
   `serverName`, `takaroWsUrl`, `logLevel`, `enableLogEvents`, `commandAllowlistExact`,
   `commandAllowlistPrefixes`, `chatSenderName`.
2. Open `BepInEx/config/com.takaro.valheim.cfg` and set those keys to the same values. Check
   that `registrationToken` and `identityToken` are filled in; if not, copy them over as well.
3. The comment at the top of that file still says to edit `takaro.cfg` instead. That no longer
   applies; you can delete those comment lines.

Without step 2, `serverName` becomes `Valheim Server`, and the console command allowlist and
`chatSenderName` go back to their defaults.

## From 4.1.x or older (including 3.x)

These versions already kept everything in `BepInEx/config/com.takaro.valheim.cfg`. When its
`identityToken` was empty, they used `serverName` as the server's identity in Takaro. 4.3.0 and
later instead write a new identity into an empty `identityToken`, and Takaro would then see a new
server. Before you upgrade, open the file and, if `identityToken` is empty, set it to the exact
`serverName` value:

```ini
serverName = My Valheim Server
identityToken = My Valheim Server
```

From 3.x there is more: 4.x needs nothing on the game clients.

1. Players remove the old client companion mod (`TakaroValheimCompanion`); it no longer works
   with this server. For inventory, players can install the optional inventory mod instead
   (README, [step 7](README.md#7-optional-inventory-mod-for-players)).
2. The old `companionMode` config key is ignored and can be deleted.
