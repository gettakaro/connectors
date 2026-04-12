# Integration Testing

End-to-end verification of the Takaro Minecraft connector using the Mineflayer test bot, RCON, and **Takaro MCP tools**. Complements the unit tests in `core/` — those test internal logic, these verify the full protocol flow from game server through WebSocket to Takaro backend.

## Prerequisites / Infrastructure Discovery

Takaro-assigned UUIDs change on re-registration. Discover them fresh each session:

```
# Find gameserver IDs
mcp__takaro__gameserverSearch({})

# Find online players (need gameserverId from above)
mcp__takaro__gameserverGetPlayers({ gameserverId: "<id>" })

# Confirm command prefix (default: +)
mcp__takaro__settingsGet({})
```

Bot usernames: `TakaroBot_paper`, `TakaroBot_neoforge`, `TakaroBot_fabric`.

## Event Verification Workflow

The canonical pattern for verifying events reach Takaro:

1. **Record timestamp** before triggering:
   ```bash
   date -u +%Y-%m-%dT%H:%M:%SZ
   ```

2. **Trigger event** (bot API, RCON, or MCP action)

3. **Wait 3–5 seconds** for event propagation

4. **Search events** via MCP:
   ```
   mcp__takaro__eventSearch({
     filters: {
       eventName: ["chat-message"],
       gameserverId: ["<id>"]
     },
     greaterThan: { createdAt: "<timestamp>" }
   })
   ```

5. **Verify event data** — check the `meta` field for expected content

### Event types and triggers

| Event | How to trigger | What to verify in `meta` |
|-------|---------------|--------------------------|
| `chat-message` | Bot sends chat via `/bot/:server/chat` | `msg`, `channel` |
| `command-executed` | Bot sends `+ping` (or other command) | `command`, `arguments` |
| `player-connected` | Bot reconnects or server restart | `player.name` |
| `player-disconnected` | Kick bot via MCP `gameserverKickPlayer` | `player.name` |
| `player-death` | RCON: `kill TakaroBot_paper` | `player.name` |
| `entity-killed` | RCON: summon + kill mob near bot | `entity`, `weapon` |

## Action Verification Workflow

Verify Takaro actions work by calling MCP tools and cross-validating:

| Action | MCP tool | Cross-validation |
|--------|----------|------------------|
| Get players | `gameserverGetPlayers` | Compare with `curl localhost:3001/bot/:server/players` |
| Send message | `gameserverSendMessage` | Check server logs for message |
| Teleport | `gameserverTeleportPlayer` | Check bot position: `curl localhost:3001/bot/:server/position` |
| Give item | `gameserverGiveItem` | Check bot inventory: `curl localhost:3001/bot/:server/inventory` |
| Kick player | `gameserverKickPlayer` | Bot status goes disconnected, then reconnects |
| Execute command | `gameserverExecuteCommand` | Check server logs for command execution |
| Ban/unban | `gameserverBanPlayer` / `gameserverUnbanPlayer` | `gameserverListBans` to verify |
| Reachability | `gameserverTestReachabilityForId` | Should return connectable: true |

## Code Change → Test Mapping

After modifying code, run the corresponding integration tests:

| Code area | What to test |
|-----------|-------------|
| Chat/message parsing | Bot sends `+ping`, verify `chat-message` + `command-executed` events |
| Connect/disconnect events | Kick bot via MCP, verify `player-disconnected` event, wait for reconnect |
| Death/kill events | Kill bot via RCON, verify `player-death` event |
| Action handlers | Call corresponding MCP tool, verify response |
| WebSocket/auth | `gameserverTestReachabilityForId`, check logs for "Identified successfully" |
| Item/entity code | Give item via MCP, check bot inventory; note platform naming differences |
| Config changes | Restart server, verify reconnection and `settingsGet` reflects changes |

## RCON Recipes

Common RCON commands for testing. Always quote the command argument:

```bash
# Kill bot (triggers player-death event)
cd minecraft && docker compose exec paper rcon-cli 'kill TakaroBot_paper'

# Summon mob near bot (for entity-killed testing)
cd minecraft && docker compose exec paper rcon-cli 'execute at TakaroBot_paper run summon zombie ~ ~ ~1'

# Kill nearby mobs
cd minecraft && docker compose exec paper rcon-cli 'execute at TakaroBot_paper run kill @e[type=zombie,distance=..20]'

# Give item to bot
cd minecraft && docker compose exec paper rcon-cli 'give TakaroBot_paper minecraft:diamond 1'

# Teleport bot
cd minecraft && docker compose exec paper rcon-cli 'tp TakaroBot_paper 0 64 0'

# Check online players
cd minecraft && docker compose exec paper rcon-cli 'list'
```

Replace `paper` with `fabric` or `neoforge` and adjust bot username accordingly.

## Known Limitations

- **Bot `attack()` targets hostile mobs and players**: The attack endpoint filters by `e.type === 'mob' || e.type === 'player'`, not `animal` (passive). To test entity-killed events, summon hostile mobs via RCON.
- **Entity tracking breaks after teleport**: After teleporting the bot, nearby entity tracking may be stale. Wait a few seconds or have the bot move.
- **NeoForge bot connection broken**: Protocol mismatch prevents the Mineflayer bot from connecting to NeoForge. Test NeoForge via RCON + MCP tools only.
- **Bot does NOT auto-respawn**: After death, you must call `POST /bot/:server/respawn` to respawn it.
- **`executeConsoleCommand` rawResult always empty**: The Minecraft server doesn't return command output through the protocol. Verify command effects instead.
- **`getPlayerLocation` after disconnect**: Takaro queries player location after disconnect, causing an IPosition validation error in the backend. This is a known backend issue, not a connector bug.
- **Item naming varies by platform**: Paper uses lowercase registry names (`cod`), Fabric/NeoForge use display names (`Raw Cod`). Account for this in inventory checks.
- **Fabric GameProfile**: Uses `.name()` not `.getName()` — it's a record class, not a POJO.
