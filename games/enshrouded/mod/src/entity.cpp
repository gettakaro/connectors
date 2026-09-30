#include "entity.h"

#include "gamedata.h"
#include "names.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

const EntityLabel* EntityLabelByGuid(const uint8_t* guid) {
    if (!guid) return nullptr;
    const EntityLabel* end = kEntityLabels + kEntityLabelCount;
    const EntityLabel* it = std::lower_bound(kEntityLabels, end, guid, [](const EntityLabel& l, const uint8_t* g) {
        return std::memcmp(l.guid, g, 16) < 0;
    });
    return it != end && std::memcmp(it->guid, guid, 16) == 0 ? it : nullptr;
}

EntityName ResolveEntityName(const uint8_t* guid, const char* code, const std::string& raw) {
    if (const EntityLabel* l = EntityLabelByGuid(guid)) return {l->name, l->source};
    if (code && *code) {
        std::string derived = DisplayName(code, NameKind::Entity);
        if (!derived.empty()) return {derived, "code"};
        return {code, "raw"};
    }
    return {raw.empty() ? std::string("Unknown") : raw, "raw"};
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
    for (size_t i = 0; i < kEntityCount; i++) {
        if (!IsCatalogueEntity(kEntities[i].code)) continue;
        rows.push_back(&kEntities[i]);
        names.push_back(ResolveEntityName(kEntities[i].guid, kEntities[i].code).name);
    }
}
