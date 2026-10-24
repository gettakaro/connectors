// The places listLocations answers with (location.cpp, locationlabels.cpp), over the whole shipped
// kLocations table: every row has a code no other row has (Takaro keys locations by code, so a
// shared code would merge markers), and the name the in-game map shows, never a code-derived one.
#include "gamedata.h"
#include "location.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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

}  // namespace

int main() {
    for (size_t i = 1; i < kLocationLabelCount; i++)
        Check(std::strcmp(kLocationLabels[i - 1].code, kLocationLabels[i].code) < 0,
              std::string("labels sorted and unique at ") + kLocationLabels[i].code);

    // Names as the 1.0 in-game map shows them at these places.
    Check(std::string(LocationLabelByCode("8kMapLabel_steppes_areaName_02_TwinPeaks")) == "Pillars of Creation",
          "TwinPeaks is Pillars of Creation");
    Check(std::string(LocationLabelByCode("8kMapLabel_grassland_town_10_SalineSprings")) == "Saline Springs",
          "Saline Springs");
    Check(std::string(LocationLabelByCode("8kMapLabel_AncientDungeon_02_Alchemist")) == "Ancient Vault \xe2\x80\x94 Alchemist",
          "markup stripped from Ancient Vault name");
    Check(LocationLabelByCode("Prop_OpenWorld_SavePoint") == nullptr, "save points are not map places");
    Check(LocationLabelByCode("8kMapLabel_general_PlaceholderMapMarker_empty") == nullptr, "placeholder dropped");
    Check(LocationLabelByCode(nullptr) == nullptr, "null code");

    std::vector<LocationRow> rows = LocationRows(kLocations, kLocationCount);
    std::set<std::string> codes;
    std::map<std::string, int> byBase;
    for (const LocationRow& r : rows) {
        Check(codes.insert(r.code).second, "unique code " + r.code);
        Check(r.name && *r.name, "name for " + r.code);
        std::string n = r.name ? r.name : "";
        Check(n.find('_') == std::string::npos && n.find('<') == std::string::npos, "readable name '" + n + "'");
        Check(n != r.def->code, "name is not the code: " + n);
        std::string lower = r.code;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        Check(lower.find("noui") == std::string::npos, "hidden marker left out: " + r.code);
        byBase[r.def->code]++;
    }
    // Every fast travel platform is its own marker.
    Check(byBase["Teleport_Platform_Gameplay"] == 52, "52 fast travel platforms on the surface");
    int fastTravel = 0;
    for (const LocationRow& r : rows)
        if (std::strcmp(r.def->code, "Teleport_Platform_Gameplay") == 0) {
            fastTravel++;
            Check(std::string(r.name) == "Fast Travel Point", "fast travel name");
            Check(r.code.rfind("Teleport_Platform_Gameplay@", 0) == 0, "fast travel code " + r.code);
        }
    Check(fastTravel == 52, "fast travel rows");
    // Nothing from the Hollow Halls interiors in the north-west corner.
    for (const LocationRow& r : rows)
        Check(!(r.def->x < 2400 && r.def->z > 8400), "no dungeon-interior placement: " + r.code);
    Check(byBase["Teleport_Platform_FallenCrypt_Gameplay"] == 4, "4 crypt fast travel platforms on the surface");

    // A code used once keeps its code.
    bool twin = false;
    for (const LocationRow& r : rows) twin |= r.code == "8kMapLabel_steppes_areaName_02_TwinPeaks";
    Check(twin, "single-use code kept as it is");

    // Two placements of one code in the same square metre still get two codes.
    LocationDef same[] = {{"Teleport_Platform_Gameplay", "mapMarker", "", 10.2, 0, 20.4},
                          {"Teleport_Platform_Gameplay", "mapMarker", "", 9.8, 0, 19.6},
                          {"Prop_OpenWorld_SavePoint", "spawnPoint", "SavePoint", 3000, 0, 3000},
                          {"Teleport_Platform_Gameplay", "mapMarker", "", 860, 200, 9934}};
    std::vector<LocationRow> two = LocationRows(same, 4);
    Check(two.size() == 2, "unnamed and interior placements dropped from a synthetic table");
    Check(two.size() == 2 && two[0].code == "Teleport_Platform_Gameplay@10_20" &&
              two[1].code == "Teleport_Platform_Gameplay@10_20_2",
          "same-metre placements numbered");

    std::printf("location_test: %zu of %zu placements listed, %d checks, %d failures\n", rows.size(), kLocationCount,
                checks, failures);
    return failures ? 1 : 0;
}
