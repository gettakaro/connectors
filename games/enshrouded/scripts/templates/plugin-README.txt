Takaro Enshrouded Connector @VERSION@

A server-side connector only: players do not install anything. dbghelp.dll is a proxy
the Enshrouded dedicated server loads in place of the system dbghelp; it hooks the game
and holds the connection to Takaro itself. There is nothing else to run.

It hooks the server through per-build game data (takaro/enshrouded-gamedata.json) and is
proven on Enshrouded 1.0, game build 1267041 (Steam app 2278520, branch public, build
25967645). On a game build without a section in that file the affected capabilities self-check as "degraded" at load; the server keeps running, and
Takaro's reachability reason names what no longer works.

Install (full steps, upgrade and rollback: INSTALL.md):
1. Stop the Enshrouded dedicated server (a running server holds dbghelp.dll open).
2. Copy dbghelp.dll next to enshrouded_server.exe.
3. The configuration is one file, takaro\plugin.json next to enshrouded_server.exe. The
   connector creates it at the first start; you can also create it yourself before:
     {
       "registrationToken": "<your registration token>",
       "identityToken": "",
       "name": "",
       "url": "wss://connect.takaro.io/",
       "token": ""
     }
   Leave "identityToken" empty: the connector writes a unique one in. Environment
   variables TAKARO_REGISTRATION_TOKEN and TAKARO_IDENTITY_TOKEN, when set, win over
   the file.
4. Under Wine/Proton (for example the mornedhels/enshrouded-server image) set
   WINEDLLOVERRIDES="dbghelp=n,b" so the server prefers this DLL; a Windows server
   loads the local file already.
5. Start the server. The server console shows
     [Takaro] connected to Takaro as "..."
   and the game server turns online in Takaro. A block of * lines says what to fix.

Pasting the token later is fine: save takaro\plugin.json while the server runs and
the connector connects within a few seconds, no restart needed.

Upgrade: stop the server, replace only dbghelp.dll, start it. Your config in
takaro\plugin.json is never touched.

"token" in plugin.json is optional: set it to a long random value to read the plugin's
diagnostics at http://127.0.0.1:18890/health (Authorization: Bearer <token>). The port
listens on loopback only.

SHA256SUMS lists every file in this folder. THIRD-PARTY.md and licenses/ cover the code
linked into dbghelp.dll.
