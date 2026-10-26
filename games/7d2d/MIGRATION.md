# Moving from Takaro's built-in 7 Days to Die integration (Connector Migration)

A game server that Takaro already runs through its built-in 7 Days to Die integration can move to
this connector **in place**: it stays the same game server in Takaro, with its players, currency,
roles, bans, shop and modules. Nothing is exported or re-imported, and no second game server is
created.

This needs connector version `7d2d-v0.4.0` or newer, and a Takaro that offers Connector Migration
(the **Migrate to connector** action on the game server). The connector tells Takaro that it can
take over a 7 Days to Die server (`migration.native` for game `7d2d`); Takaro refuses the migration
for any connector that does not.

1. In Takaro, open the existing 7 Days to Die game server and choose **Migrate to connector**.
   Takaro shows a registration token and an identity token. The server keeps running on the
   built-in connection while the migration is pending, and you can cancel it there at any time.
2. Stop the game server and install the connector ([README](README.md), Install steps 2 and 3).
3. Put both tokens in `<server>/Takaro/Config.xml`:

   ```xml
   <Takaro>
     <WebSocket>
       <Url>wss://connect.takaro.io/</Url>
       <IdentityToken>identity-token-from-takaro</IdentityToken>
       <RegistrationToken>registration-token-from-takaro</RegistrationToken>
       <Enabled>true</Enabled>
       <ReconnectIntervalSeconds>30</ReconnectIntervalSeconds>
     </WebSocket>
   </Takaro>
   ```

   If the file already exists because the connector ran before, **replace** the `IdentityToken` it
   generated with the one Takaro issued. Keeping the generated one makes Takaro create a second,
   separate game server.
4. Start the server. The connector's log shows
   `Takaro accepted identify: game server <id>, protocol version 1`, and the same game server in
   Takaro is now connector-based.
5. Check the migration report on the game server in Takaro: it lists which players were matched to
   existing profiles and which are new. A large number of new players means something is wrong; revert.
6. To revert, use Takaro within 14 days of the migration. Afterwards stop the server and remove
   `Mods/Takaro` (or set `<Enabled>false</Enabled>`); otherwise the connector connects again with the
   same identity token and Takaro registers it as a new, separate game server.

Players already known to Takaro from the built-in integration are recognised by their Steam/Epic id
and keep their profile. If an older version of this connector already created a second profile for
a player (one with an empty Steam ID), delete that second profile in Takaro once; until then Takaro
ignores that player's events.

If Takaro refuses the connection the log says why:

```
Takaro rejected identify (unsupported): Connector does not support migration of this GameServer: ...
```

means the connector is too old for Connector Migration: update it. A second game server appearing
in Takaro means the `IdentityToken` was not replaced in step 3: delete the new game server, paste
Takaro's token and restart (two game servers cannot be merged).
