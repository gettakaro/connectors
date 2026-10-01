# Native connector state files

Everything the native connector persists lives in one directory, by default the plugin data
directory `<server>/RSDragonwilds/Binaries/Linux/takaro/` (next to `plugin.log`, `symcache.json`,
`bans.json` and the optional `plugin.json`). All writes are atomic: temporary file, `fsync`,
`rename`, `fsync` of the directory. The connector writes them from background workers only.

## Location

| Setting | Default | Meaning |
|---|---|---|
| `TAKARO_PLUGIN_DATA_DIR` | `<exe dir>/takaro` | plugin data directory (`bans.json`, logs, symcache) |
| `TAKARO_STATE_DIR` | the plugin data directory | `event-outbox.json`, `ban-intent.json`, and the default home of the four files below |
| `TAKARO_CURSOR_FILE` | `<state dir>/event-cursor.json` | the 0.2.x sidecar key; when set, its directory also becomes the default for the next three |
| `TAKARO_ONLINE_FILE` | `<cursor dir>/online-players.json` | |
| `TAKARO_BAN_FILE` | `<cursor dir>/timed-bans.json` | |
| `TAKARO_KNOWN_PLAYERS_FILE` | `<cursor dir>/known-players.json` | |

## Files shared with the 0.2.x sidecar (same names, same JSON shapes)

The sidecar kept these next to its cursor (`./data/` in the sidecar container, on the rig
`_data/dragonwilds-sidecar/`). Copying them once into the native state directory **before the first
native start** (with the sidecar stopped) is the whole migration; the connector reads them in place.

| File | Shape | Native role |
|---|---|---|
| `event-cursor.json` | `{"seq":452,"bootId":"0f0f0f0f0f0f0f0f"}` (`bootId` optional) | Read once when no `event-outbox.json` exists. Afterwards a derived mirror of the confirmed cursor. A cursor from an older server process is superseded at the first ring poll (new `bootId`). |
| `known-players.json` | `[{"gameId":"<puid>","name":"…","epicOnlineServicesId":"<puid>","platformId":"epic:<puid>",…}]` (IGamePlayer rows, ≤500) | Last-known players for offline `getPlayer` (F7). Rows without string `gameId` and `name` are ignored. |
| `online-players.json` | `[ IGamePlayer rows ]` | Players Takaro was told are online; anyone not on the server at the next reconcile gets a `player-disconnected` (crash recovery). |
| `timed-bans.json` | `[{"gameId":"<puid>","expiresAt":"2030-01-01T00:00:00.000Z","reason":"…"}]` (`reason` optional) | Timed-ban schedule. Every row must have a non-empty `gameId` and a valid ISO-8601 `expiresAt`; an existing corrupt or empty file is an explicit startup error, never an empty ban list. |

One-shot timed-ban import: the sidecar called the plugin's `/ban` without an expiry, so the
plugin's `bans.json` says permanent and the expiry exists only in `timed-bans.json`. On the first
native start each `timed-bans.json` row is merged into its `bans.json` record (`expiresAt`, and
`reason` when the record has none), `bans.json` is flushed, and `legacyBanMigrationDone:true` is
committed to the outbox, so the import never runs twice. A row whose id is missing from `bans.json`
is kept in the schedule (its expiry still clears the game's own `KnownPlayerList` flag) and reported
in `/health.native.behavior.lastError`. After the import `bans.json` is authoritative for expiries
and `timed-bans.json` is a derived mirror of it.

## Native-only files

| File | Shape |
|---|---|
| `event-outbox.json` | `{"version":1,"nextOutboxId":N,"scan":{"seq":S,"bootId":"…"},"confirmed":{"seq":C,"bootId":"…"},"deliveryLosses":L,"legacyBanMigrationDone":true,"derivedOnline":[…],"derivedKnown":[…],"pending":[{"outboxId":7,"source":{"seq":12,"bootId":"…"},"frame":"<gameEvent JSON text>"}]}` |
| `ban-intent.json` | `{"version":1,"intents":[{"version":1,"requestId":"…","gameId":"<puid>","action":"banPlayer","mutation":"ban\|unban","revision":R,"beforeBans":[{"gameId","expiresAt"}],"beforeRecord":{…}\|null,"desired":{"gameId","reason","expiresAt"}\|null}]}` — present only while a ban/unban is in flight or awaiting verification |

`event-outbox.json` is the authority for event delivery: an event is admitted there (durably)
before it is sent, and removed only after a WebSocket pong that follows its write on the same
connection confirms it. Unconfirmed events are re-sent after the next `identifyResponse`, including
across a process restart. Bounds: 5000 events / 32 MiB; the oldest are dropped first and counted in
`deliveryLosses`.

## Plugin enforcement list (unchanged from 0.2.x)

`bans.json`: `{"version":1,"bans":[{"gameId":"<puid>","name":"…","reason":"…","createdAt":"<iso>","expiresAt":"<iso>"|null}]}`.
A corrupt `bans.json` is never overwritten; the ban capabilities report the error instead.

## What a drain/import tool should do

1. Stop the 0.2.x sidecar (never run both against one Takaro identity).
2. Copy `event-cursor.json`, `known-players.json`, `online-players.json`, `timed-bans.json` from the
   sidecar data directory into the native state directory, unchanged. Leave `bans.json` alone (it is
   already in the plugin data directory).
3. Do not create `event-outbox.json` or `ban-intent.json`; the plugin creates them.
4. Start the server with the native plugin. `/health.native.stateVersion` is `1` and
   `/health.native.persistenceLastError` is empty once the import succeeded.
