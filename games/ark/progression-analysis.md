# Player progression bindings

Exact executable: Linux `ShooterGameServer` build 21241282, SHA-256 `7e7ded49…c520`. Game symbols are stripped, so
each address below was derived from the native-function registration table with `objdump -d -M intel`.

| Value | Source | Evidence |
|---|---|---|
| Level | `BaseCharacterLevel` (+0x6C4, int) + `ExtraCharacterLevel` (+0x6C8, uint16) | `execGetCharacterLevel` thunk `0x15A38A0` calls `0xA5CDB0`, which is exactly that sum |
| Experience | `ExperiencePoints` (+0x6CC, float) | `SetPlayerLevel` (`0xDD66F0`) subtracts this field from the ramp value to work out the XP to grant |
| Next threshold | `0xA5D100` | thunk `0x15A3A30` of `GetExperienceRequiredForNextLevelUp` |
| Previous threshold | `0xA5D200` | thunk `0x15A3A60` of `GetExperienceRequiredForPreviousLevelUp`; 0 at the first level |
| Character | `0xF440A0` on the controller | resolves the weak pointer at controller +0xA54 and returns null for a pending-kill actor |
| Status component | `MyCharacterStatusComponent` (+0xCE0) | read by `SetPlayerLevel` from the same character |

The plugin finds the field offsets by reflection, checks each reflected type and caches the result per status class.
The three called functions are prologue-checked on top of the executable hash.

## Semantics

- The level is the one the game shows. XP can pass the next threshold without changing it: ARK raises the level
  when the player spends the point, so `progress` stays at 1 until then.
- At the level cap both thresholds are equal and `progress` is left out.
- The cheat `SetPlayerLevel` only raises a level (`cmp r14d, eax; jle` to the exit). Creating a new survivor on the respawn screen starts again at level 1.
- A player without a living body keeps the last reading. A player who never had one has no tracks.
- The character lives on the server, so the capability declares `source: server`.
