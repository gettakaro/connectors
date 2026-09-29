#include "weapon.h"

#include "names.h"

#include <algorithm>

const char* ItemLabelById(uint32_t itemId) {
    const ItemLabel* end = kItemLabels + kItemLabelCount;
    const ItemLabel* it =
        std::lower_bound(kItemLabels, end, itemId, [](const ItemLabel& l, uint32_t id) { return l.itemId < id; });
    return it != end && it->itemId == itemId ? it->name : nullptr;
}

const char* WeaponCategoryName(uint32_t categoryId) {
    const WeaponCategoryLabel* end = kWeaponCategoryLabels + kWeaponCategoryLabelCount;
    const WeaponCategoryLabel* it = std::lower_bound(
        kWeaponCategoryLabels, end, categoryId, [](const WeaponCategoryLabel& l, uint32_t id) { return l.id < id; });
    return it != end && it->id == categoryId ? it->name : nullptr;
}

KillWeapon ResolveKillWeapon(uint32_t itemId, const char* itemCode, uint32_t weaponCategoryId) {
    if (itemId) {
        if (const char* n = ItemLabelById(itemId)) return {n, "item"};
        if (itemCode && *itemCode) return {DisplayName(itemCode, NameKind::Item), "itemCode"};
    }
    if (const char* c = WeaponCategoryName(weaponCategoryId)) return {c, "category"};
    return {"Unknown", "unknown"};
}
