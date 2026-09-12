# Project Zomboid connector — client-dependent live-proof checklist

The M1 server-side surface (identify, reachability, getPlayers, console command,
global sendMessage, reconnect) is proven without a client. The items below
need a **real PZ client connected from the gamer PC** and were deferred during
M1 because the client on the gamer PC had a render-thread stall (see the M0
evidence doc). Run them once a client is repaired and connected.

Setup: client joins `192.168.129.92:16261` + server password, windowed + low
graphics. Drive actions from the Takaro side (REST API or dashboard). Watch
`/home/steam/Zomboid/Takaro/takaro-agent.log` and the Takaro `eventSearch`.

| # | Takaro action / trigger | Expected in-game / log result |
| --- | --- | --- |
| 1 | Client joins | agent log `HOOK CONFIRMED: connect`, then `event: player-connected <user>`; Takaro `player-connected` event |
| 2 | Client quits | agent log `HOOK CONFIRMED: disconnect`, `event: player-disconnected <user>`; Takaro `player-disconnected` event |
| 3 | Client types in chat | agent log `HOOK CONFIRMED: chat`; Takaro `chat-message` event with the text + channel |
| 4 | Client dies (godmode off, zombie kill) | agent log `HOOK CONFIRMED: death`, `event: player-death`; Takaro `player-death` event (position present) |
| 5 | `POST /gameserver/{id}/message` with `opts.recipient.gameId=<user>` | private message visible only to that client |
| 6 | `POST /gameserver/{id}/player/{playerId}/kick` | client disconnected; rejoin works |
| 7 | `POST /gameserver/{id}/player/{playerId}/ban` (permanent) | client disconnected and cannot rejoin; `GET /gameserver/{id}/bans` lists it |
| 8 | ban with 2-min expiry | after ~2 min the reconciler logs `timed ban expired … unbanning` and the player can rejoin |
| 9 | `POST /gameserver/{id}/player/{playerId}/unban` | previously-banned client can rejoin |
| 10 | `POST /gameserver/{id}/player/{playerId}/teleport` `{x,y,z}` | client visibly moves; `getPlayerLocation` (via getPlayers) reports ~the new coords |
| 11 | `POST /gameserver/{id}/player/{playerId}/giveItem` `Base.Axe` | axe appears in the client inventory without a relog |
| 12 | `GET /gameserver/{id}/players` while client online | returns the player with gameId=username, steamId, platformId `steam:<id>`, ip, ping |

Oracles: `Zomboid/db/*.db` `SELECT * FROM bannedid` for steam-id bans;
`Zomboid/Takaro/bans.json` for Takaro-issued bans with expiry;
`Zomboid/Logs/*_user.txt` appears on first join.
