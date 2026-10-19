#include "items_kfc.h"

#include "common.h"
#include "scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

#include "../third_party/zstd/zstd.h"

namespace items_kfc {
namespace {

bool ReadAll(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// keen reflection type: {name*, len, name*, len, fullName*, len, ..., +0x48 u32 count, +0x50 u32 hash, +0x58 fields*,
// +0x60 enum values*}. Fields are {name*, u64 len, type*, u64 offset} 0x30 apart, enum values {name*, u64 len,
// u64 value, ...} 0x28 apart. `t` is the type for the full name `fullName` (the pointer at +0x20).
template <class F>
bool ForEachType(const char* fullName, F f) {
    uint32_t s = scan::FindCString(fullName);
    if (!s) return false;
    for (uint32_t ref : scan::FindPointerRefs(s, 8))
        if (ref >= 0x20 && f(ref - 0x20)) return true;
    return false;
}

std::string Name(uint64_t ptr, uint64_t len) {
    if (ptr < scan::PointerBase() || len == 0 || len > 128) return "";
    return std::string((const char*)scan::Ptr((uint32_t)(ptr - scan::PointerBase())), (size_t)len);
}

bool EnumNames(const char* fullName, std::vector<std::string>& out) {
    return ForEachType(fullName, [&](uint32_t t) {
        uint32_t count = (uint32_t)scan::ReadU64(t + 0x48);
        uint64_t vals = scan::ReadU64(t + 0x60);
        if (!count || count > 256 || vals < scan::PointerBase()) return false;
        uint32_t v = (uint32_t)(vals - scan::PointerBase());
        std::vector<std::string> names;
        for (uint32_t k = 0; k < count; k++) {
            std::string n = Name(scan::ReadU64(v + k * 0x28), scan::ReadU64(v + k * 0x28 + 8));
            uint64_t value = scan::ReadU64(v + k * 0x28 + 0x10);
            if (n.empty() || value > 255) return false;
            if (names.size() <= value) names.resize((size_t)value + 1);
            names[(size_t)value] = n;
        }
        out = names;
        return true;
    });
}

uint32_t U32(const std::string& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) memcpy(&v, d.data() + at, 4);
    return v;
}

std::string Enum(const std::vector<std::string>& names, uint8_t v) {
    return v < names.size() && !names[v].empty() ? names[v] : std::to_string(v);
}

// Storage behind the ItemDef pointers handed out by Load (never freed: the table lives as long as the process).
std::vector<ItemDef> g_defs;
std::deque<std::string> g_strings;

const char* Keep(const std::string& s) {
    g_strings.push_back(s);
    return g_strings.back().c_str();
}

void Publish(const std::vector<Row>& rows, const ItemDef*& items, size_t& count) {
    std::map<std::string, const char*> shared;  // category and rarity repeat on every row
    auto keepShared = [&](const std::string& s) {
        auto it = shared.find(s);
        return it != shared.end() ? it->second : (shared[s] = Keep(s));
    };
    g_defs.clear();
    g_defs.reserve(rows.size());
    for (auto& r : rows) {
        std::string name = r.code;
        for (auto& c : name)
            if (c == '_') c = ' ';
        g_defs.push_back({r.itemId, r.maxStack, r.pide, Keep(r.code), Keep(name), keepShared(r.category), keepShared(r.rarity)});
    }
    items = g_defs.data();
    count = g_defs.size();
}

std::string CachePath() { return PluginBaseDir() + "\\takaro\\enshrouded-items.json"; }

bool LoadCache(const std::string& gameBuild, std::vector<Row>& rows) {
    std::string text;
    JsonValue root;
    if (!ReadAll(CachePath(), text) || !JsonParse(text, root) || root.type != JsonValue::Object) return false;
    const JsonValue* b = root.get("gameBuild");
    const JsonValue* list = root.get("items");
    if (!b || !b->isStr() || b->str != gameBuild || !list || list->type != JsonValue::Array || list->arr.empty()) return false;
    rows.clear();
    for (auto& e : list->arr) {
        if (e.type != JsonValue::Array || e.arr.size() != 6 || !e.arr[0].isNum() || !e.arr[1].isNum() || !e.arr[2].isNum() ||
            !e.arr[3].isStr() || !e.arr[4].isStr() || !e.arr[5].isStr())
            return false;
        rows.push_back({(uint32_t)strtoul(e.arr[0].str.c_str(), nullptr, 10), (uint16_t)e.arr[1].num, (uint8_t)(e.arr[2].num != 0),
                        e.arr[3].str, e.arr[4].str, e.arr[5].str});
    }
    return true;
}

bool SaveCache(const std::string& gameBuild, const std::string& version, const std::vector<Row>& rows) {
    std::string s = "{\n  \"gameBuild\": " + JsonStr(gameBuild) + ",\n  \"note\": " +
                    JsonStr("rebuilt from enshrouded_server.kfc (" + version + ") by plugin " TAKARO_PLUGIN_VERSION " at " +
                            IsoNowUtc() + "; rows are [itemId, maxStackSize, pide, code, category, rarity]") +
                    ",\n  \"items\": [";
    for (size_t i = 0; i < rows.size(); i++) {
        auto& r = rows[i];
        s += std::string(i ? ",\n    " : "\n    ") + "[" + std::to_string(r.itemId) + "," + std::to_string(r.maxStack) + "," +
             std::to_string(r.pide) + "," + JsonStr(r.code) + "," + JsonStr(r.category) + "," + JsonStr(r.rarity) + "]";
    }
    s += "\n  ]\n}\n";
    FILE* f = fopen(CachePath().c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    return ok;
}

}  // namespace

bool LayoutFromReflection(Layout& out, std::string& err) {
    Layout l;
    bool found = ForEachType("keen::ItemInfo", [&](uint32_t t) {
        uint32_t count = (uint32_t)scan::ReadU64(t + 0x48), hash = (uint32_t)scan::ReadU64(t + 0x50);
        uint64_t fields = scan::ReadU64(t + 0x58);
        if (!hash || !count || count > 512 || fields < scan::PointerBase()) return false;
        uint32_t f = (uint32_t)(fields - scan::PointerBase());
        std::map<std::string, uint32_t> at;
        for (uint32_t k = 0; k < count; k++)
            at[Name(scan::ReadU64(f + k * 0x30), scan::ReadU64(f + k * 0x30 + 8))] = (uint32_t)scan::ReadU64(f + k * 0x30 + 0x18);
        for (auto* n : {"itemId", "maxStackSize", "rarity", "category", "flags", "debugName"})
            if (!at.count(n)) return false;
        l.typeHash = hash;
        l.itemId = at["itemId"], l.maxStackSize = at["maxStackSize"], l.rarity = at["rarity"];
        l.category = at["category"], l.flags = at["flags"], l.debugName = at["debugName"];
        return true;
    });
    if (!found) {
        err = "keen::ItemInfo reflection type not found";
        return false;
    }
    std::vector<std::string> itemFlags;
    if (!EnumNames("keen::ItemCategory", l.categories) || !EnumNames("keen::ItemRarity", l.rarities) ||
        !EnumNames("keen::ItemFlag", itemFlags)) {
        err = "item enum reflection (ItemCategory/ItemRarity/ItemFlag) not found";
        return false;
    }
    for (size_t i = 0; i < itemFlags.size(); i++)
        if (itemFlags[i] == "HasPIDE") l.pideBit = (int)i;
    if (l.pideBit < 0 || l.pideBit > 31) {
        err = "ItemFlag::HasPIDE not found";
        return false;
    }
    out = l;
    return true;
}

// KFC4 container (layout from Brabb3l/kfc-parser): 16 x {u32 offset (from that field), u32 count} after a 16-byte
// header; resource keys are 32 bytes, values {u32 offset, u32 size} into the uncompressed resource stream, which is
// stored as zstd chunks {u32 offset, size, compressedSize, uncompressedOffset, uncompressedSize} in <kfc>_resources.
bool ReadKfc(const std::string& kfcPath, const Layout& l, std::vector<Row>& rows, std::string& version, std::string& err) {
    std::string d;
    if (!ReadAll(kfcPath, d)) {
        err = "cannot read " + kfcPath;
        return false;
    }
    if (d.size() < 16 + 16 * 8 || memcmp(d.data(), "KFC4", 4) != 0) {
        err = kfcPath + " is not a KFC4 container";
        return false;
    }
    struct Loc {
        size_t at;
        uint32_t count;
    } loc[16];
    for (int i = 0; i < 16; i++) loc[i] = {16 + (size_t)i * 8 + U32(d, 16 + i * 8), U32(d, 16 + i * 8 + 4)};
    const Loc &ver = loc[0], &ridx = loc[5], &rv = loc[11], &bk = loc[13], &bv = loc[14], &ch = loc[15];
    auto fits = [&](const Loc& x, size_t stride) { return x.at <= d.size() && (uint64_t)x.count * stride <= d.size() - x.at; };
    if (!fits(ver, 1) || !fits(ridx, 4) || !fits(rv, 8) || !fits(bk, 4) || !fits(bv, 12) || !fits(ch, 20)) {
        err = "KFC4 table outside the file";
        return false;
    }
    version.assign(d.data() + ver.at, ver.count);

    uint32_t first = 0, n = 0;
    bool have = false;
    for (uint32_t i = 0; i < bk.count && !have; i++)
        if (U32(d, bk.at + i * 4) == l.typeHash) first = U32(d, bv.at + i * 12 + 4), n = U32(d, bv.at + i * 12 + 8), have = true;
    if (!have || !n || (uint64_t)first + n > ridx.count) {
        err = "no ItemInfo resources in " + kfcPath;
        return false;
    }

    FILE* res = fopen((kfcPath + "_resources").c_str(), "rb");
    if (!res) {
        err = "cannot read " + kfcPath + "_resources";
        return false;
    }
    std::map<uint32_t, std::string> chunks;  // decompressed, by chunk index
    auto readRange = [&](uint64_t off, uint32_t size, std::string& out) {
        out.clear();
        for (uint32_t c = 0; c < ch.count; c++) {
            size_t e = ch.at + (size_t)c * 20;
            uint64_t uoff = U32(d, e + 12), usz = U32(d, e + 16);
            if (uoff + usz <= off || uoff >= off + size) continue;
            auto it = chunks.find(c);
            if (it == chunks.end()) {
                std::string packed(U32(d, e + 8), '\0');
                std::string plain((size_t)usz, '\0');
                if (_fseeki64(res, (long long)U32(d, e), SEEK_SET) != 0 || fread(&packed[0], 1, packed.size(), res) != packed.size())
                    return false;
                size_t got = ZSTD_decompress(&plain[0], plain.size(), packed.data(), packed.size());
                if (ZSTD_isError(got) || got != plain.size()) return false;
                it = chunks.emplace(c, std::move(plain)).first;
            }
            uint64_t a = (off > uoff ? off : uoff) - uoff, b = (off + size < uoff + usz ? off + size : uoff + usz) - uoff;
            out.append(it->second, (size_t)a, (size_t)(b - a));
        }
        return out.size() == size;
    };

    rows.clear();
    std::string item;
    bool ok = true;
    for (uint32_t i = first; i < first + n && ok; i++) {
        uint32_t k = U32(d, ridx.at + (size_t)i * 4);
        if (k >= rv.count || !readRange(U32(d, rv.at + (size_t)k * 8), U32(d, rv.at + (size_t)k * 8 + 4), item) ||
            item.size() < l.debugName + 8 || item.size() < l.flags + 4) {
            err = "ItemInfo resource " + std::to_string(i - first) + " unreadable";
            ok = false;
            break;
        }
        // debugName is a BlobString {u32 offset from the field, u32 size}
        uint32_t so = U32(item, l.debugName), sl = U32(item, l.debugName + 4);
        if ((uint64_t)l.debugName + so + sl > item.size() || !sl) {
            err = "ItemInfo resource " + std::to_string(i - first) + " has no debugName";
            ok = false;
            break;
        }
        uint16_t stack;
        memcpy(&stack, item.data() + l.maxStackSize, 2);
        rows.push_back({U32(item, l.itemId), stack, (uint8_t)((U32(item, l.flags) >> l.pideBit) & 1),
                        item.substr(l.debugName + so, sl), Enum(l.categories, (uint8_t)item[l.category]),
                        Enum(l.rarities, (uint8_t)item[l.rarity])});
    }
    fclose(res);
    return ok;
}

bool Load(const std::string& gameBuild, const ItemDef*& items, size_t& count, std::string& source, std::string& err) {
    std::vector<Row> rows;
    if (LoadCache(gameBuild, rows)) {
        Publish(rows, items, count);
        source = "takaro\\enshrouded-items.json (game build " + gameBuild + ")";
        return true;
    }
    Layout l;
    std::string version;
    if (!LayoutFromReflection(l, err) || !ReadKfc(PluginBaseDir() + "\\enshrouded_server.kfc", l, rows, version, err)) return false;
    if (!SaveCache(gameBuild, version, rows)) PluginLog("[items] could not write %s", CachePath().c_str());
    Publish(rows, items, count);
    source = "rebuilt from enshrouded_server.kfc for game build " + gameBuild;
    PluginLog("[items] %zu items rebuilt from enshrouded_server.kfc (%s) for game build %s", count, version.c_str(),
              gameBuild.c_str());
    return true;
}

}  // namespace items_kfc
