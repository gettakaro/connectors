# Takaro-Owned Valheim Client Companion Design

**Date:** 2026-07-11
**Status:** Approved
**Target:** `gettakaro/connectors` Valheim connector

## Problem

The released Valheim v1.0.0 connector deliberately runs only in the dedicated-server process. That boundary proves server-owned actions and state, but Valheim does not expose reliable remote-player inventory, vanilla inbound chat, rich player-death context, or player-attributed entity kills to a dedicated-server-only plugin.

The connector also contains server-side implementations for `getPlayer`, moderation, and shutdown that require exact live proof rather than client code. The standard Takaro `listLocations` route is an upstream Takaro limitation and is not solved by a client mod.

## Approved Product Decisions

- Takaro will own the client plugin, server bridge, wire protocol, validation, packaging, documentation, and tests.
- Every player must install the companion for a production server using companion-backed capabilities.
- BepInEx is the only separately installed runtime dependency. The design does not depend on Jotunn, ServerSync, or another gameplay mod.
- The client never receives a Takaro registration token, identity token, WebSocket endpoint, or other cloud credential.
- V1 companion scope is general chat and `$` commands, inventory snapshots, player-death events, and player-attributed entity-killed events.
- Client map rendering is deferred. Server-owned teleport, world-drop `giveItem`, messaging, location, catalogs, moderation, and shutdown remain in the server connector.
- Client reports are functional integration data, not anti-cheat-authoritative evidence. Documentation and capability notes must say so explicitly.

## Architecture

```text
Takaro.Valheim.Companion.dll (every player)
  - BepInEx/Harmony hooks
  - no Takaro credentials or cloud connection
  - chat/command, inventory, death, kill observation
                 |
                 | versioned, bounded ZRoutedRpc messages
                 v
TakaroValheim.dll (dedicated server only)
  - handshake and required-client enforcement
  - derives identity from the real RPC sender/Valheim peer
  - validates, rate-limits, deduplicates, and caches reports
  - forwards accepted actions/events through the existing Takaro WebSocket
                 |
                 v
              Takaro
```

The implementation uses separate client and server BepInEx plugins. A small game-independent companion protocol project contains envelopes, version rules, limits, validation, and cache/deduplication primitives shared by both plugins. It contains no Takaro credentials or network client.

## Components

### Companion protocol

The protocol defines a stable GUID, protocol version, supported version range, capability flags, maximum payload sizes, sequence identifiers, event identifiers, and these versioned messages:

- `hello` / `hello-ack` / `hello-nack`
- `heartbeat`
- `chat`
- `inventory-snapshot`
- `player-death`
- `entity-killed`

Messages use bounded serialized envelopes over Valheim's existing routed RPC transport. The server never trusts a player identifier supplied in the payload; it resolves the RPC sender to the connected `ZNetPeer` and Takaro player.

### Client companion

The client plugin:

- loads only in the graphical Valheim client;
- registers the protocol and responds to the server nonce/version handshake;
- forwards ordinary local-player chat while leaving Valheim's normal chat behavior intact;
- intercepts configured `$` commands, forwards them once, and prevents accidental duplicate normal-chat delivery;
- hashes the local inventory periodically, sends an immediate snapshot after readiness, sends changed snapshots, and refreshes unchanged state before the server cache expires;
- observes the local player's death and emits one event identifier per death;
- observes `Character.OnDeath`, emits an entity kill only when the local player is the validated attacker, and includes bounded entity/weapon/position data;
- sends a heartbeat while the local player remains connected.

### Dedicated-server bridge

The existing server plugin:

- enables the companion bridge through explicit configuration;
- sends a nonce and supported protocol range to each ready peer;
- records the negotiated companion version/capabilities by peer and character session;
- rejects payloads before successful negotiation;
- maps every report from the transport sender rather than a claimed identity;
- keeps only fresh, bounded inventory snapshots and suppresses unavailable inventory rather than fabricating `[]`;
- emits accepted chat, death, and entity-killed events through the existing Takaro event path;
- removes all companion state on disconnect/world change;
- sends a built-in Valheim explanation and disconnects a player who misses the handshake deadline, has an incompatible protocol, or loses heartbeats beyond the grace period while required mode is active.

## Compatibility and Enforcement

The Takaro-owned handshake mirrors normal Valheim mod-compatibility behavior without taking a Jotunn/ServerSync dependency.

