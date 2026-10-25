// Offline check of the item-table rebuild (src/items_kfc.cpp): maps enshrouded_server.exe as an image (nothing runs),
// reads the ItemInfo layout from its reflection tables, rebuilds the table from enshrouded_server.kfc and compares
// it with the compiled table (src/gamedata.cpp). Build with zig for x86_64-windows-gnu and run on Windows:
//   items_kfc_test.exe <enshrouded_server.exe> <enshrouded_server.kfc>
//   items_kfc_test.exe <enshrouded_server.exe> --load <game build>   (as the plugin does at startup, in the exe's folder:
//                      reads takaro\enshrouded-items.json for that build, else rebuilds it from the kfc and saves it)
#include "common.h"
#include "gamedata.h"
#include "items_kfc.h"
#include "scan.h"

#include <cstdio>
#include <cstring>
#include <map>

HMODULE g_selfModule = nullptr;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HMODULE m = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) {
        fprintf(stderr, "cannot map %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }
    if (!scan::InitAt((uintptr_t)m & ~(uintptr_t)3)) return 3;
    if (argc > 3 && strcmp(argv[2], "--load") == 0) {
        const ItemDef* items = nullptr;
        size_t count = 0;
        std::string source, err;
        if (!items_kfc::Load(argv[3], items, count, source, err)) {
            fprintf(stderr, "load: %s\n", err.c_str());
            return 1;
        }
        printf("load: %zu items from %s; first %s (%u)\n", count, source.c_str(), count ? items[0].code : "-", count ? items[0].itemId : 0);
        return 0;
    }
    items_kfc::Layout l;
    std::string err, version;
    if (!items_kfc::LayoutFromReflection(l, err)) {
        fprintf(stderr, "layout: %s\n", err.c_str());
        return 1;
    }
    printf("layout: type 0x%08x itemId +0x%x maxStackSize +0x%x rarity +0x%x category +0x%x flags +0x%x debugName +0x%x "
           "HasPIDE bit %d, %zu categories, %zu rarities\n",
           l.typeHash, l.itemId, l.maxStackSize, l.rarity, l.category, l.flags, l.debugName, l.pideBit, l.categories.size(),
           l.rarities.size());
    std::vector<items_kfc::Row> rows;
    if (!items_kfc::ReadKfc(argv[2], l, rows, version, err)) {
        fprintf(stderr, "kfc: %s\n", err.c_str());
        return 1;
    }
    std::map<uint32_t, const ItemDef*> compiled;
    for (size_t i = 0; i < kItemCount; i++) compiled[kItems[i].itemId] = &kItems[i];
    size_t same = 0, changed = 0, added = 0;
    for (auto& r : rows) {
        auto it = compiled.find(r.itemId);
        if (it == compiled.end()) {
            if (added++ < 10) printf("new     %u %s\n", r.itemId, r.code.c_str());
            continue;
        }
        const ItemDef* d = it->second;
        if (r.code != d->code || r.maxStack != d->maxStack || r.pide != d->pide || r.category != d->category ||
            r.rarity != d->rarity) {
            if (changed++ < 10)
                printf("changed %u %s/%s stack %u/%u pide %u/%u %s/%s %s/%s\n", r.itemId, r.code.c_str(), d->code, r.maxStack,
                       d->maxStack, r.pide, d->pide, r.category.c_str(), d->category, r.rarity.c_str(), d->rarity);
        } else {
            same++;
        }
        compiled.erase(it);
    }
    printf("%s: %zu items, %zu same as the compiled table, %zu changed, %zu new, %zu gone\n", version.c_str(), rows.size(), same,
           changed, added, compiled.size());
    return 0;
}
