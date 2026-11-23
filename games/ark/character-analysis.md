# Survivors as Takaro Characters

Exact executable: Linux `ShooterGameServer` build 21241282, SHA-256 `7e7ded49…c520`.

| Value | Source | Evidence |
|---|---|---|
| Character id | `PlayerState.MyPlayerData` → `MyData` (StructProperty, +0x28) → `PlayerDataID` (UInt64Property, +0) | Registered in `0x135A280`; the save file holds the same value |
| Character name | `MyData.MyPlayerCharacterConfig` (+0x40) → `PlayerCharacterName` (StrProperty, +0x68) | Registered in `0x130A140`; the name typed in the character creator |
| Struct of a struct property | `UStructProperty::Struct` at +0x70 | Stored by the constructor at `0x1D2B4F0` |
| Id generation | `0xE93EF0` | Random, 50001 or higher, retried until unused |

The plugin finds the offsets by reflection, checks each reflected type and caches the result per player data class.
The id does not need a living body, so it is also known on the respawn screen.

## Why this id

- `PlayerDataID` stays the same across deaths, respawns and server restarts.
- Create New Survivor replaces it. The game warns that this destroys the previous survivor.
- `SpawnDayNumber` and `SpawnDayTime` change on every respawn, so they cannot name a character.
- `LinkedPlayerDataID` on the pawn is not used: it needs a body.

## Semantics

- `players.characters` declares `held: server`: survivors live on the server. A death does not end one, so `ends` is left out.
- A destroyed survivor never comes back, so there is no switching back to one.
- Takaro changes the active Character only on `player-connected`, and ARK replaces a survivor without a relog. The
  connector therefore reports a new survivor the way Takaro defines a switch: `player-disconnected` as the old survivor,
  then `player-connected` as the new one. Hooks on those events run once for each new survivor.
- A new survivor starts with no reading, so the old level is never reported for it.
- A connector declaring this needs a Takaro version with Characters support: older versions reject `character` on events.