- `disabled`: companion RPCs are not registered and v1.0.0 server-only behavior remains.
- `optional`: compatible clients contribute client-owned capabilities; other clients may join but are reported unavailable.
- `required`: every player must negotiate the configured protocol range within a short grace period.

The production package documents `required` as the intended mode. A clear in-game message names the missing/incompatible companion and expected version before disconnect. Product versions and wire-protocol versions are separate so compatible patch releases do not force needless disconnects.

## Validation, Abuse Controls, and Trust

- Per-message byte, string, array, item-count, and numeric bounds.
- Per-peer token-bucket rate limits by message type.
- Monotonic per-session sequence numbers and bounded event-ID deduplication.
- Nonce-bound session negotiation to reject stale/replayed session messages.
- Inventory freshness TTL and maximum item count; malformed or stale snapshots never become Takaro inventory state.
- Chat type/prefix validation and maximum message length.
- Death and kill validation against online peer identity, server-known position/time windows, entity shape, and duplicate suppression.
- Rate-limited actionable logs without dumping inventory contents, chat bodies, credentials, or raw protocol payloads.

A modified client can still lie about its own inventory or observed combat. This is documented as `client-reported`; it is not used for bans or anti-cheat decisions.

## Error Handling

- Missing/incompatible companion: built-in player-visible explanation, server log, then disconnect in required mode.
- Invalid payload or rate limit: reject without forwarding, emit a bounded diagnostic, retain the connection unless abuse crosses a configured threshold.
- Stale/missing inventory: do not fabricate an empty array; the existing Takaro request follows the documented unavailable path.
- Client plugin stops responding: expire its cache/capabilities, notify, and disconnect after grace in required mode.
- Takaro is unavailable: existing server WebSocket reconnect behavior remains authoritative; the client never retries toward Takaro.
- World or peer transition: clear nonce, sequence, cache, and dedupe state atomically.

## Packaging and Release

CI produces two independently inspectable artifacts from the same source/version:

- `takaro-valheim-plugin.zip` — dedicated-server connector and server dependencies.
- `takaro-valheim-companion.zip` — client companion and client runtime dependencies.

The client archive must contain no server config, Takaro token field, WebSocket client, registration/identity token marker, server-only adapter, or cloud endpoint. Both manifests include product version, protocol version/range, process role, and SHA-256-friendly deterministic contents where the existing release flow permits it.

Because required client installation changes the deployment boundary, release notes must treat it as a breaking Valheim connector change and provide server plus player installation/upgrade instructions.

## Test Strategy

### Automated

- Red/green tests for protocol parsing, compatibility negotiation, nonce/session behavior, sender binding, limits, rate limiting, TTLs, deduplication, and cache cleanup.
- Client scaffold/contract tests for every required Harmony hook and the absence of tokens/cloud connectivity.
- Server dispatcher/event tests for client-backed inventory/chat/death/kill behavior and unavailable fallbacks.
- Real `net472` builds against current Valheim/BepInEx references.
- Package inspection for exact DLL roles, metadata, banned markers, dependencies, and source immutability.
- Existing 183 server-only tests and setup/rollback harness remain regression gates.

### Exact live acceptance

The feature is not called working until the exact packaged DLLs are installed and their hashes recorded on the local dedicated test server and graphical client. Evidence must cover:

1. A missing companion is rejected with a useful message.
2. An incompatible protocol version is rejected with expected/actual versions.
3. A compatible client joins, negotiates capabilities, reconnects, and survives a server restart.
4. Ordinary chat persists as one Takaro `chat-message`.
5. A `$` command reaches a real installed Takaro command/teleport module exactly once.
6. Inventory appears in Takaro, changes after a real item mutation, and never produces a fabricated empty snapshot.
7. One real player death persists exactly once.
8. One player-attributed entity kill persists with schema-valid entity and weapon data exactly once.
9. Oversized, spoofed, duplicate, stale, and rate-limited messages are rejected.
10. Existing reachability, players, location, teleport, world-drop `giveItem`, messaging, catalogs, and lifecycle behavior still pass.
11. `getPlayer`, kick, ban, unban, and shutdown are exercised on the disposable local server; ban state is checked both in Takaro and Valheim's official ban list, and shutdown runs last.
12. A fresh capability matrix and QA ledger distinguish server-owned, client-reported, upstream-blocked, and unsupported behavior without overclaiming.

## Success Criterion

The work is complete only when automated verification is green, both release artifacts are reproducible and free of secrets, the exact live acceptance suite passes at the game and Takaro boundaries, the installed module paths are proven, and GitHub CI plus the release artifacts remain green.
