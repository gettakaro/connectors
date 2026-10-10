Takaro Dune: Awakening sidecar @VERSION@
Built for the self-hosted server package @REVISION@ (catalog target @TARGET@; see takaro-target.json).

This is the part that talks to Takaro, and on its own it already covers almost everything: it
reaches the battlegroup's Postgres (read-only), the GAME RabbitMQ for chat, and the GM command bus
for every write. The plugin archive is optional.

Before it can write anything, EVERY map server process needs both gates:
  -ini:engine:[ConsoleVariables]:server.NotificationSystem.Enabled=true
  -ini:engine:[FuncomLiveServices]:ServerCommandsAuthToken=<same value as DUNE_GM_AUTH_TOKEN>
and UserGame.ini needs the two commands the game does not allow by default:
  [AdminSetting.Global]
  +Allowed_GM_Commands=KickPlayer
  +Allowed_GM_Commands=ServiceBroadcast

Option A - Docker (what docker-compose.example.yml does):
1. Rename this folder to "sidecar". Move docker-compose.example.yml and .env.example out of it
   to beside it, and rename .env.example to .env. Keep .env outside "sidecar": upgrades replace it.
2. Fill in .env (leave TAKARO_IDENTITY_TOKEN empty: one is generated and kept in /data).
3. docker compose -f docker-compose.example.yml --env-file .env up -d --build
   No token yet? The log says so in a banner. Paste it into .env and save: the sidecar connects
   within a few seconds, no restart needed.

Option B - plain Node.js 22 on the host:
1. npm ci --omit=dev
2. Set at least DUNE_PG_URL, DUNE_RMQ_URL, DUNE_GM_AUTH_TOKEN and DUNE_GM_PUBLISHER in the
   environment (see .env.example). TAKARO_REGISTRATION_TOKEN can go in the environment or in
   ../.env (beside this folder), which is re-read every 5 s.
3. npm run catalogue      # builds the item catalogue; see data/ATTRIBUTION.md
4. node dist/index.js

Verify: the sidecar log prints "Identified with Takaro (gameServerId=...)", the server shows as
online in Takaro, and curl http://127.0.0.1:18891/health returns status "ok".

Never commit or share live registration tokens, GM auth tokens or plugin tokens.
