#include "conan/catalogue.h"

#include "common.h"
#include "conan/text.h"
#include "takaro/json_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_set>

namespace conan {

using UE::Mem;
using UE::Reflection;
using takaro::Trim;

namespace {
const char* const kItemTable = "/Game/Items/ItemTable.ItemTable";
const char* const kItemCodes = "/Game/Systems/Survival/Gathering/Gathering_v2/ItemNameToTemplateID.ItemNameToTemplateID";
const char* const kSpawnTable = "/Game/Systems/SpawnTable/SpawnDataTable.SpawnDataTable";
const char* const kMarkerTable = "/Game/Systems/Map/MapMarkers_ConanSandbox.MapMarkers_ConanSandbox";

std::string LowerAscii(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// RowStruct of a DataTable, read through the reflected property (DataTable.RowStruct).
uintptr_t RowStructOf(const Reflection& r, uintptr_t table) {
    uintptr_t dtClass = r.mem().Rd<uintptr_t>(table + UE::Layout::kObjClass);
    int32_t off = r.Offset(dtClass, "RowStruct", "ObjectProperty", 8);
    if (off < 0) off = (int32_t)TableLayout::kRowStruct;
    return r.mem().Rd<uintptr_t>(table + (uintptr_t)off);
}

int64_t Elapsed(uint64_t t0) { return (int64_t)(NowNs() - t0); }
}  // namespace

TextKind DecodeText(const Mem& m, uintptr_t at, std::string& out) {
    using namespace TextLayout;
    out.clear();
    uintptr_t data = m.Rd<uintptr_t>(at);
    if (!UE::Plausible(data)) return TextKind::Unreadable;
    uintptr_t vt = 0;
    if (!m.Get(data, vt)) return TextKind::Unreadable;
    if (vt == kPlainVtable) return m.ReadFString(data + kPlainString, out) ? TextKind::Plain : TextKind::Unreadable;
    if (vt == kStringTableVtable) {
        uintptr_t ref = m.Rd<uintptr_t>(data + kStRef);
        uintptr_t entry = UE::Plausible(ref) ? m.Rd<uintptr_t>(ref + kStEntry) : 0;
        if (!UE::Plausible(entry)) return TextKind::Unreadable;
        return m.ReadFString(entry + kStString, out) ? TextKind::StringTable : TextKind::Unreadable;
    }
    return TextKind::UnknownVtable;
}

bool ReadTableRows(const Reflection& r, uintptr_t table, std::vector<TableRow>& out, std::string& error) {
    using namespace TableLayout;
    out.clear();
    const Mem& m = r.mem();
    uint8_t h[0x68];
    if (!m.Read(table, h, sizeof h)) {
        error = "table header unreadable";
        return false;
    }
    uintptr_t data;
    int32_t num, max, numBits, firstFree, numFree;
    memcpy(&data, h + kRowData, 8);
    memcpy(&num, h + kRowNum, 4);
    memcpy(&max, h + kRowMax, 4);
    memcpy(&numBits, h + kAllocNumBits, 4);
    memcpy(&firstFree, h + kFirstFree, 4);
    memcpy(&numFree, h + kNumFree, 4);
    if (num < 0 || num > 500000 || max < num || numFree < 0 || numFree > num || (num && !data)) {
        error = "implausible RowMap (num " + std::to_string(num) + ")";
        return false;
    }
    if (num == 0) return true;
    std::vector<uint8_t> elems((size_t)num * kElemStride);
    if (!m.Read(data, elems.data(), elems.size())) {
        error = "RowMap elements unreadable";
        return false;
    }
    std::vector<uint32_t> bits;
    if (numFree > 0) {  // holes: only allocated slots carry rows
        if (numBits < num) {
            error = "RowMap allocation bits shorter than the element array";
            return false;
        }
        bits.resize(((size_t)num + 31) / 32);
        uintptr_t src = numBits <= 128 ? table + kAllocInline : 0;
        if (!src) memcpy(&src, h + kAllocSecondary, 8);
        if (!m.Read(src, bits.data(), bits.size() * 4)) {
            error = "RowMap allocation bits unreadable";
            return false;
        }
    }
    for (int32_t i = 0; i < num; i++) {
        if (!bits.empty() && !(bits[(size_t)i / 32] & (1u << (i % 32)))) continue;
        const uint8_t* e = elems.data() + (size_t)i * kElemStride;
        uint32_t idx, number;
        uintptr_t row;
        memcpy(&idx, e + kElemName, 4);
        memcpy(&number, e + kElemName + 4, 4);
        memcpy(&row, e + kElemRow, 8);
        if (!UE::Plausible(row)) continue;
        std::string name = r.Name(idx);
        if (name.empty()) continue;
        if (number) name += "_" + std::to_string(number - 1);
        out.push_back({std::move(name), row});
    }
    // Torn-read guard: the table must not have changed while we copied it.
    int32_t num2 = m.Rd<int32_t>(table + kRowNum);
    uintptr_t data2 = m.Rd<uintptr_t>(table + kRowData);
    if (num2 != num || data2 != data) {
        error = "RowMap changed while reading";
        out.clear();
        return false;
    }
    return true;
}

bool IsInternalName(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    if (i == s.size()) return true;
    std::string t = LowerAscii(s.substr(i));
    size_t x = 0;
    while (x < t.size() && t[x] == 'x') x++;
    if (x >= 2 && x < t.size() && (t[x] == ' ' || t[x] == '_')) return true;
    for (const char* p : {"dev ", "dev_", "test ", "test_", "deprecated", "do not use"})
        if (t.compare(0, strlen(p), p) == 0) return true;
    return false;
}

std::string SortKey(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == '"' || s[i] == '\'' || s[i] == ' ')) i++;
    return LowerAscii(s.substr(i));
}

