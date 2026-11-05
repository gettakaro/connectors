#include "location.h"

#include "gamedata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

const char* LocationLabelByCode(const char* code) {
    if (!code) return nullptr;
    const LocationLabel* end = kLocationLabels + kLocationLabelCount;
    const LocationLabel* it = std::lower_bound(kLocationLabels, end, code, [](const LocationLabel& l, const char* c) {
        return std::strcmp(l.code, c) < 0;
    });
    return it != end && std::strcmp(it->code, code) == 0 ? it->name : nullptr;
}

// The Hollow Halls are dungeon interiors the world keeps in one block in its north-west corner, far
// below the surface (x 0..1800, z 8700..10240, y 100..560), where the world map shows nothing. Their
// fast travel platforms are no place on the map, so placements in that block are left out.
bool InInteriorBlock(const LocationDef& d) { return d.x < 2400 && d.z > 8400; }

std::vector<LocationRow> LocationRows(const LocationDef* defs, size_t count) {
    std::map<std::string, int> uses;
    for (size_t i = 0; i < count; i++)
        if (LocationLabelByCode(defs[i].code) && !InInteriorBlock(defs[i])) uses[defs[i].code]++;
    std::vector<LocationRow> rows;
    std::map<std::string, int> taken;
    for (size_t i = 0; i < count; i++) {
        const char* name = LocationLabelByCode(defs[i].code);
        if (!name || InInteriorBlock(defs[i])) continue;
        std::string code = defs[i].code;
        if (uses[code] > 1) {
            code += "@" + std::to_string(std::lround(defs[i].x)) + "_" + std::to_string(std::lround(defs[i].z));
            // Two placements of one code in the same square metre: number the later ones.
            if (int n = taken[code]++) code += "_" + std::to_string(n + 1);
        }
        rows.push_back({&defs[i], code, name});
    }
    return rows;
}
