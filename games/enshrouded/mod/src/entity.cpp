#include "entity.h"

#include "gamedata.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <vector>

const EntityLabel* EntityLabelByGuid(const uint8_t* guid) {
    if (!guid) return nullptr;
    const EntityLabel* end = kEntityLabels + kEntityLabelCount;
    const EntityLabel* it = std::lower_bound(kEntityLabels, end, guid, [](const EntityLabel& l, const uint8_t* g) {
        return std::memcmp(l.guid, g, 16) < 0;
    });
    return it != end && std::memcmp(it->guid, guid, 16) == 0 ? it : nullptr;
}

EntityName ResolveEntityName(const EntityDef* def) {
    if (!def) return {"Unknown", "unknown"};
    if (const EntityLabel* l = EntityLabelByGuid(def->guid)) return {l->name, l->source};
    for (size_t i = 0; i < kFactionLabelCount; i++)
        if (def->faction && std::strcmp(def->faction, kFactionLabels[i].faction) == 0) return {kFactionLabels[i].name, "faction"};
    return {"Unknown", "unknown"};
}

namespace {

// Substrings of a lowercased code that mark a template as something other than a creature or
// an NPC: a part of one (collider, weak spot, armour plate, weapon), a trap, zone, projectile,
// prop or effect, a player-side template, or a dev-only template.
const char* const kNotACreature[] = {
    "collider",  "colldier",   "weakspot",    "protection", "attachment", "_weapon_", "modular",
    "trap",      "zone_",      "projectile",  "vfx",        "prop_",      "shroud_gem_forge",
    "treasurecontainer",       "fishingfloat", "home_base", "placement",  "movingplatform",
    "static_attack",           "utility",     "jumppad",
    "test",      "debug",      "deprecated",  "depricated", "donotuse",   "notfinished",
    "disfunctional",           "_wip",
};

// Prefixes of the same: abstract base templates, attack shapes and player-side templates.
const char* const kNotACreaturePrefix[] = {"base_", "attack_", "1_player", "player_", "automatedplayer",
                                           "fakeplayer_", "companion_test", "_deprecated", "z_"};

}  // namespace

bool IsCatalogueEntity(const char* code) {
    if (!code || !*code) return false;
    std::string l = code;
    for (char& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* p : kNotACreaturePrefix)
        if (l.compare(0, std::strlen(p), p) == 0) return false;
    for (const char* s : kNotACreature)
        if (l.find(s) != std::string::npos) return false;
    return true;
}

void CatalogueEntities(std::vector<const EntityDef*>& rows, std::vector<std::string>& names) {
    rows.clear();
    names.clear();
    std::vector<size_t> named;
    std::map<std::string, std::vector<size_t>> byCode;  // code -> its named catalogue templates
    for (size_t i = 0; i < kEntityCount; i++) {
        if (!IsCatalogueEntity(kEntities[i].code) || !EntityLabelByGuid(kEntities[i].guid)) continue;
        named.push_back(i);
        byCode[kEntities[i].code].push_back(i);
    }
    // Takaro keys entities by code, so a code the game gives several templates is listed once:
    // under the name the nearest shorter code names it (Enemy_Skeleton_Elite_Melee01_Dungeon_Hound
    // is the Hollow Reaper of Enemy_Skeleton_Elite_Melee01_Dungeon, not the Hollow Titan model
    // one of its two templates wears), else the first in table order. A kill still names each
    // template by its own label.
    auto keep = [&](const std::vector<size_t>& same) {
        std::string prefix = kEntities[same[0]].code;
        for (size_t cut; (cut = prefix.rfind('_')) != std::string::npos && cut > 0;) {
            prefix.resize(cut);
            auto it = byCode.find(prefix);
            if (it == byCode.end()) continue;
            const char* shorter = EntityLabelByGuid(kEntities[it->second[0]].guid)->name;
            for (size_t i : same)
                if (std::strcmp(EntityLabelByGuid(kEntities[i].guid)->name, shorter) == 0) return i;
        }
        return same[0];
    };
    for (size_t i : named) {
        const std::vector<size_t>& same = byCode[kEntities[i].code];
        if (same.size() > 1 && keep(same) != i) continue;
        rows.push_back(&kEntities[i]);
        names.push_back(ResolveEntityName(&kEntities[i]).name);
    }
}