std::string Catalogue::Text(const Reflection& r, const SlowTextFn& slow, uintptr_t addr) {
    std::string s;
    if (forceSlow_) {
        if (slow && slow(addr, s)) {
            stats_.textSlow++;
            return s;
        }
        stats_.textMissing++;
        return "";
    }
    TextKind k = DecodeText(r.mem(), addr, s);
    if (k == TextKind::Plain || k == TextKind::StringTable) {
        size_t& n = k == TextKind::Plain ? stats_.textPlain : stats_.textStringTable;
        // The first rows of each kind are re-checked against Conv_TextToString at startup.
        if (n < 6 && !s.empty()) textSamples_.push_back({addr, s});
        n++;
        return s;
    }
    if (k == TextKind::UnknownVtable && slow && slow(addr, s)) {
        stats_.textSlow++;
        return s;
    }
    stats_.textMissing++;
    return "";
}

bool Catalogue::BuildItems(const Reflection& r, const SlowTextFn& slow, std::string& error) {
    const uint64_t t0 = NowNs();
    uintptr_t table = r.ByPath(kItemTable), codes = r.ByPath(kItemCodes);
    if (!table || !codes) {
        error = std::string(table ? kItemCodes : kItemTable) + " is not loaded";
        return false;
    }
    uintptr_t rowStruct = RowStructOf(r, table);
    int32_t offName = r.Offset(rowStruct, "Name", "TextProperty", 16);
    int32_t offDesc = r.Offset(rowStruct, "ShortDesc", "TextProperty", 16);
    if (offName < 0 || offDesc < 0) {
        error = "ItemTableRow Name/ShortDesc not found by reflection";
        return false;
    }
    // Codes: ItemNameToTemplateID row name -> template id (int, the struct's only member).
    uintptr_t codeStruct = RowStructOf(r, codes);
    uintptr_t firstProp = r.mem().Rd<uintptr_t>(codeStruct + UE::Layout::kStructChildProps);
    int32_t offId = firstProp ? r.mem().Rd<int32_t>(firstProp + UE::Layout::kPropOffset) : -1;
    if (r.ObjName(codeStruct) != "ItemNameToTemplateIDStruct" || offId < 0 || offId > 64) {
        error = "ItemNameToTemplateID row struct is not the expected one";
        return false;
    }
    std::vector<TableRow> rows, codeRows;
    if (!ReadTableRows(r, table, rows, error) || !ReadTableRows(r, codes, codeRows, error)) {
        error = "item tables: " + error;
        return false;
    }
    std::unordered_map<int32_t, std::string> codeFor;  // first name in table order wins
    for (auto& c : codeRows) {
        int32_t id = 0;
        if (!r.mem().Get(c.row + (uintptr_t)offId, id) || id <= 0) continue;
        codeFor.emplace(id, c.name);
    }
    std::vector<ItemEntry> items;
    items.reserve(rows.size());
    for (auto& row : rows) {
        char* end = nullptr;
        long id = strtol(row.name.c_str(), &end, 10);
        if (!end || *end || id <= 0) continue;  // rows are keyed by the template id
        ItemEntry e;
        e.templateId = (int32_t)id;
        e.rawName = Trim(Text(r, slow, row.row + (uintptr_t)offName));
        e.description = Trim(Text(r, slow, row.row + (uintptr_t)offDesc));
        if (IsInternalName(e.description)) e.description.clear();
        auto c = codeFor.find(e.templateId);
        e.code = c != codeFor.end() ? c->second : row.name;
        e.listed = !IsInternalName(e.rawName) && !IsInternalName(e.code);
        e.name = e.rawName;
        items.push_back(std::move(e));
    }
    std::unordered_map<std::string, int> counts;
    for (auto& e : items)
        if (e.listed) counts[LowerAscii(e.name)]++;
    for (auto& e : items)
        if (e.listed && counts[LowerAscii(e.name)] > 1) e.name += " (#" + std::to_string(e.templateId) + ")";
    std::stable_sort(items.begin(), items.end(), [](const ItemEntry& a, const ItemEntry& b) {
        if (a.listed != b.listed) return a.listed;
        std::string ka = SortKey(a.name), kb = SortKey(b.name);
        if (ka != kb) return ka < kb;
        return a.templateId < b.templateId;
    });
    items_ = std::move(items);
    byTemplate_.clear();
    byCode_.clear();
    stats_.itemsListed = 0;
    for (size_t i = 0; i < items_.size(); i++) {
        byTemplate_[items_[i].templateId] = i;
        stats_.itemsListed += items_[i].listed;
    }
    for (auto& c : codeRows) {  // every alias of a template that exists in ItemTable
        int32_t id = 0;
        if (r.mem().Get(c.row + (uintptr_t)offId, id) && byTemplate_.count(id)) byCode_.emplace(LowerAscii(c.name), id);
    }
    stats_.itemRows = rows.size();
    stats_.itemCodes = codeFor.size();
    stats_.itemsMs = Elapsed(t0) / 1e6;
    haveItems_ = true;
    return true;
}

