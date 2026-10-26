# Upgrading the Takaro Valheim Connector

The normal upgrade is in the [README](README.md#6-upgrading): stop the server, replace the
`BepInEx/plugins/TakaroValheim/` folder, delete `BepInEx/cache/chainloader_typeloader.dat` and
start again. From 4.3.0 on, the only config file is `BepInEx/config/com.takaro.valheim.cfg`, and
an upgrade never touches it. Coming from an older version, do the step for your version first.

## From 4.2.x

4.2.x read its settings from `BepInEx/plugins/TakaroValheim/takaro.cfg`. Versions after 4.3.0
move them over for you:

1. Stop the server.
2. Unzip the new version **over** `BepInEx/plugins/TakaroValheim/`. Do not delete the folder
   first: `takaro.cfg` in it holds your settings.
3. Delete `BepInEx/cache/chainloader_typeloader.dat` and start the server.

On the first start the connector writes the settings 4.2.x used (token, identity, server name,
URL, allowlists, log level, chat sender name) into `BepInEx/config/com.takaro.valheim.cfg` and
renames `takaro.cfg` to `takaro.cfg.migrated`. The log says
`carried the settings of the older connector version ... over`. From then on, edit only
`com.takaro.valheim.cfg`; you can delete `takaro.cfg.migrated`.

If the plugin folder was already deleted, the token, the identity and the server name in Takaro
are still kept (4.2.x saved them outside the folder). Settings that were only in `takaro.cfg`,
such as a non-default `takaroWsUrl`, the console command allowlist or `chatSenderName`, are back
at their defaults; set them again in `com.takaro.valheim.cfg`.

Upgraded to 4.3.0 itself? 4.3.0 does not move these settings: check `serverName`,
`takaroWsUrl`, the allowlists and `chatSenderName` in `com.takaro.valheim.cfg`, or upgrade to a
later version while `takaro.cfg` is still in the plugin folder.

## From 4.1.x or older (including 3.x)

These versions kept everything in `BepInEx/config/com.takaro.valheim.cfg` and, when
`identityToken` was empty, used `serverName` as the server's identity in Takaro. Versions after
4.3.0 keep that identity on the first start and rewrite the file in the current format; nothing
to do by hand. (4.3.0 itself instead wrote a new identity, and Takaro then saw a new server: on
4.3.0, set `identityToken` to the exact `serverName` value before you start it.)

From 3.x there is more: 4.x needs nothing on the game clients.

1. Players remove the old client companion mod (`TakaroValheimCompanion`); it no longer works
   with this server. For inventory, players can install the optional inventory mod instead
   (README, [step 7](README.md#7-optional-inventory-mod-for-players)).
2. The old `companionMode` config key is ignored and can be deleted.
