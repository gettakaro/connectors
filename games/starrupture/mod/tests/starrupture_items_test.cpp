#include "starrupture_items.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    Expect(HumanizeTakaroCode("WeaponSwapSpeedGemT3") == "Weapon Swap Speed Gem T3", "tier mark stays joined");
    Expect(HumanizeTakaroCode("Melee_Tier1_Reskin_A") == "Melee Tier 1 Reskin A", "separators become spaces");
    Expect(HumanizeTakaroCode("Helium3") == "Helium 3", "a number after a word is split");
    Expect(HumanizeTakaroCode("RangedTier2ReskinA") == "Ranged Tier 2 Reskin A", "a word after a number is split");

    try
    {
        const std::vector<TakaroRawItemDefinition> definitions = {
            {"iron-plate", "Iron Plate", "A plate", true, 100},
            {"internal", "Internal", "", false, 1},
            {"", "Missing code", "", true, 1},
            {"empty-name", "", "", true, 1},
        };
        const auto catalog = NormalizeTakaroItemCatalog(definitions);
        Expect(catalog.ok, "valid catalog normalization failed");
        Expect(catalog.items.size() == 1, "catalog retained internal or malformed items");
        Expect(catalog.items[0].code == "iron-plate" && catalog.items[0].name == "Iron Plate",
            "catalog changed the exact item identity");
        Expect(catalog.items[0].description == std::optional<std::string>{"A plate"},
            "catalog lost the item description");

        const auto duplicate = NormalizeTakaroItemCatalog({
            {"same", "One", "", true, 1}, {"same", "Two", "", true, 1},
        });
        Expect(!duplicate.ok && duplicate.error.find("duplicate") != std::string::npos,
            "ambiguous duplicate catalog code was accepted");

        const std::vector<TakaroRawItemDefinition> sharedCodeDefinitions = {
            {"shared", "Internal", "", false, 1},
            {" shared ", "Grantable", "", true, 1},
        };
        const auto sharedCodeCatalog = NormalizeTakaroItemCatalog(sharedCodeDefinitions);
        const auto sharedCodeSources = ResolveTakaroItemCatalogSources(sharedCodeDefinitions, sharedCodeCatalog.items);
        Expect(sharedCodeCatalog.ok && sharedCodeSources.ok,
            "internal item sharing a code blocked the grantable catalog item");
        Expect(sharedCodeSources.sourceIndexes.at("shared") == 1,
            "catalog code resolved to the internal native source");

        const auto inventory = NormalizeTakaroInventory({
            {"iron-plate", 2, true}, {"iron-plate", 3, true},
            {"iron-plate", 10, false}, {"unknown", 4, true}, {"iron-plate", 0, true},
        }, catalog.items);
        Expect(inventory.ok, "valid inventory normalization failed");
        Expect(inventory.items.size() == 1 && inventory.items[0].amount == std::optional<double>{5},
            "inventory did not aggregate occupied exact-code quantities");

        const auto badInventory = NormalizeTakaroInventory({
            {"iron-plate", std::numeric_limits<int>::max(), true}, {"iron-plate", 1, true},
        }, catalog.items);
        Expect(!badInventory.ok, "overflowing inventory quantity was accepted");

        Expect(ValidateTakaroItemGrant("iron-plate", 2, catalog.items).ok,
            "valid exact item grant was rejected");
        Expect(!ValidateTakaroItemGrant("iron", 2, catalog.items).ok,
            "partial item code was accepted");
        Expect(!ValidateTakaroItemGrant("iron-plate", 0, catalog.items).ok,
            "zero item amount was accepted");
        Expect(!ValidateTakaroItemGrantPostcondition(std::nullopt, std::nullopt, 2).ok,
            "absent player pawn/inventory was accepted as a grant target");
        Expect(!ValidateTakaroItemGrantPostcondition(5, 5, 2).ok,
            "full inventory with no authoritative count change reported success");
        Expect(ValidateTakaroItemGrantPostcondition(5, 7, 2).ok,
            "exact authoritative inventory delta was rejected");

        std::cout << "starrupture_items_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_items_test: " << error.what() << "\n";
        return 1;
    }
}