bool Catalogue::BuildEntities(const Reflection& r, const SlowTextFn& slow, std::string& error) {
    const uint64_t t0 = NowNs();
    uintptr_t table = r.ByPath(kSpawnTable);
    if (!table) {
        error = std::string(kSpawnTable) + " is not loaded";
        return false;
    }
    uintptr_t rowStruct = RowStructOf(r, table);
    int32_t offName = r.Offset(rowStruct, "Name", "TextProperty", 16);
    int32_t offClass = r.Offset(rowStruct, "NPCClass", "SoftClassProperty", 40);
    if (offName < 0 || offClass < 0) {
        error = "SpawnTableRow Name/NPCClass not found by reflection";
        return false;
    }
    std::vector<TableRow> rows;
    if (!ReadTableRows(r, table, rows, error)) {
        error = "spawn table: " + error;
        return false;
    }
    std::vector<EntityEntry> out;
    for (auto& row : rows) {
        EntityEntry e;
        e.code = row.name;
        e.name = Trim(Text(r, slow, row.row + (uintptr_t)offName));
        if (IsInternalName(e.name) || IsInternalName(e.code)) continue;
        // FSoftObjectPtr: weak ptr (8), then FSoftObjectPath { package FName, asset FName, sub path }.
        e.npcClass = r.FName(row.row + (uintptr_t)offClass + 16);
        out.push_back(std::move(e));
    }
    std::stable_sort(out.begin(), out.end(), [](const EntityEntry& a, const EntityEntry& b) {
        std::string ka = SortKey(a.name), kb = SortKey(b.name);
        return ka != kb ? ka < kb : a.code < b.code;
    });
    entities_ = std::move(out);
    stats_.entityRows = rows.size();
    stats_.entities = entities_.size();
    stats_.entitiesMs = Elapsed(t0) / 1e6;
    haveEntities_ = true;
    return true;
}

