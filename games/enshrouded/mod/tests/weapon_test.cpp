// The weapon an entity-killed event reports: item display names from the client's En_Us
// localisation, the derived-name fallback, the weapon category fallback and Unarmed.
#include "gamedata.h"
#include "weapon.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0, checks = 0;

void Expect(uint32_t itemId, const char* code, uint32_t category, const char* name, const char* source) {
    checks++;
    KillWeapon w = ResolveKillWeapon(itemId, code, category);
    if (w.name != name || strcmp(w.source, source) != 0) {
        std::printf("  item=%u code=%s cat=%u -> \"%s\" (%s), wanted \"%s\" (%s)\n", itemId, code ? code : "-",
                    category, w.name.c_str(), w.source, name, source);
        failures++;
    }
}

void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        failures++;
    }
}

}  // namespace

int main() {
    const uint32_t kSword = 1404206905u, kBow = 1382431183u, kStaff = 3956108896u, kWand = 2533728086u,
                   kFists = 3128520336u;
    // weapons by their PIDE item: the in-game names
    Expect(1275936258u, "Weapon_T1_1H_Sword_Craft", kSword, "Scrappy Sword", "item");
    Expect(327157552u, "Weapon_T1_2H_Bow_02", kBow, "Forest Longbow", "item");
    Expect(1157288032u, "Weapon_T1_2H_Staff_01", kStaff, "Shepherd's Staff", "item");
    Expect(3748173823u, "Weapon_T0_1H_Wand_Bone_Ice_Craft", kWand, "Wand", "item");
    Expect(3595411093u, "Weapon_T2_2H_GreatSword_01", 359923632u, "Copperblade Greatsword", "item");
    // an item the localisation does not name: the code-derived name, never the raw code
    Expect(123u, "Weapon_T9_1H_Sword_Imaginary", kSword, "1H Sword Imaginary (Tier 9)", "itemCode");
    // no item: the category
    Expect(0, nullptr, kBow, "Bow", "category");
    Expect(0, nullptr, kFists, "Unarmed", "category");
    Expect(0, nullptr, 155579452u, "Axe", "category");
    // nothing at all: still a string (Takaro drops an entity-killed without one)
    Expect(0, nullptr, 0, "Unknown", "unknown");
    Expect(424242u, nullptr, 0, "Unknown", "unknown");

    // tables: sorted for the binary search, every entry named
    bool sorted = true, named = true;
    for (size_t i = 0; i < kItemLabelCount; i++) {
        if (i && kItemLabels[i - 1].itemId >= kItemLabels[i].itemId) sorted = false;
        if (!kItemLabels[i].name[0] || kItemLabels[i].name[0] == ' ') named = false;
    }
    for (size_t i = 1; i < kWeaponCategoryLabelCount; i++)
        if (kWeaponCategoryLabels[i - 1].id >= kWeaponCategoryLabels[i].id) sorted = false;
    Check(sorted, "label tables sorted by id");
    Check(named, "every item label non-empty and trimmed");
    // every labelled id is a real item of the server's table (same ItemId space)
    size_t known = 0;
    for (size_t i = 0; i < kItemLabelCount; i++)
        for (size_t q = 0; q < kItemCount; q++)
            if (kItems[q].itemId == kItemLabels[i].itemId) {
                known++;
                break;
            }
    Check(known * 100 >= kItemLabelCount * 95, ">=95% of labels match a server item id");
    // every weapon of the server table resolves to a non-empty name
    size_t weapons = 0, labelled = 0;
    for (size_t q = 0; q < kItemCount; q++)
        if (!strcmp(kItems[q].category, "Weapons")) {
            weapons++;
            KillWeapon w = ResolveKillWeapon(kItems[q].itemId, kItems[q].code, 0);
            if (!w.name.empty() && strcmp(w.source, "unknown") != 0) labelled++;
        }
    Check(weapons > 0 && labelled == weapons, "every server weapon resolves to a name");

    std::printf("weapon: %d checks, %zu item labels (%zu known to the server table), %zu categories, %d failures\n",
                checks, kItemLabelCount, known, kWeaponCategoryLabelCount, failures);
    return failures ? 1 : 0;
}
