#pragma once

#include "plugin_state.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

struct TakaroRawItemDefinition
{
    std::string code;
    std::string name;
    std::string description;
    bool inventoryItem = false;
    int maxStack = 0;
};

struct TakaroRawInventoryEntry
{
    std::string code;
    int amount = 0;
    bool occupied = false;
};

struct TakaroItemNormalizationResult
{
    bool ok = false;
    std::vector<TakaroItemDto> items;
    std::string error;
};

struct TakaroItemGrantValidation
{
    bool ok = false;
    std::string code;
    int amount = 0;
    std::string error;
};

struct TakaroItemCatalogSourceResolution
{
    bool ok = false;
    std::map<std::string, size_t> sourceIndexes;
    std::string error;
};

struct TakaroItemGrantPostcondition
{
    bool ok = false;
    std::string error;
};

// "WeaponSwapSpeedGemT3" -> "Weapon Swap Speed Gem T3", "Melee_Tier1" -> "Melee Tier 1".
std::string HumanizeTakaroCode(const std::string& code);

TakaroItemNormalizationResult NormalizeTakaroItemCatalog(
    const std::vector<TakaroRawItemDefinition>& definitions);
TakaroItemNormalizationResult NormalizeTakaroInventory(
    const std::vector<TakaroRawInventoryEntry>& entries,
    const std::vector<TakaroItemDto>& catalog);
TakaroItemGrantValidation ValidateTakaroItemGrant(
    const std::string& code,
    int amount,
    const std::vector<TakaroItemDto>& catalog);
TakaroItemCatalogSourceResolution ResolveTakaroItemCatalogSources(
    const std::vector<TakaroRawItemDefinition>& definitions,
    const std::vector<TakaroItemDto>& catalog);
TakaroItemGrantPostcondition ValidateTakaroItemGrantPostcondition(
    const std::optional<int>& before,
    const std::optional<int>& after,
    int requestedAmount);
TakaroOperationResult ExecuteTakaroItemOperation(const TakaroOperationRequest& request);
