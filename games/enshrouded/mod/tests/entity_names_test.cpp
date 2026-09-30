// The names entity-killed, the player-death message and listEntities give a creature, and the
// items listItems answers with.
//
// Entities: the client's En_Us name where the client has one (bosses by their health bar,
// NPCs, and the variants of a named template), else the code-derived name, else the raw code;
// the catalogue keeps creatures and NPCs and drops colliders, weak spots, traps and dev
// templates. Items: only what the client names, with the client's name, never an Ability_.
#include "entity.h"
#include "gamedata.h"
#include "names.h"
#include "weapon.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0, checks = 0;

void Check(bool ok, const std::string& what) {
    checks++;
    if (!ok) {
        std::printf("  FAIL %s\n", what.c_str());
        failures++;
    }
}

const EntityDef* ByCode(const char* code) {
    for (size_t i = 0; i < kEntityCount; i++)
        if (std::strcmp(kEntities[i].code, code) == 0) return &kEntities[i];
    return nullptr;
}

void Expect(const char* code, const char* name, const char* source) {
    const EntityDef* e = ByCode(code);
    Check(e != nullptr, std::string("gamedata has ") + code);
    if (!e) return;
    EntityName n = ResolveEntityName(e->guid, e->code);
    Check(n.name == name && std::strcmp(n.source, source) == 0,
          std::string(code) + " -> \"" + n.name + "\" (" + n.source + "), wanted \"" + name + "\" (" + source + ")");
}

bool Lowercase(const std::string& s) {
    for (char c : s)
        if (c >= 'A' && c <= 'Z') return false;
    return true;
}

}  // namespace

