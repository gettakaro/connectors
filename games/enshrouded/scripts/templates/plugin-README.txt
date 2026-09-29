Takaro Enshrouded Connector @VERSION@

A server-side connector only: players do not install anything. dbghelp.dll is a proxy
the Enshrouded dedicated server loads in place of the system dbghelp; it hooks the game
and holds the connection to Takaro itself. There is nothing else to run.

It hooks the server by pinned code signatures and is proven on Enshrouded game build
1024233 (Steam app 2278520, branch public, build 23178631). On any other game build the
affected capabilities self-check as "degraded" at load; the server keeps running, and
Takaro's reachability reason names what no longer works.

Install (full steps, upgrade and rollback: INSTALL.md):
1. Stop the Enshrouded dedicated server (a running server holds dbghelp.dll open).
2. Copy dbghelp.dll and the takaro folder next to enshrouded_server.exe.
3. Rename takaro\plugin.json.example to takaro\plugin.json and fill in
   "registrationToken" (from your Takaro Generic game server) and "identityToken"
   (any stable name for this server). Environment variables TAKARO_REGISTRATION_TOKEN
   and TAKARO_IDENTITY_TOKEN, when set, win over the file.
4. Under Wine/Proton (for example the mornedhels/enshrouded-server image) set
   WINEDLLOVERRIDES="dbghelp=n,b" so the server prefers this DLL; a Windows server
   loads the local file already.
5. Start the server. <server>\takaro\plugin.log shows
     takaro enshrouded plugin @VERSION@ starting (pid ...)
     native: identified with Takaro gameServerId=...
   and the game server turns online in Takaro.

"token" in plugin.json is optional: set it to a long random value to read the plugin's
diagnostics at http://127.0.0.1:18890/health (Authorization: Bearer <token>). The port
listens on loopback only.

SHA256SUMS lists every file in this folder. THIRD-PARTY.md and licenses/ cover the code
linked into dbghelp.dll.
