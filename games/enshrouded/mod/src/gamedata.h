// Static game data extracted from enshrouded_server.kfc (see tools/gen_gamedata.py).
#pragma once
#include <cstddef>
#include <cstdint>

struct ItemDef {
    uint32_t itemId;     // runtime ItemStack.itemId
    uint16_t maxStack;   // ItemInfo.maxStackSize
    uint8_t pide;        // HasPIDE flag (unique item entity; count must be 1)
    const char* code;    // ItemInfo.debugName (stable identifier)
    const char* name;    // code with '_' -> ' ' (localized names are not shipped with the server)
    const char* category;
    const char* rarity;
};
struct EntityDef {
    uint8_t guid[16];    // templateGuid, RFC-4122 byte order
    const char* code;
    const char* type;    // hostile | friendly | neutral
    const char* family;
    const char* faction;
};
struct LocationDef {
    const char* code;
    const char* kind;       // mapMarker | spawnPoint | location
    const char* spawnType;  // StartLocation | SavePoint | Base | ""
    double x, y, z;         // metres, y up
};

extern const ItemDef kItems[];
extern const size_t kItemCount;
extern const EntityDef kEntities[];
extern const size_t kEntityCount;
extern const LocationDef kLocations[];
extern const size_t kLocationCount;
