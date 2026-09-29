// The weapon of a killing blow, as a player reads it in-game.
//
// HitEvent.weaponPideId names the PIDE entity of the weapon that dealt the blow (for an
// arrow or a spell it is the bow, staff or wand that fired it). The PIDE entity's
// ItemState carries the ItemId, and the ItemId has an English display name in
// kItemLabels, baked from the game client's En_Us localisation by
// tools/gen_item_labels.py (the dedicated server ships no localisation). When no item can
// be named, HitEvent.weaponCategory (a WeaponCategoryId) still names the kind of weapon.
//
// Pure lookups over static tables: no Windows, no game memory. tests/weapon_test.cpp
// links this on the host.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct ItemLabel {
    uint32_t itemId;
    const char* name;  // English display name from the client's localisation
};
struct WeaponCategoryLabel {
    uint32_t id;        // WeaponCategoryId: FNV-1a of the WeaponCategory resource guid
    const char* name;   // readable category, "Unarmed" for fists
};

extern const ItemLabel kItemLabels[];  // sorted by itemId
extern const size_t kItemLabelCount;
extern const WeaponCategoryLabel kWeaponCategoryLabels[];  // sorted by id
extern const size_t kWeaponCategoryLabelCount;

// English display name of an item, or nullptr when the client has none for it.
const char* ItemLabelById(uint32_t itemId);
// Whether an item belongs in listItems: the client names it (label is ItemLabelById's answer)
// and it is not an Ability_ row, which is a skill the game models as an item.
bool IsCatalogueItem(const char* code, const char* label);
// Readable name of a WeaponCategoryId, or nullptr when unknown.
const char* WeaponCategoryName(uint32_t categoryId);

struct KillWeapon {
    std::string name;    // never empty: Takaro's entity-killed needs a string
    const char* source;  // "item" | "itemCode" | "category" | "unknown"
};

// itemId is the ItemState.itemId of the weapon PIDE (0 when there was none or it could not
// be read); itemCode is its gamedata code (nullptr when not in the table).
KillWeapon ResolveKillWeapon(uint32_t itemId, const char* itemCode, uint32_t weaponCategoryId);
