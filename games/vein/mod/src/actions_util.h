// Lane L3: the pure, game-free half of the action handlers.
//
// Everything here is a function of its arguments only - no UObject, no symbol, no process state -
// so tests/unit_test.cpp links it directly and exercises it on the host. actions.cpp holds only the
// parts that genuinely have to touch the game.
#pragma once
#include "common.h"

namespace ActionsUtil {

std::string Lower(std::string s);

// A SteamID64 is 0x0110000100000000 | accountId: the "individual account, public universe" pattern.
// Specific enough that a stray word in a struct cannot pass for one.
bool LooksLikeSteamId64(uint64_t v);

// VEIN is Steam-only, so `gameId` is the bare 17-digit SteamID64. Accepts `steam:<id>`, a bare id,
// or anything else (which is passed through lower-cased so a player *name* still matches).
std::string NormalizeGameId(const std::string& raw);

// Blueprint/reflection artefacts that must never appear in a catalogue.
bool IsGeneratedArtefact(const std::string& name);

// ---- lane L3c: readable catalogue names -----------------------------------------------------
// Turns a VEIN class name into a human-readable label, used only when the game itself has no
// display text for the class. Strips the Blueprint decorations (`BP_` prefix, `_C` suffix), splits
// on underscores and camel-case boundaries, and drops a trailing `Item` word when something else
// remains:
//     BloodPressureCuffItem   -> "Blood Pressure Cuff"
//     BP_NeedleThread_C       -> "Needle Thread"
//     BP_USD_C                -> "USD"
//     Item                    -> "Item"
// Never returns the empty string: a name it cannot improve on comes back unchanged.
std::string HumaniseCode(const std::string& code);

// ---- lane L3c / finding F11: one `code` per item ---------------------------------------------
// The catalogue, `giveItem` and the inventory must all speak the *same* identifier. The catalogue
// and `giveItem` use the UClass short name (`BP_Pen_C`) because that is what
// UAdminComponent::Server_GiveItem takes; a player's inventory holds a TSoftClassPtr whose
// FSoftObjectPath is a package path (`/Game/Vein/Items/Junk/Office/BP_Pen`) or an object path
// (`/Game/.../BP_Pen.BP_Pen_C`). This folds any of those forms onto the class short name:
//     /Game/Vein/Items/Junk/Office/BP_Pen   -> BP_Pen_C
//     /Game/.../BP_Pen.BP_Pen_C             -> BP_Pen_C
//     BP_Pen_C                              -> BP_Pen_C     (already a class name; unchanged)
// An empty or path-only input comes back empty.
std::string ItemCodeFromSoftPath(const std::string& raw);

// Splits on whitespace.
std::vector<std::string> Words(const std::string& s);
// Everything after the first `skipWords` whitespace-separated words, leading space stripped.
std::string Rest(const std::string& s, size_t skipWords);

// ---- catalogue lookup ---------------------------------------------------------------------
// Exact code (case-insensitive), then exact name, then a *unique* case-insensitive substring of
// either. An ambiguous or unknown needle returns -1 and sets `err`.
struct CatalogueEntry {
    std::string code;
    std::string name;
};
int LookupIndex(const std::vector<CatalogueEntry>& entries, const std::string& needle, std::string& err);

// True when `entry` should appear in a `?search=` filtered listing.
bool MatchesSearch(const CatalogueEntry& entry, const std::string& lowerNeedle);


// ---- lane L3d: broadcast path selection ------------------------------------------------------
// `AVeinGameStateBase::NetMulticast_SendChat(AVeinPlayerState* Sender, ...)` dereferences `Sender`
// through a `TObjectPtr` with no null check, so calling it with a null sender SIGSEGVs the whole
// server (lane L6b, 2026-09-17). The broadcast path is therefore chosen here, in a pure function,
// and `kChatWithSender` is unreachable unless a real sender object was found.
enum class BroadcastPath {
    kNone = 0,              // nothing usable - the caller must fail with 503
    kGameStateServerMsg,    // AVeinGameStateBase::NetMulticast_BroadcastServerMessage(FString const&)
    kAdminServerMsg,        // UAdminComponent::Server_SendServerMessage(FString const&)
    kChatWithSender,        // NetMulticast_SendChat with a REAL AVeinPlayerState*
};

// What the process can actually do right now: which symbols resolved, and whether a validated,
// non-null AVeinPlayerState is available to act as the chat sender.
struct BroadcastCaps {
    bool gameStateServerMsg = false;
    bool adminServerMsg = false;
    bool sendChat = false;
    bool haveSender = false;
};

// `viaPref` is TAKARO_BROADCAST_VIA / `broadcastVia`: "" or "auto" (server message first, chat as a
// last resort), "chat" (prefer real chat when a sender exists), "servermessage" / "admin" to pin a
// path. Returns kNone with `err` set when nothing is safe to call.
BroadcastPath ChooseBroadcastPath(const std::string& viaPref, const BroadcastCaps& caps, std::string& err);

// The symbol name reported in the response's `via` field for a chosen path.
const char* BroadcastPathSymbol(BroadcastPath p);

// The message body as it goes on the wire: `[sender] text`, or `text` when there is no sender name.
// Trailing/leading whitespace in the sender is ignored; a sender that is only whitespace is no
// sender at all.
std::string RenderMessage(const std::string& senderName, const std::string& text);

// ---- lane L3b: admin grant list -------------------------------------------------------------
// Parses TAKARO_ADMIN_STEAMIDS / TAKARO_SUPERADMIN_STEAMIDS: a list of SteamID64s separated by
// commas, semicolons or whitespace. `steam:<id>` is accepted and stripped, surrounding quotes and
// whitespace are trimmed, duplicates are dropped keeping first-seen order, and anything that is not
// a 17-digit SteamID64 (the 0x0110000100000000 pattern) is rejected rather than passed to the game.
// When `rejected` is non-null every discarded token is appended to it, so /health can show them.
std::vector<std::string> ParseSteamIdList(const std::string& raw, std::vector<std::string>* rejected = nullptr);

// ---- lane L3e: the pure half of "never report success without the effect" --------------------
// Every mutating action now decides its own verdict from a before/after measurement rather than
// from "the call returned". The measurements themselves need the game thread; the *decisions*
// below are pure, so they are tested.

// Euclidean distance between two {x,y,z} in centimetres.
double Distance3(const double a[3], const double b[3]);

// A teleport counts as verified only when the pawn BOTH left where it was and ended up within
// `toleranceCm` of the target. "Did not move at all" is the exact failure L6b caught being reported
// as success, and "is already standing on the target" is not a teleport we performed.
bool TeleportArrived(const double before[3], const double after[3], const double target[3], double toleranceCm);

// A give counts as verified only when the player holds strictly more of that item code than before.
bool GiveArrived(int before, int after);

// A JSON array of strings, for the `attempted` field of a 409 body.
std::string JsonStrArray(const std::vector<std::string>& v);

// ---- lane L3f / finding F19: the current pawn's inventory, never a stale container -----------
// After a death and respawn the player state and the controller's outer chain can still reference
// the OLD character, and VEIN additionally keeps the dead body's loot in a
// `UPersistentCorpseInventory` (a `UBaseInventoryComponent` subclass). A scan that accepts any
// inventory component found under the pawn or the controller's outer chain can therefore return a
// corpse's or a cached character's items - which is exactly what `getPlayerInventory` did (Takaro
// showed "Corn 2" for a character that was holding no corn at all).
//
// `IsPlayerInventoryClass` is the pure half of the guard: the component the live character carries
// is a plain `BaseInventoryComponent`/`InventoryComponent`; anything whose class name names a
// corpse, a cache, a container, a storage or a vehicle is somebody else's inventory and must never
// answer for the player.
bool IsPlayerInventoryClass(const std::string& className);


// ---- lane L2c: the `weapon` field of entity-killed -------------------------------------------
// The death event's `DamageCauser` is an ACTOR, not an item: for a melee kill it is the swung
// `AMeleeEquippedItem`, for a bite it is the zombie or the wolf itself, and for the debug kill it
// is the player's own pawn. The old code put that actor's class name in `weapon`, which is how a
// wolf eaten by a zombie was reported as "killed with BP_Zombie_C".
//
// `itemName` is the display name of the UItem the causer carries, resolved by reflection from
// `AEquippedItem::ItemInstance` (empty when the causer carries none). The rule:
//   * an item was found      -> its display name ("Baseball Bat");
//   * no item, but the class names a weapon-ish actor (an equipped item, a projectile, a bullet,
//     an arrow, a thrown object) -> the humanised class name, so a hit still says what hit;
//   * anything else (a pawn, an animal, a vehicle, the world, nothing at all) -> "", and the
//     caller OMITS `weapon` rather than inventing one.
std::string KillWeaponName(const std::string& causerClass, const std::string& itemName);

// UE's `FGuid::ToString(EGuidFormats::Digits)`: the four words as 32 upper-case hex digits, which
// is exactly the form VEIN's own `127.0.0.1:8080/status` prints for `characterId` and the form the
// `selected character <id>` log line carries. An all-zero GUID means "no character" and returns "".
std::string GuidDigits(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// How `POST /give amount:N` is split into `AddItem` calls, given the item class's own stack limit
// (`maxStack`, which is 1 for an item whose `bStackable` is false). Each returned element is the
// `Stack` of one instance to build, and they sum to `amount`.
//
// The pre-L3f split was `n = maxStack > 1 && remaining > maxStack ? maxStack : remaining`, which
// for maxStack == 1 degenerated to a SINGLE instance carrying `Stack = amount`. VEIN ignores
// `Stack` on a non-stackable item, so three corn arrived as one corn - while the inventory reader
// read the instance back as three (finding F19). A non-stackable item must therefore be added one
// instance per unit.
std::vector<int> StackSplit(int amount, int maxStack);

// ---- lane L3f / finding F19: the current pawn's inventory, never a stale container -----------
// After a death and respawn the player state, the controller and the world all still reference the
// OLD character for a while, and VEIN additionally keeps the dead body's loot in a
// `UPersistentCorpseInventory` (a `UBaseInventoryComponent` subclass). A scan that accepts any
// inventory component found under the pawn or the controller's outer chain can therefore return a
// corpse's or a cached character's items - which is exactly what `getPlayerInventory` did (Takaro
// showed "Corn 2" for a character that was holding no corn at all).
//
// `IsPlayerInventoryClass` is the pure half of the guard: the component class the live character
// carries is `UBaseInventoryComponent` / `BaseInventoryComponent` or a plain subclass; anything
// whose class name names a corpse, a cache, a container, a storage or a vehicle is somebody
// else's inventory and must never answer for the player.
bool IsPlayerInventoryClass(const std::string& className);

// UE's `FGuid::ToString(EGuidFormats::Digits)`: the four words as 32 upper-case hex digits, which
// is exactly the form VEIN's own `127.0.0.1:8080/status` prints for `characterId` and the form the
// `selected character <id>` log line carries. An all-zero GUID is "no character" and returns "".
std::string GuidDigits(uint32_t a, uint32_t b, uint32_t c, uint32_t d);

// Why `getPlayerLocation` has no position to report, or "" when it has one. Only a spawned
// character stands in the world: the character-creation pawn sits in a lobby scene far outside
// the map, and reporting it puts the player off the map and draws a false path on it.
std::string LocationRefusal(bool hasPawn, bool spawned, bool hasLoc, const std::string& pawnClass);

}  // namespace ActionsUtil
