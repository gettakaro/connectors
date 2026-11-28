Takaro DayZ sidecar @VERSION@
Built for DayZ server build @REVISION@ (catalog target @TARGET@; see takaro-target.json).

The sidecar is the half of the connector that talks to Takaro. DayZ's script engine can make
HTTP requests but has no WebSocket client, so the @TakaroIntegration server mod calls this
sidecar over loopback HTTP (127.0.0.1:8088) and the sidecar holds the Takaro WebSocket.
You need both archives: this one and takaro-dayz-mod-*.zip.

Run the sidecar on the same host as the DayZ server, where the mod can reach 127.0.0.1:8088.

Option A - plain Node.js 22:
1. npm ci --omit=dev
2. Copy TakaroConfig.example.txt to TakaroConfig.txt and set registrationToken and
   serverName, or set TAKARO_REGISTRATION_TOKEN and TAKARO_SERVER_NAME in the environment
   (the environment wins).
3. node dist/index.js

Option B - Docker (what docker-compose.example.yml does):
1. Move docker-compose.example.yml out of this folder to beside it, and rename this folder
   to "sidecar".
2. Put TAKARO_REGISTRATION_TOKEN in a .env file beside the compose file.
3. docker compose -f docker-compose.example.yml up -d --build
   The container uses the host network, so the mod's 127.0.0.1:8088 reaches it. For a DayZ
   server that itself runs in a container, use network_mode: "service:<dayz service>" instead.

The mod's profiles/TakaroIntegration/config.json must point TakaroApiUrl at
http://127.0.0.1:<localPort> (8088 by default).

Verify: the sidecar log prints "Identified as gameServerId=...", the server shows as online
in Takaro, and curl http://127.0.0.1:8088/health answers.

Never commit or share a live registration token.
