// The names entity-killed, the player-death message and listEntities give a creature, and the
// items listItems answers with.
//
// Entities: the game client's En_Us name (entity.h: its own, the Logic Spawner's, an item's, or
// that of the named templates sharing its model, code or arsenal), never one built from the code;
// a template without one is left out of listEntities and named by its faction's in-game word in a
// kill. The catalogue keeps creatures and NPCs and drops colliders, weak spots, traps and dev
// templates. Items: only what the client names, with the client's name, never an Ability_ or a
// character-creator Icon_ swatch.
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

void Expect(const char* code, const char* name, const char* source, bool listed = true) {
    const EntityDef* e = ByCode(code);
    Check(e != nullptr, std::string("gamedata has ") + code);
    if (!e) return;
    EntityName n = ResolveEntityName(e);
    Check(n.name == name && std::strcmp(n.source, source) == 0,
          std::string(code) + " -> \"" + n.name + "\" (" + n.source + "), wanted \"" + name + "\" (" + source + ")");
    std::vector<const EntityDef*> rows;
    std::vector<std::string> names;
    CatalogueEntities(rows, names);
    bool in = false;
    for (const EntityDef* r : rows) in = in || r == e;
    Check(in == listed, std::string(code) + (listed ? " is listed" : " is left out of listEntities"));
}

// Whether a name could have been built from the code: the code with spaces for underscores, or a
// code fragment the old derivation left behind.
bool CodeLike(const std::string& name, const std::string& code) {
    std::string opened = code;
    for (char& c : opened) c = c == '_' ? ' ' : c;
    if (name == code || name == opened || name.find('_') != std::string::npos) return true;
    for (const char* frag : {"Enemy", "Hook", "hook", "Wildbeast", "Wildlife ", "Fogger", "Skeleton", "Health Bar", "Summon Stone",
                             "AG2", "(Tier", "(variant", "Melee", "Ranger0", "Lizardpeople", "Npc ", "NPC", "Workshop"})
        if (name.find(frag) != std::string::npos) return true;
    return false;
}

bool Lowercase(const std::string& s) {
    for (char c : s)
        if (c >= 'A' && c <= 'Z') return false;
    return true;
}

}  // namespace

