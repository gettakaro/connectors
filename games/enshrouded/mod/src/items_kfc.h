// Item table for a game build the compiled table (gamedata.cpp) was not made for: rebuilt once from the server's own
// enshrouded_server.kfc and saved as takaro/enshrouded-items.json together with the game build. The next start on
// the same build reads that file; a new build rebuilds it.
#pragma once
#include "gamedata.h"

#include <cstdint>
#include <string>
#include <vector>

namespace items_kfc {

// Where keen::ItemInfo keeps what ItemDef needs, from the exe's reflection tables (fields by name, enums by value).
struct Layout {
    uint32_t typeHash = 0;  // kfc resource type of keen::ItemInfo
    uint32_t itemId = 0, maxStackSize = 0, rarity = 0, category = 0, flags = 0, debugName = 0;
    int pideBit = -1;  // ItemFlag::HasPIDE
    std::vector<std::string> categories, rarities;  // enum value -> name
};
// Needs scan::Init().
bool LayoutFromReflection(Layout& out, std::string& err);

struct Row {
    uint32_t itemId;
    uint16_t maxStack;
    uint8_t pide;
    std::string code, category, rarity;
};
// Reads every ItemInfo resource of <kfcPath> (+ "<kfcPath>_resources"). `version` gets the container's build string.
bool ReadKfc(const std::string& kfcPath, const Layout& l, std::vector<Row>& rows, std::string& version, std::string& err);

// The table for `gameBuild`: takaro/enshrouded-items.json when it was saved for this build, else rebuilt from the kfc
// (and saved). On success `items`/`count` point at storage that lives for the rest of the process.
bool Load(const std::string& gameBuild, const ItemDef*& items, size_t& count, std::string& source, std::string& err);

}  // namespace items_kfc
