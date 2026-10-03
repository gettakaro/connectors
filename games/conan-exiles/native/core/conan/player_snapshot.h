// Worker-thread reads of the online players, their location and inventory (El-Limon evidence
// spikes/S3-data.md §1, §2, §6). Every pointer is followed through UE::Mem (safe self-reads) and
// every list is checked for a torn read: the TArray header is read before and after, objects
// must still be alive, and inventory items must point back at their inventory.
#pragma once

#include "ue/safe_reflect.h"

#include <string>
#include <vector>

namespace conan {

// Offsets found by reflection at startup, plus the non-reflected ones pinned to build 25639945
// (each checked once against its game-thread getter before it is trusted; see reads.cpp).
struct PlayerLayout {
    // classes
    uintptr_t gameStateBase = 0, pcClass = 0, basePlayerChar = 0, baseBPChar = 0, itemInventory = 0, gameItem = 0;
    // reflected
    int32_t playerArray = -1;        // GameStateBase.PlayerArray (TArray<PlayerState*>)
    int32_t psOwner = -1;            // Actor.Owner (PlayerState -> its controller)
    int32_t psName = -1;             // PlayerState.PlayerNamePrivate (FString, platform name)
    int32_t psAddress = -1;          // PlayerState.SavedNetworkAddress (FString)
    int32_t pcUserId = -1;           // ConanPlayerController.UserIDFromURLOptions (FString: Steam64 on old
                                     // accounts, the Funcom account id "A-..." on newer ones)
    int32_t psUniqueId = -1;         // PlayerState.UniqueID (FUniqueNetIdRepl): the platform id, Steam64
    int32_t pcPawn = -1;             // Controller.Pawn
    int32_t rootComponent = -1;      // Actor.RootComponent
    int32_t attachParent = -1;       // SceneComponent.AttachParent
    int32_t relativeLocation = -1;   // SceneComponent.RelativeLocation (FVector, doubles)
    int32_t characterName = -1;      // BaseBPChar_C.CharacterName (FString)
    int32_t backpack = -1, hotbar = -1, equipment = -1;  // inventories on the pawn
    int32_t itemList = -1;           // ItemInventory.ItemList (TArray<GameItem*>)
    int32_t templateId = -1;         // GameItem.TemplateId
    int32_t ownerInventory = -1;     // GameItem.m_OwnerInventory
    // pinned to build 25639945, not reflected
    // PlayerState float (GetPingInMilliseconds), 8 bytes before SavedNetworkAddress on both servers:
    // 824 under clang (Linux), 840 under MSVC (Windows, SavedNetworkAddress at 848; live 2026-10-03).
#ifdef _WIN32
    static constexpr int32_t kExactPing = 840;
#else
    static constexpr int32_t kExactPing = 824;
#endif
    static constexpr int32_t kComponentToWorldT = 0x210;  // SceneComponent FTransform.Translation
    static constexpr int32_t kIntStats = 0x128, kFloatStats = 0x138;  // GameItem TArray, stride 48
    static constexpr int32_t kStatStride = 48, kStatId = 0xC, kStatValue = 0x14;
    // FUniqueNetIdRepl: vtable, then TSharedPtr<FUniqueNetId> (object at +8). The object is an
    // FUniqueNetIdString (Type FName "STEAM") whose id FString sits at +0x10 on Linux (live
    // 2026-10-03: "76561198000735875"); the reader tries the nearby slots for the MSVC layout.
    static constexpr int32_t kNetIdPtr = 8;
    static constexpr int32_t kNetIdStringSlots[4] = {0x10, 0x18, 0x08, 0x20};

    // Resolves every reflected offset; false with the missing names in `error`.
    bool Resolve(const UE::Reflection& r, std::string& error);
};

struct PlayerRecord {
    uintptr_t ps = 0, pc = 0, pawn = 0;
    std::string steam64;  // the Takaro gameId: Steam64 from PlayerState.UniqueID (else the best id we have)
    std::string uniqueId, urlId;  // PlayerState.UniqueID string, UserIDFromURLOptions (may be "A-...")
    std::string name, characterName, ip;
    float ping = -1;  // ExactPing (ms); < 0 = unknown
};

struct Location {
    double x = 0, y = 0, z = 0;
    bool attached = false;  // on a mount/seat: ComponentToWorld instead of RelativeLocation
    double rel[3] = {0, 0, 0}, world[3] = {0, 0, 0};
};

struct InvItem {
    int32_t templateId = 0;
    int32_t stack = 1;
    float durability = -1, maxDurability = -1;  // < 0: the item has no durability stat
    int inventory = 0;  // 0 backpack, 1 hotbar, 2 equipment
    int slot = 0;
    uintptr_t item = 0;
};

namespace ItemStat {
constexpr int kStackSize = 1;              // EItemIntStatID
constexpr int kMaxDurability = 7, kDurability = 8;  // EItemFloatStatID
}  // namespace ItemStat

// Internal templates the client never shows (the bare-fist weapons in the equipment slots).
inline bool IsInternalTemplate(int32_t id) { return id == 51204 || id == 51205; }

class PlayerReader {
public:
    PlayerReader(const UE::Reflection& r, const PlayerLayout& l) : r_(r), m_(r.mem()), l_(l) {}

    // Online players of the given GameStates (Steam64 known, controller alive). False with
    // `error` when PlayerArray stayed torn after retries.
    bool Players(const std::vector<uintptr_t>& gameStates, std::vector<PlayerRecord>& out, std::string& error) const;
    // Re-reads the volatile fields (ping) of one known record.
    bool Location(uintptr_t pawn, conan::Location& out, std::string& error) const;
    // Backpack, hotbar and equipment. False with `error` when a list stayed torn.
    bool Inventory(uintptr_t pawn, std::vector<InvItem>& out, std::string& error) const;
    // Decoded int/float stats of one item: {id -> value}.
    bool Stats(uintptr_t item, std::vector<std::pair<int, int32_t>>& ints,
               std::vector<std::pair<int, float>>& floats) const;

    // The id string behind PlayerState.UniqueID ("" when unreadable).
    std::string UniqueIdString(uintptr_t ps) const;

private:
    bool ReadPtrArray(uintptr_t arrayAt, int32_t maxNum, std::vector<uintptr_t>& out) const;
    bool OneInventory(uintptr_t inv, int which, std::vector<InvItem>& out, std::string& error) const;

    const UE::Reflection& r_;
    const UE::Mem& m_;
    const PlayerLayout& l_;
};

}  // namespace conan