int main() {
    // The client's own names: boss health bars and NPCs.
    Expect("Enemy_Fogger_Heavy_BossHealthBar", "Armored Fell Thunderbrute", "client");  // renamed in 1.0
    Expect("Enemy_Boss_GiantLizard", "Fell Wispwyvern", "client");
    Expect("Enemy_Fog_Predator_BossHealthBar", "Shroud Stalker", "client");
    Expect("Enemy_Midboss_Vukah_Bigfoot_BossHealthBar", "Vukah Brawler", "client");
    Expect("NPC_Workshop_Blacksmith01", "Oswald Anders", "client");
    Expect("NPC_Workshop_cryptKeeper_Assistant_01", "Nameless One", "client");
    Expect("NPC_Workshop_Alchemist01", "Balthazar", "client");
    Expect("NPC_Workshop_Farmer01", "Emily Fray", "client");  // her dialog headline
    Expect("Townsfolk_Female_villager_02_MarianneReed", "Marianne Reed", "client");
    // The Logic Spawner's names: the regular creatures.
    Expect("Enemy_Wildbeast_Rat_hook", "Rat", "logicSpawn");
    Expect("Enemy_Fog_Bug_Critter_hook", "Fell Critter", "logicSpawn");
    Expect("Enemy_Wildbeast_Wolf_hook_AG2", "Wolf", "logicSpawn");
    Expect("Enemy_Scavenger_Melee01", "Scavenger Scalper", "logicSpawn");
    Expect("Enemy_Scavenger_Melee_Weakling", "Scavenger Scab", "logicSpawn");
    Expect("Enemy_Fog_Predator", "Shroud Stalker", "logicSpawn");
    Expect("Enemy_Fogger_MageHeavy_hook", "Fell Sicklescythe", "logicSpawn");
    // An item that is the creature: a fish by its catch.
    Expect("Fish_T1_common_01", "Shimmerfin", "item");
    Expect("Animal_wildlife_Fish_T6_epic_02_tame", "Yellowfin", "item");
    // The named templates sharing its model, code (looks and spawn suffixes dropped) or arsenal.
    Expect("Enemy_Fogger_Heavy_SummonStone", "Armored Fell Thunderbrute", "model");
    Expect("Animal_Wildlife_T1_Goat_AG2", "Common Goat", "model");
    Expect("Enemy_Skeleton_Weakling01", "Hollow Wretch", "model");
    Expect("Enemy_Wildbeast_Rat_Coldheights_Variation_hook", "Rat", "model");
    Expect("Enemy_Scavenger_Melee07", "Scavenger Scalper", "sibling");
    Expect("Enemy_Fogger_Soldier03", "Fell Soldier I", "sibling");
    Expect("Enemy_Fogger_Ranger_Armored_T3_02", "Fell Scout II", "sibling");
    Expect("Animal_wildlife_Fish_T1_common_02", "Lakehopper", "sibling");  // not _01's Shimmerfin
    Expect("Enemy_Scavenger_Ranger02", "Scavenger Cannoneer", "arsenal");
    // No in-game name anywhere: left out of listEntities; a kill names the faction or nothing.
    Expect("Enemy_Fog_VorgothOrgan_Eye_SubEnemy", "Fell", "faction", false);
    Expect("Enemy_Wildbeast_Crocodile_Amphibious", "Wildlife", "faction", false);
    Expect("Enemy_Tornado_Desert_PROTOTYPE", "Unknown", "unknown", false);
    Expect("Animal_Wildlife_Fish_Big03_DarkBlue", "Unknown", "unknown", false);
    {
        EntityName u = ResolveEntityName(nullptr);
        Check(u.name == "Unknown" && std::strcmp(u.source, "unknown") == 0, "a template outside the table is Unknown");
        const uint8_t zero[16] = {0};
        Check(EntityLabelByGuid(zero) == nullptr, "zero guid has no label");
    }

    // The label table: sorted, every row a known template, names clean, known sources.
    size_t by_source[6] = {0};
    const char* sources[6] = {"client", "logicSpawn", "item", "model", "sibling", "arsenal"};
    for (size_t i = 0; i < kEntityLabelCount; i++) {
        const EntityLabel& l = kEntityLabels[i];
        if (i) Check(std::memcmp(kEntityLabels[i - 1].guid, l.guid, 16) < 0, "labels sorted by guid");
        const EntityDef* def = nullptr;
        for (size_t k = 0; k < kEntityCount && !def; k++)
            if (std::memcmp(kEntities[k].guid, l.guid, 16) == 0) def = &kEntities[k];
        Check(def != nullptr, std::string("label for a known template: ") + l.name);
        Check(*l.name && !std::strchr(l.name, '<') && l.name[std::strlen(l.name) - 1] != ' ', std::string("clean label: ") + l.name);
        if (def) Check(!CodeLike(l.name, def->code), std::string("label not from the code: ") + def->code + " -> " + l.name);
        bool known = false;
        for (size_t k = 0; k < 6; k++)
            if (std::strcmp(l.source, sources[k]) == 0) by_source[k]++, known = true;
        Check(known, std::string("known label source: ") + l.source);
    }
    for (size_t i = 0; i < kFactionLabelCount; i++)
        Check(*kFactionLabels[i].name && !std::strchr(kFactionLabels[i].name, '_'), std::string("faction word: ") + kFactionLabels[i].name);

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
    size_t catalogue = 0;
    for (size_t i = 0; i < kEntityCount; i++) catalogue += IsCatalogueEntity(kEntities[i].code) ? 1 : 0;
    for (size_t i = 0; i < rows.size(); i++) {
        const std::string& n = names[i];
        const std::string code = rows[i]->code;
        Check(!n.empty() && !Lowercase(n) && !CodeLike(n, code), "catalogue name " + code + " -> \"" + n + "\"");
        const EntityLabel* l = EntityLabelByGuid(rows[i]->guid);
        Check(l != nullptr && n == l->name, "catalogue row has a client label: " + code);
        // listEntities and entity-killed name a template the same way
        Check(n == ResolveEntityName(rows[i]).name, "catalogue name = kill name for " + code);
    }
    // Every template, listed or not, gets a name in a kill that is not built from its code.
    for (size_t i = 0; i < kEntityCount; i++) {
        EntityName n = ResolveEntityName(&kEntities[i]);
        Check(!n.name.empty() && !CodeLike(n.name, kEntities[i].code), std::string("kill name for ") + kEntities[i].code + " -> " + n.name);
    }

    // Items: only what the client names, never an ability, never markup.
    size_t items = 0, unnamed = 0, abilities = 0, swatches = 0;
    for (size_t i = 0; i < kItemCount; i++) {
        const char* label = ItemLabelById(kItems[i].itemId);
        if (!label) unnamed++;
        if (label && std::strncmp(kItems[i].code, "Ability_", 8) == 0) abilities++;
        if (label && std::strncmp(kItems[i].code, "Icon_", 5) == 0) swatches++;
        if (!IsCatalogueItem(kItems[i].code, label)) continue;
        items++;
        Check(!std::strchr(label, '<') && std::strcmp(label, kItems[i].code) != 0,
              std::string("item name ") + kItems[i].code + " -> " + label);
    }
    Check(!IsCatalogueItem("Ability_Evade", "Dodge Roll"), "an ability is not an item");
    Check(!IsCatalogueItem("VC_Beard10_balbo", nullptr), "an unnamed item is not listed");
    Check(IsCatalogueItem("Weapon_T3_2H_Bow_Rattlesnake", "Rattlesnake Bow"), "a named weapon is listed");
    Check(!IsCatalogueItem("Icon_EyeColor_red", "Red Eyes"), "a character-creator eye colour is not an item");
    Check(!IsCatalogueItem("Icon_HairColor_red", "Copper Hair"), "a character-creator hair colour is not an item");
    Check(swatches > 0, "the item table has character-creator swatches to leave out");
    for (size_t i = 0; i < kItemLabelCount; i++)
        Check(!std::strchr(kItemLabels[i].name, '<'), std::string("item label without markup: ") + kItemLabels[i].name);

    std::printf("entity labels: %zu of %zu templates (client %zu, logicSpawn %zu, item %zu, model %zu, sibling %zu, arsenal %zu)\n",
                kEntityLabelCount, kEntityCount, by_source[0], by_source[1], by_source[2], by_source[3], by_source[4], by_source[5]);
    std::printf("listEntities: %zu of %zu catalogue templates (%zu without an in-game name left out)\n", rows.size(), catalogue,
                catalogue - rows.size());
    std::printf("listItems: %zu of %zu items (%zu unnamed, %zu named abilities and %zu swatches left out)\n", items, kItemCount,
                unnamed, abilities, swatches);
    std::printf("entity_names_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