int main() {
    // The client's own names: boss health bars and NPCs.
    Expect("Enemy_Fogger_Heavy_BossHealthBar", "Fell Thunderbrute", "client");
    Expect("Enemy_Boss_GiantLizard", "Fell Wispwyvern", "client");
    Expect("Enemy_Fog_Predator_BossHealthBar", "Shroud Stalker", "client");
    Expect("Enemy_Midboss_Vukah_Bigfoot_BossHealthBar", "Vukah Brawler", "client");
    Expect("NPC_Workshop_Blacksmith01", "Oswald Anders", "client");
    Expect("NPC_Workshop_cryptKeeper_Assistant_01", "Nameless One", "client");
    // A variant of a named template: the named one's name.
    Expect("Enemy_Fogger_Heavy_SummonStone", "Fell Thunderbrute", "sibling");
    Expect("Enemy_Fogger_MageHeavy_hook", "Fell Sicklescythe", "sibling");
    Expect("Enemy_Fog_Predator", "Shroud Stalker", "sibling");
    Expect("NPC_Workshop_Alchemist01", "Balthazar", "sibling");
    // No client name anywhere (the En_Us table has no rat): the cleaned code.
    Expect("Enemy_Wildbeast_Rat_hook", "Rat", "code");
    Expect("Enemy_Fog_Bug_Critter_hook", "Fog Bug Critter", "code");
    Expect("Enemy_Wildbeast_Wolf_hook_AG2", "Wolf", "code");
    Expect("Enemy_Scavenger_Melee01", "Scavenger Melee", "code");
    // A fish: its catch's name.
    Expect("Fish_T1_common_01", "Shimmerfin", "fishItem");
    Expect("Animal_wildlife_Fish_T6_epic_02_tame", "Yellowfin", "fishItem");
    Expect("Animal_Wildlife_Fish_Big03_DarkBlue", "Fish Big Dark Blue", "code");
    // Unknown template: whatever the caller has.
    {
        EntityName n = ResolveEntityName(nullptr, nullptr, "entity#2402");
        Check(n.name == "entity#2402" && std::strcmp(n.source, "raw") == 0, "unknown template keeps the raw id");
        EntityName u = ResolveEntityName(nullptr, nullptr);
        Check(u.name == "Unknown", "no template and no id is still a string");
        const uint8_t zero[16] = {0};
        Check(EntityLabelByGuid(zero) == nullptr, "zero guid has no label");
    }

    // The label table: sorted, every row a known template, names clean.
    size_t client = 0, sibling = 0;
    for (size_t i = 0; i < kEntityLabelCount; i++) {
        const EntityLabel& l = kEntityLabels[i];
        if (i) Check(std::memcmp(kEntityLabels[i - 1].guid, l.guid, 16) < 0, "labels sorted by guid");
        bool known = false;
        for (size_t k = 0; k < kEntityCount && !known; k++) known = std::memcmp(kEntities[k].guid, l.guid, 16) == 0;
        Check(known, std::string("label for a known template: ") + l.name);
        Check(*l.name && !std::strchr(l.name, '<') && !std::strchr(l.name, '_'), std::string("clean label: ") + l.name);
        if (std::strcmp(l.source, "client") == 0)
            client++;
        else if (std::strcmp(l.source, "sibling") == 0)
            sibling++;
        else
            Check(std::strcmp(l.source, "fishItem") == 0, std::string("known label source: ") + l.source);
    }

    // What the catalogue keeps.
    Check(IsCatalogueEntity("Enemy_Wildbeast_Rat_hook"), "rat is a creature");
    Check(IsCatalogueEntity("NPC_Workshop_Blacksmith01"), "blacksmith is an NPC");
    Check(IsCatalogueEntity("Animal_Wildlife_T2_Bunny_AG2"), "bunny is a creature");
    for (const char* not_one :
         {"Enemy_Collider_Rat_OnContact_TurnAggressive", "Enemy_Weakspots_Wildlife_Rat_Head", "Trap_Wall_Fireball",
          "Base_Enemy", "1_Player", "Zone_ThrowIntoAir_JumpPad_High_Jump", "Projectile_Enemy_Fogger_Mage_Skull",
          "Enemy_Wildbeast_Rat_huge_test_hook", "PLACEMENT_HELPER_x", "TreasureContainer", "Shroud_Gem_Forge_01_Grassland",
          "Enemy_Lizardpeople_Weapon_Spear", "Attack_AncientConstruct_Heavy_LaserGrid", "VFX_SubEntity_Prop_Lantern_Light",
          "_DEPRECATED_NPC_Workshop_Alchemist_8k"})
        Check(!IsCatalogueEntity(not_one), std::string("not a catalogue entity: ") + not_one);

    std::vector<const EntityDef*> rows;
    std::vector<std::string> names;
    CatalogueEntities(rows, names);
    Check(rows.size() == names.size() && !rows.empty(), "one name per catalogue row");
    size_t labelled = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const std::string& n = names[i];
        const std::string code = rows[i]->code;
        std::string opened = code;
        for (char& c : opened) c = c == '_' ? ' ' : c;
        Check(!n.empty() && n != code && n != opened && n.find('_') == std::string::npos && !Lowercase(n),
              "catalogue name " + code + " -> \"" + n + "\"");
        for (const char* token : {"Hook", "Enemy ", "Wildbeast", "Health", "Summon Stone", "AG2", "(variant"})
            Check(n.find(token) == std::string::npos, "catalogue name " + code + " -> \"" + n + "\" keeps " + token);
        // listEntities and entity-killed name a template the same way
        Check(n == ResolveEntityName(rows[i]->guid, rows[i]->code).name, "catalogue name = kill name for " + code);
        if (EntityLabelByGuid(rows[i]->guid)) labelled++;
    }

    // Items: only what the client names, never an ability, never markup.
    size_t items = 0, unnamed = 0, abilities = 0;
    for (size_t i = 0; i < kItemCount; i++) {
        const char* label = ItemLabelById(kItems[i].itemId);
        if (!label) unnamed++;
        if (label && std::strncmp(kItems[i].code, "Ability_", 8) == 0) abilities++;
        if (!IsCatalogueItem(kItems[i].code, label)) continue;
        items++;
        Check(!std::strchr(label, '<') && std::strcmp(label, kItems[i].code) != 0,
              std::string("item name ") + kItems[i].code + " -> " + label);
    }
    Check(!IsCatalogueItem("Ability_Evade", "Dodge Roll"), "an ability is not an item");
    Check(!IsCatalogueItem("VC_Beard10_balbo", nullptr), "an unnamed item is not listed");
    Check(IsCatalogueItem("Weapon_T3_2H_Bow_Rattlesnake", "Rattlesnake Bow"), "a named weapon is listed");
    for (size_t i = 0; i < kItemLabelCount; i++)
        Check(!std::strchr(kItemLabels[i].name, '<'), std::string("item label without markup: ") + kItemLabels[i].name);

    std::printf("entity labels: %zu (%zu client, %zu sibling, %zu fish) of %zu templates\n", kEntityLabelCount, client,
                sibling, kEntityLabelCount - client - sibling, kEntityCount);
    std::printf("listEntities: %zu of %zu templates, %zu with a client name, %zu code-derived\n", rows.size(), kEntityCount,
                labelled, rows.size() - labelled);
    std::printf("listItems: %zu of %zu items (%zu unnamed and %zu named abilities left out)\n", items, kItemCount, unnamed,
                abilities);
    std::printf("entity_names_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
