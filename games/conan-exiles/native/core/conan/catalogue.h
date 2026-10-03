// The world catalogues behind listItems, listEntities and listLocations, plus the item display
// names getPlayerInventory uses. Built once on a worker thread from the game's own DataTables
// (El-Limon evidence spikes/S3-data.md), read only through UE::Reflection (safe self-reads):
//
//   items      /Game/Items/ItemTable (every DLC table is merged into it at load); codes from
//              /Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID
//   entities   /Game/Systems/SpawnTable/SpawnDataTable
//   locations  /Game/Systems/Map/MapMarkers_ConanSandbox (loads about a minute after the rest)
//
// Display names are the localised FText strings the client shows, never class or row names.
#pragma once

#include "ue/safe_reflect.h"

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace conan {

// ---- FText (worker decode; build 25639945 internals, validated against Conv_TextToString) ----
namespace TextLayout {
constexpr uintptr_t kPlainVtable = 0xe0c6a8;        // display string FString at +0x20
constexpr uintptr_t kStringTableVtable = 0xe0d198;  // +0x18 -> ref data, +0x18 -> entry, +0x10 FString
constexpr size_t kPlainString = 0x20, kStRef = 0x18, kStEntry = 0x18, kStString = 0x10;
}  // namespace TextLayout

enum class TextKind { Unreadable, Plain, StringTable, UnknownVtable };
// ITextData vtable -> how its display string is stored. Seeded with the Linux 25639945 vtables;
// other vtables (and every one on Windows, where the PE addresses differ) are learned at runtime
// by comparing the first text of each vtable with Conv_TextToString (Catalogue::Text).
using TextVtables = std::unordered_map<uintptr_t, TextKind>;
TextVtables DefaultTextVtables();
// Decodes the FText at `ftextAddr`. Returns the kind; `out` is set for Plain/StringTable.
TextKind DecodeText(const UE::Mem& m, uintptr_t ftextAddr, std::string& out, const TextVtables& vtables);
TextKind DecodeText(const UE::Mem& m, uintptr_t ftextAddr, std::string& out);  // default vtables

// The game-thread fallback for one FText (Conv_TextToString); returns false when it could not run.
using SlowTextFn = std::function<bool(uintptr_t ftextAddr, std::string& out)>;

// ---- DataTable rows (TMap<FName, uint8*> RowMap, UE 5.8 layout) ----
namespace TableLayout {
constexpr size_t kRowStruct = 0x28;  // also reflected (DataTable.RowStruct); checked at build
constexpr size_t kRowData = 0x30, kRowNum = 0x38, kRowMax = 0x3C;
constexpr size_t kAllocInline = 0x40, kAllocSecondary = 0x50, kAllocNumBits = 0x58;
constexpr size_t kFirstFree = 0x60, kNumFree = 0x64;
constexpr size_t kElemStride = 24, kElemName = 0, kElemRow = 8;
}  // namespace TableLayout

struct TableRow {
    std::string name;  // the row FName
    uintptr_t row = 0;
};
bool ReadTableRows(const UE::Reflection& r, uintptr_t table, std::vector<TableRow>& out, std::string& error);

// Rows the client never shows as a real thing: empty names and the dev prefixes
// ^\s*(x{2,}[ _]|dev[ _]|test[ _]|deprecated|do not use), case-insensitive.
bool IsInternalName(const std::string& s);
// Sort key: lower case with leading quotes/apostrophes/spaces dropped.
std::string SortKey(const std::string& s);

struct ItemEntry {
    int32_t templateId = 0;
    std::string code;         // ItemNameToTemplateID row name, else the template id
    std::string name;         // display name; " (#id)" appended when the name is not unique
    std::string rawName;      // display name as the table has it
    std::string description;  // ShortDesc ("" when internal)
    bool listed = false;      // part of listItems
};
struct EntityEntry {
    std::string code, name, npcClass;
};
struct LocationEntry {
    std::string code, name, region;
    double x = 0, y = 0, z = 0, radius = 0;
};

struct CatalogueStats {
    size_t itemRows = 0, itemsListed = 0, itemCodes = 0, entityRows = 0, entities = 0, markerRows = 0,
           locations = 0;
    size_t textPlain = 0, textStringTable = 0, textSlow = 0, textMissing = 0;
    double itemsMs = 0, entitiesMs = 0, locationsMs = 0;
};

class Catalogue {
public:
    // Every FText through the game-thread fallback (when the worker decode failed its self-check).
    void ForceSlowText(bool on) { forceSlow_ = on; }
    // Each Build* returns false with `error` when its table is not loaded (yet).
    bool BuildItems(const UE::Reflection& r, const SlowTextFn& slow, std::string& error);
    bool BuildEntities(const UE::Reflection& r, const SlowTextFn& slow, std::string& error);
    // `region` = "ExiledLands" / "IsleOfSiptah" keeps only that map's markers; "" keeps all.
    bool BuildLocations(const UE::Reflection& r, const SlowTextFn& slow, const std::string& region,
                        std::string& error);

    bool HaveItems() const { return haveItems_; }
    bool HaveEntities() const { return haveEntities_; }
    bool HaveLocations() const { return haveLocations_; }

    // JSON arrays in Takaro's DTO shapes.
    std::string ItemsJson() const;      // [{code,name,description?}]
    std::string EntitiesJson() const;   // [{code,name,metadata:{npcClass}}]
    std::string LocationsJson() const;  // [{code,name,position:{x,y,z},radius}]

    const ItemEntry* ByTemplate(int32_t templateId) const;
    // A Takaro item code (ItemNameToTemplateID name, case-insensitive) or a bare template id ->
    // template id of an ItemTable row; 0 when unknown. (For giveItem in lane L2b.)
    int32_t ResolveCode(const std::string& code) const;
    // Inventory display name: the listed name, else the raw table name, else "Conan item <id>".
    std::string DisplayName(int32_t templateId) const;
    std::string CodeFor(int32_t templateId) const;
    // Display name of the first entity row spawning `npcClass` (case-insensitive); "" when none.
    std::string EntityNameForClass(const std::string& npcClass) const;

    const std::vector<ItemEntry>& Items() const { return items_; }
    const std::vector<EntityEntry>& Entities() const { return entities_; }
    const std::vector<LocationEntry>& Locations() const { return locations_; }
    const CatalogueStats& Stats() const { return stats_; }
    // Samples for the startup text self-check: FText addresses of a few rows of each kind.
    const std::vector<std::pair<uintptr_t, std::string>>& TextSamples() const { return textSamples_; }

private:
    std::string Text(const UE::Reflection& r, const SlowTextFn& slow, uintptr_t addr);

    bool forceSlow_ = false;
    TextVtables vtables_ = DefaultTextVtables();
    std::unordered_map<uintptr_t, bool> slowOnly_;  // vtables no worker layout matched
    bool haveItems_ = false, haveEntities_ = false, haveLocations_ = false;
    std::vector<ItemEntry> items_;  // every ItemTable row, listed ones sorted first by name
    std::unordered_map<int32_t, size_t> byTemplate_;
    std::unordered_map<std::string, int32_t> byCode_;  // lower-case code -> template id
    std::vector<EntityEntry> entities_;
    std::vector<LocationEntry> locations_;
    CatalogueStats stats_;
    std::vector<std::pair<uintptr_t, std::string>> textSamples_;
};

}  // namespace conan