bool Catalogue::BuildLocations(const Reflection& r, const SlowTextFn& slow, const std::string& region,
                               std::string& error) {
    const uint64_t t0 = NowNs();
    uintptr_t table = r.ByPath(kMarkerTable);
    if (!table) {
        error = std::string(kMarkerTable) + " is not loaded yet";
        return false;
    }
    uintptr_t rowStruct = RowStructOf(r, table);
    int32_t offName = r.Offset(rowStruct, "Name", "TextProperty", 16);
    int32_t offRegion = r.Offset(rowStruct, "Region", "StructProperty", 8);
    int32_t offLoc = r.Offset(rowStruct, "Location", "StructProperty", 24);
    int32_t offRadius = r.Offset(rowStruct, "DiscoveryRadius", "FloatProperty", 4);
    if (offName < 0 || offRegion < 0 || offLoc < 0 || offRadius < 0) {
        error = "MapMarkerTableRow fields not found by reflection";
        return false;
    }
    std::vector<TableRow> rows;
    if (!ReadTableRows(r, table, rows, error)) {
        error = "marker table: " + error;
        return false;
    }
    std::vector<LocationEntry> out;
    for (auto& row : rows) {
        LocationEntry e;
        e.code = "marker:" + row.name;
        e.name = Trim(Text(r, slow, row.row + (uintptr_t)offName));
        e.region = r.FName(row.row + (uintptr_t)offRegion);
        double v[3];
        float radius = 0;
        if (!r.mem().Read(row.row + (uintptr_t)offLoc, v, sizeof v) ||
            !r.mem().Get(row.row + (uintptr_t)offRadius, radius))
            continue;
        if (IsInternalName(e.name)) continue;
        if (!region.empty() && !e.region.empty() && !EqualsIgnoreCase(e.region, region)) continue;
        e.x = v[0];
        e.y = v[1];
        e.z = v[2];
        e.radius = radius;
        out.push_back(std::move(e));
    }
    locations_ = std::move(out);
    stats_.markerRows = rows.size();
    stats_.locations = locations_.size();
    stats_.locationsMs = Elapsed(t0) / 1e6;
    haveLocations_ = true;
    return true;
}

std::string Catalogue::ItemsJson() const {
    std::string o = "[";
    bool first = true;
    for (auto& e : items_) {
        if (!e.listed) continue;
        takaro::ObjBuilder b;
        b.S("code", e.code).S("name", e.name);
        if (!e.description.empty()) b.S("description", e.description);
        o += (first ? "" : ",") + b.Done();
        first = false;
    }
    return o + "]";
}

std::string Catalogue::EntitiesJson() const {
    std::string o = "[";
    for (size_t i = 0; i < entities_.size(); i++) {
        const auto& e = entities_[i];
        takaro::ObjBuilder b;
        b.S("code", e.code).S("name", e.name);
        if (!e.npcClass.empty()) b.Raw("metadata", takaro::ObjBuilder().S("npcClass", e.npcClass).Done());
        o += (i ? "," : "") + b.Done();
    }
    return o + "]";
}

std::string Catalogue::LocationsJson() const {
    std::string o = "[";
    for (size_t i = 0; i < locations_.size(); i++) {
        const auto& e = locations_[i];
        std::string pos = takaro::ObjBuilder().N("x", e.x).N("y", e.y).N("z", e.z).Done();
        takaro::ObjBuilder b;
        b.S("code", e.code).S("name", e.name).Raw("position", pos);
        if (e.radius > 0) b.N("radius", e.radius);
        o += (i ? "," : "") + b.Done();
    }
    return o + "]";
}

const ItemEntry* Catalogue::ByTemplate(int32_t id) const {
    auto it = byTemplate_.find(id);
    return it == byTemplate_.end() ? nullptr : &items_[it->second];
}

int32_t Catalogue::ResolveCode(const std::string& raw) const {
    std::string code = Trim(raw);
    auto it = byCode_.find(LowerAscii(code));
    if (it != byCode_.end()) return it->second;
    char* end = nullptr;
    long id = strtol(code.c_str(), &end, 10);
    if (end && !*end && id > 0 && byTemplate_.count((int32_t)id)) return (int32_t)id;
    return 0;
}

std::string Catalogue::DisplayName(int32_t id) const {
    const ItemEntry* e = ByTemplate(id);
    if (e && !e->name.empty()) return e->name;
    if (e && !e->rawName.empty()) return e->rawName;
    return "Conan item " + std::to_string(id);
}

std::string Catalogue::CodeFor(int32_t id) const {
    const ItemEntry* e = ByTemplate(id);
    return e ? e->code : std::to_string(id);
}

}  // namespace conan
