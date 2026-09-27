# Entity catalog candidate in ARK build 21241282

This is an offline binding and test record, not live `listEntities` acceptance.
All addresses refer to Linux `ShooterGameServer` SHA-256
`7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520`.

The reflected `DinoEntriesObjects` name is UTF-32 at `0x4129730`. Its
property registration at `0x14090D0–0x140913F` creates a 16-byte TArray at
offset `0xAE8` in the same PrimalGameData registration block that defines
`MasterItemList` at offset `0x910`. The TArray's inner property registration
at `0x14091A6–0x14091D6` calls `0x18647B0` for the allowed object class.
`0x18647B0` jumps to `0x13B5940`, whose class constructor passes UTF-32
`UPrimalDinoEntry` at `0x40DC2F0` to the native UClass constructor. Therefore
the array holds `UPrimalDinoEntry*` objects, not spawnable dino actor classes.

`AdditionalDinoEntries` is separately registered at offset `0xC18` via
`0x1407A79–0x1407AE8`. It may extend the entries but does not establish a
complete actor catalog on its own. The packaged localization list also
contains DinoEntry asset paths, including DLC entries, but it is not a live
loaded class catalog and cannot substitute for validated native rows.

The Generic entity catalog needs a stable code and display name, not a
spawnable actor class. Loose installed `DinoEntry_Dodo.uasset` has CDO
`DinoNameTag` (`NameProperty`) = `Dodo` and `DinoDescriptiveName`
(`StrProperty`) = `Dodo`. Offline parsing found both properties with those
exact types in all 207 installed `DinoEntry_*.uasset` CDO exports, with 207
distinct tag values. These assets corroborate the field names and types but
do not establish the live game's loaded entries or array coverage.

Exact reflected-property layout is derived from native code:
`0x1CCC600` prepends UField objects to `UStruct+0x38`, linking the prior
child at `UField+0x28`; `0x1D1DB68` writes the property data offset at
`UProperty+0x4C`. A UField FName is at `+0x18`, UObject class at `+0x10`,
and UClass superclass at `+0x30`. Native `0x1C892D0` creates an FName,
`0x1C0B900` converts FName to caller-owned FString (freed by `0x1AF8E30`),
and static classes for UNameProperty and UStrProperty are `0x1D2A7D0`
and `0x1D2B470`. The bounded game-thread reader in
`mod/src/entity_bindings.hpp` checks object class, property type, property
offset/size, UTF-32 content, and per-page tag uniqueness before returning
code/name rows. Its focused synthetic tests pass under GCC 14 C++20
`-O2 -Wall -Wextra -Werror -pthread`.

`Dodo_Character_BP.uasset` also serializes `DinoNameTag` as `NameProperty`
with value `Dodo` on its CDO, matching the entry code. The exact
`APrimalDinoCharacter` static class getter is `0x17EDD80`. The helper's
`actor_code` reads the same reflected tag before a death callback's original
function runs; native integration still must verify actual death/attacker,
catalog membership, lifetime, and real event acceptance. No live entity
snapshot or kill event has been tested yet, so the sidecar must continue to
report this capability unavailable until that integration is proven.

Offline evidence was produced with `objdump -d -M intel` around
`0x14090D0–0x14091D6`, `0x18647B0`, and `0x13B5940` on the exact executable.
