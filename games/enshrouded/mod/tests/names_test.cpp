// The names listItems, listEntities and listLocations answer with, over the whole shipped
// table.
//
// Three things are asserted. The vectors pin the derivation rules for one code each. The
// corpus pass asserts that no code in gamedata.cpp -- 3,609 items, 979 entity rows and
// 1,031 location rows -- derives a name that still carries a dev token, an underscore, a
// bare tier code or a leading enumerator, and that none of them is just its own code with
// the underscores opened. The distinctness pass asserts that DistinctNames, which is what
// world.cpp answers with, never hands two different codes the same name.
#include "gamedata.h"
#include "names.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(const char* code, NameKind kind, const char* want) {
    const std::string got = DisplayName(code, kind);
    if (got != want) {
        std::printf("  %-52s -> \"%s\" (wanted \"%s\")\n", code, got.c_str(), want);
        failures++;
    }
}

std::vector<std::string> Words(const std::string& name) {
    std::vector<std::string> words;
    std::string current;
    for (char c : name) {
        if (c == ' ') {
            if (!current.empty()) words.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) words.push_back(current);
    return words;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// Everything one derived name has to satisfy, or the reason it does not.
std::string Wrong(const char* code, const std::string& name) {
    if (name.empty()) return "empty";
    if (name.find('_') != std::string::npos) return "still carries an underscore";
    if (name[0] == ' ') return "starts with a space";
    const std::vector<std::string> words = Words(name);
    if (!words.empty() && AllDigits(words.front())) return "starts with an enumerator";
    for (const std::string& word : words) {
        // Every derived word is capitalised; a lowercase one is a piece of the code.
        if (std::islower(static_cast<unsigned char>(word[0]))) return "keeps the raw token \"" + word + "\"";
        const std::string lowered = Lower(word);
        if (lowered == "ag2" || lowered == "deprecated" || lowered == "depricated" || lowered == "placement" ||
            lowered == "noui" || lowered == "healthbar" || lowered == "unused" || lowered == "hasbugs" ||
            lowered == "test" || lowered == "lvlxx")
            return "keeps the dev token \"" + word + "\"";
        if (word.size() == 2 && (word[0] == 'T' || word[0] == 't') && word[1] >= '0' && word[1] <= '9')
            return "keeps the bare tier token \"" + word + "\"";
    }
    std::string opened;
    for (const char* c = code; *c; c++) opened += (*c == '_') ? ' ' : *c;
    if (name == opened) return "is the code with its underscores opened";
    return "";
}

int Corpus(const char* what, NameKind kind, const char* const* codes, size_t count) {
    int wrong = 0;
    for (size_t i = 0; i < count; i++) {
        const std::string reason = Wrong(codes[i], DisplayName(codes[i], kind));
        if (reason.empty()) continue;
        if (wrong < 10) std::printf("  %-64s %s\n", codes[i], reason.c_str());
        wrong++;
    }
    std::printf("%s: %zu codes, %d offenders\n", what, count, wrong);
    return wrong;
}

// What the catalogue actually answers with: one name per code, no two different codes
// sharing one, and every one of them still satisfying the rules above.
int Distinct(const char* what, NameKind kind, const char* const* codes, size_t count) {
    std::vector<std::string> names;
    DistinctNames(kind, codes, count, names);
    if (names.size() != count) {
        std::printf("%s: DistinctNames returned %zu names for %zu codes\n", what, names.size(), count);
        return 1;
    }
    std::map<std::string, std::vector<std::string>> by_name;
    for (size_t i = 0; i < count; i++) {
        std::vector<std::string>& codes_for = by_name[names[i]];
        bool known = false;
        for (const std::string& seen : codes_for)
            if (seen == codes[i]) known = true;
        if (!known) codes_for.push_back(codes[i]);
    }
    int shared = 0;
    for (const auto& entry : by_name) {
        if (entry.second.size() < 2) continue;
        if (shared < 10) {
            std::string list;
            for (size_t i = 0; i < entry.second.size() && i < 3; i++) {
                if (!list.empty()) list += ", ";
                list += entry.second[i];
            }
            std::printf("  %-44s <- %s\n", entry.first.c_str(), list.c_str());
        }
        shared++;
    }
    int wrong = 0;
    for (size_t i = 0; i < count; i++) {
        const std::string reason = Wrong(codes[i], names[i]);
        if (reason.empty()) continue;
        if (wrong < 10) std::printf("  %-64s %s\n", codes[i], reason.c_str());
        wrong++;
    }
    std::printf("%s: %zu codes, %d shared, %d offenders after distinctness\n", what, count, shared, wrong);
    return shared + wrong;
}

}  // namespace

int main() {
    // The rules, in the order names.cpp applies them.
    Expect("1_Player_AG2", NameKind::Entity, "Player");
    Expect("Animal_Baby_T1_Goat", NameKind::Entity, "Baby Goat (Tier 1)");
    Expect("Enemy_Skeleton_Heavy", NameKind::Entity, "Skeleton Heavy");
    Expect("NPC_Workshop_cryptKeeper01", NameKind::Entity, "Workshop Crypt Keeper");
    Expect("Animal_Wildlife_Cat_01_black_AG2", NameKind::Entity, "Cat Black");
    // A creature the client never names: the kind of template (Enemy, Wildbeast) and how it
    // is hooked into the world (_hook, _hookPtE, _SummonStone) are not part of its name.
    Expect("Enemy_Wildbeast_Rat_hook", NameKind::Entity, "Rat");
    Expect("Enemy_Wildbeast_Wolf_hook_AG2", NameKind::Entity, "Wolf");
    Expect("Enemy_Fog_Bug_Critter_hook", NameKind::Entity, "Fog Bug Critter");
    Expect("Enemy_Fogger_Heavy_hookPtE", NameKind::Entity, "Fogger Heavy");
    Expect("Enemy_Fogger_MageHeavy_bossHealtBar", NameKind::Entity, "Fogger Mage Heavy");
    Expect("Enemy_Wildbeast_Rat_Coldheights_Variation_hook", NameKind::Entity, "Rat Coldheights");
    Expect("AutomatedPlayer", NameKind::Entity, "Automated Player");
    Expect("8KMapLabel_deepforest_camp_01_HuntressCamp_1", NameKind::Location, "Huntress Camp 1");
    Expect("8kMapLabel_AncientDungeon_NightTemple_general", NameKind::Location,
           "Ancient Dungeon Night Temple General");
    Expect("8kMapLabel_deepforest_Town_07_Whitewind", NameKind::Location, "Whitewind");
    Expect("8kMapLabel_steppes_town_06_Brightwich", NameKind::Location, "Brightwich");
    Expect("Prop_OpenWorld_SavePoint", NameKind::Location, "Open World Save Point");
    Expect("Teleport_Platform_Gameplay", NameKind::Location, "Platform Gameplay");
    // Items: the category leads, the tier becomes a suffix, and the pipeline's parking
    // words go.
    Expect("Block_T3_Stone_CityWall_REWARD", NameKind::Item, "Stone City Wall Reward (Tier 3)");
    Expect("Armor_Tint_TEST", NameKind::Item, "Tint");
    Expect("_Ammo_T5_Arrow_Bone_UNUSED", NameKind::Item, "Arrow Bone (Tier 5)");
    Expect("Material_T7_Moonstone", NameKind::Item, "Moonstone (Tier 7)");
    Expect("Food_T7_raw_fruit_Artichoke", NameKind::Item, "Raw Fruit Artichoke (Tier 7)");
    Expect("Weapon_T4_2H_GreatSwordEpic_01", NameKind::Item, "2H Great Sword Epic (Tier 4)");
    Expect("Prop_Decoration_T5_Cupboard_Large", NameKind::Item, "Cupboard Large (Tier 5)");
    Expect("8kLoreText_scroll_bard_song_01", NameKind::Item, "Scroll Bard Song");
    Expect("Tool_Torch04_GleamRoot_UnderWater", NameKind::Item, "Torch Gleam Root Under Water");
    Expect("Z_Prop_NPC_Cat_Totem_DEPRECATED", NameKind::Item, "NPC Cat Totem");
    Expect("Weapon_T1-TX_1H_Mace_Enemy_01", NameKind::Item, "1H Mace Enemy");
    if (failures) std::printf("%d vector(s) wrong\n", failures);

    std::vector<const char*> item_codes;
    for (size_t i = 0; i < kItemCount; i++) item_codes.push_back(kItems[i].code);
    std::vector<const char*> entity_codes;
    for (size_t i = 0; i < kEntityCount; i++) entity_codes.push_back(kEntities[i].code);
    std::vector<const char*> location_codes;
    for (size_t i = 0; i < kLocationCount; i++) location_codes.push_back(kLocations[i].code);

    const int bad = Corpus("items", NameKind::Item, item_codes.data(), item_codes.size()) +
                    Corpus("entities", NameKind::Entity, entity_codes.data(), entity_codes.size()) +
                    Corpus("locations", NameKind::Location, location_codes.data(), location_codes.size()) +
                    Distinct("items", NameKind::Item, item_codes.data(), item_codes.size()) +
                    Distinct("entities", NameKind::Entity, entity_codes.data(), entity_codes.size()) +
                    Distinct("locations", NameKind::Location, location_codes.data(), location_codes.size());

    if (failures || bad) {
        std::printf("names_test FAILED\n");
        return 1;
    }
    std::printf("names_test ok\n");
    return 0;
}
