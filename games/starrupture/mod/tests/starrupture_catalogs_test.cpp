#include "starrupture_catalogs.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    try
    {
        const auto entities = NormalizeTakaroEntityCatalog({
            {"Mite", "/Game/Enemies/DA_Mite.DA_Mite", "Burrowing Mite", true},
            {"Mite", "/Game/Enemies/DA_Mite.DA_Mite", "Burrowing Mite", true},
            {"Crawler", "/Game/Enemies/DA_Crawler.DA_Crawler", "", true},
            {"", "/Game/Enemies/DA_Fallback.DA_Fallback", "", true},
            {"Unloaded", "/Game/Enemies/DA_Unloaded.DA_Unloaded", "Hidden", false},
        });
        Expect(entities.ok, "valid enemy definitions rejected");
        Expect(entities.entities.size() == 3, "enemy catalog was not filtered and deduplicated");
        Expect(entities.entities[0].code == "/Game/Enemies/DA_Fallback.DA_Fallback",
            "stable asset path fallback was transformed");
        Expect(entities.entities[0].name == "DA Fallback", "asset-path display fallback was not readable");
        Expect(entities.entities[1].code == "Crawler" && entities.entities[1].name == "Crawler",
            "unique enemy name fallback was not preserved");
        Expect(entities.entities[2].code == "Mite" && entities.entities[2].name == "Burrowing Mite",
            "localized enemy name was not preferred");

        TakaroRawPointOfInterest discovered;
        discovered.guid = {0x12345678U, 0x90abcdefU, 0x01020304U, 0xa0b0c0d0U};
        discovered.state = TakaroPointOfInterestState::VisibleDiscovered;
        discovered.type = "cave";
        discovered.unknownName = "Unknown cave";
        discovered.discoveredName = "Echo Cavern";
        discovered.stableActorName = "BP_POI_Cave_C_12";
        discovered.position = {101.25, -202.5, 303.75, std::nullopt};
        discovered.live = true;

        auto duplicate = discovered;
        duplicate.state = TakaroPointOfInterestState::VisibleUnknown;
        duplicate.discoveredName.clear();
        duplicate.position.x = 999.0;

        auto fallback = discovered;
        fallback.guid = {1U, 2U, 3U, 4U};
        fallback.state = TakaroPointOfInterestState::VisibleUnknown;
        fallback.unknownName.clear();
        fallback.discoveredName.clear();
        fallback.stableActorName = "Orbital Lander";

        auto unloaded = fallback;
        unloaded.guid = {5U, 6U, 7U, 8U};
        unloaded.live = false;

        const auto locations = NormalizeTakaroLocationCatalog({duplicate, unloaded, fallback, discovered});
        Expect(locations.ok, "valid POIs rejected");
        Expect(locations.locations.size() == 2, "POI catalog was not filtered and deduplicated");
        Expect(locations.locations[0].code == "poi:00000001-00000002-00000003-00000004",
            "POI GUID was not normalized into a stable code");
        Expect(locations.locations[0].name == "Orbital Lander", "POI localization fallback failed");
        Expect(locations.locations[1].code == "poi:12345678-90abcdef-01020304-a0b0c0d0",
            "POI GUID code was not stable lowercase hexadecimal");
        Expect(locations.locations[1].name == "Echo Cavern", "discovered POI name was not selected");
        Expect(locations.locations[1].position.x == 101.25, "less-authoritative duplicate POI won");
        Expect(locations.locations[1].metadataJson ==
            "{\"state\":\"visible-discovered\",\"type\":\"cave\"}",
            "POI state/type metadata was not normalized");
        for (const auto& location : locations.locations)
            Expect(location.code.find("0x") == std::string::npos, "pointer-derived POI code exposed");

        auto zeroGuid = discovered;
        zeroGuid.guid = {};
        const auto invalid = NormalizeTakaroLocationCatalog({zeroGuid});
        Expect(!invalid.ok && invalid.locations.empty(), "zero POI GUID was accepted as stable identity");

        std::cout << "starrupture_catalogs_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_catalogs_test: " << error.what() << "\n";
        return 1;
    }
}
