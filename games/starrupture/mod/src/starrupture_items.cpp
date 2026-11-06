#include "starrupture_items.h"
#include "starrupture_player_pawn.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "plugin_helpers.h"
#include "SDK.hpp"
#endif

namespace
{
std::string Trim(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
std::string TextOf(const SDK::FText& text)
{
    // Game text lives in string tables, so the raw FText source is empty on the server.
    return text.TextData ? Trim(SDK::UKismetTextLibrary::Conv_TextToString(text).ToString()) : std::string{};
}

struct SdkCatalog
{
    TakaroItemNormalizationResult normalized;
    std::map<std::string, SDK::UAuItemDataBase*> nativeByCode;
};

SdkCatalog ReadSdkCatalog()
{
    SdkCatalog result;
    TAKARO_LOG_INFO("Item catalog extraction begin.");
    auto* world = SDK::UWorld::GetWorld();
    if (!world)
    {
        result.normalized.error = "StarRupture world is unavailable";
        return result;
    }
    TAKARO_LOG_INFO("Item catalog world resolved.");
    auto* baseSubsystem = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
        world, SDK::UAuItemDataBaseSubsystem::StaticClass());
    if (!baseSubsystem || !baseSubsystem->IsA(SDK::UAuItemDataBaseSubsystem::StaticClass()))
    {
        result.normalized.error = "StarRupture item database subsystem is unavailable";
        return result;
    }
    auto* subsystem = static_cast<SDK::UAuItemDataBaseSubsystem*>(baseSubsystem);
    const auto itemCount = subsystem->ItemDataBaseArray.Num();
    if (itemCount < 0 || itemCount > 10000)
    {
        result.normalized.error = "StarRupture item database has an invalid bounded size";
        return result;
    }
    TAKARO_LOG_INFO("Item catalog subsystem resolved; bounded source count=%d.", itemCount);
    std::vector<TakaroRawItemDefinition> definitions;
    std::vector<SDK::UAuItemDataBase*> nativeItems;
    definitions.reserve(static_cast<size_t>(itemCount));
    nativeItems.reserve(static_cast<size_t>(itemCount));
    for (int index = 0; index < itemCount; ++index)
    {
        auto* item = subsystem->ItemDataBaseArray[index];
        if (!item) continue;
        const std::string code = item->UniqueItemName.ToString();
        auto name = TextOf(item->ItemName);
        if (name.empty()) name = HumanizeTakaroCode(code);
        definitions.push_back({
            code,
            name,
            TextOf(item->ItemDescription),
            item->bIsInventoryItem,
            item->MaxStack,
        });
        nativeItems.push_back(item);
    }
    TAKARO_LOG_INFO("Item catalog native extraction complete.");
    result.normalized = NormalizeTakaroItemCatalog(definitions);
    TAKARO_LOG_INFO("Item catalog normalization complete.");
    if (!result.normalized.ok) return result;
    const auto sources = ResolveTakaroItemCatalogSources(definitions, result.normalized.items);
    if (!sources.ok)
    {
        result.normalized.ok = false;
        result.normalized.error = sources.error;
        return result;
    }
    for (const auto& [code, index] : sources.sourceIndexes)
    {
        if (index >= nativeItems.size() || !nativeItems[index])
        {
            result.normalized.ok = false;
            result.normalized.error = "catalog source index does not resolve to a native item";
            result.nativeByCode.clear();
            return result;
        }
        result.nativeByCode.emplace(code, nativeItems[index]);
    }
    return result;
}

SDK::ACrCharacterPlayerBase* FindSdkPlayerCharacter(const std::string& gameId)
{
    auto* world = SDK::UWorld::GetWorld();
    auto* gameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!gameState) return nullptr;
    for (auto* basePlayerState : gameState->PlayerArray)
    {
        if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) continue;
        auto* playerState = static_cast<SDK::ACrPlayerStateBase*>(basePlayerState);
        if (playerState->GetUniqueNetIdAsString().ToString() != gameId) continue;
        auto* pawn = ResolveTakaroPlayerPawn(playerState);
        if (!pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass())) return nullptr;
        return static_cast<SDK::ACrCharacterPlayerBase*>(pawn);
    }
    return nullptr;
}

TakaroItemNormalizationResult ReadSdkInventory(
    SDK::ACrCharacterPlayerBase* character,
    const SdkCatalog& catalog)
{
    TakaroItemNormalizationResult failure;
    auto* inventory = character ? character->InventoryComponent : nullptr;
    if (!inventory) inventory = character ? character->BP_GetInventory() : nullptr;
    if (!inventory)
    {
        failure.error = "connected player has no available inventory";
        return failure;
    }
    // Update 2 removed the resolved item definition from FAuItemEntry, so the
    // authoritative read asks the inventory for the amount of every exact
    // catalog item through the same accessor the grant postcondition uses.
    std::vector<TakaroRawInventoryEntry> entries;
    entries.reserve(catalog.nativeByCode.size());
    for (const auto& [code, native] : catalog.nativeByCode)
    {
        if (!native) continue;
        const int amount = inventory->BP_GetItemAmount(native);
        if (amount <= 0) continue;
        entries.push_back({code, amount, true});
    }
    return NormalizeTakaroInventory(entries, catalog.normalized.items);
}

TakaroOperationFailure Failure(const TakaroOperationRequest& request, const std::string& reason)
{
    return TakaroOperationFailure{request.action + ": " + reason};
}
#endif
}

std::string HumanizeTakaroCode(const std::string& code)
{
    std::string result;
    for (size_t index = 0; index < code.size(); ++index)
    {
        const auto ch = static_cast<unsigned char>(code[index]);
        if (ch == '_' || ch == '-' || ch == ' ')
        {
            if (!result.empty() && result.back() != ' ') result += ' ';
            continue;
        }
        const auto previous = result.empty() ? 0 : static_cast<unsigned char>(result.back());
        const bool afterLower = std::islower(previous) != 0;
        const bool wordStart = std::isupper(ch) && (afterLower || std::isdigit(previous) != 0);
        // A lone capital before a digit is a tier mark and stays joined: "GemT3".
        const bool numberStart = std::isdigit(ch) && afterLower;
        if (wordStart || numberStart) result += ' ';
        result += static_cast<char>(ch);
    }
    return Trim(result);
}

TakaroItemNormalizationResult NormalizeTakaroItemCatalog(
    const std::vector<TakaroRawItemDefinition>& definitions)
{
    TakaroItemNormalizationResult result;
    std::set<std::string> codes;
    for (const auto& definition : definitions)
    {
        if (!definition.inventoryItem || definition.maxStack <= 0) continue;
        const auto code = Trim(definition.code);
        const auto name = Trim(definition.name);
        if (code.empty() || name.empty()) continue;
        if (!codes.insert(code).second)
        {
            result.error = "duplicate exact item code in authoritative catalog";
            result.items.clear();
            return result;
        }
        TakaroItemDto item;
        item.code = code;
        item.name = name;
        const auto description = Trim(definition.description);
        if (!description.empty()) item.description = description;
        result.items.push_back(std::move(item));
    }
    std::sort(result.items.begin(), result.items.end(), [](const auto& left, const auto& right) {
        return left.code < right.code;
    });
    result.ok = true;
    return result;
}

TakaroItemNormalizationResult NormalizeTakaroInventory(
    const std::vector<TakaroRawInventoryEntry>& entries,
    const std::vector<TakaroItemDto>& catalog)
{
    TakaroItemNormalizationResult result;
    std::map<std::string, TakaroItemDto> catalogByCode;
    for (const auto& item : catalog)
    {
        if (item.code.empty() || !catalogByCode.emplace(item.code, item).second)
        {
            result.error = "inventory catalog contains an invalid or duplicate code";
            return result;
        }
    }

    std::map<std::string, int> amounts;
    for (const auto& entry : entries)
    {
        if (!entry.occupied || entry.amount <= 0) continue;
        const auto item = catalogByCode.find(entry.code);
        if (item == catalogByCode.end()) continue;
        auto& amount = amounts[entry.code];
        if (amount > (std::numeric_limits<int>::max)() - entry.amount)
        {
            result.error = "inventory item quantity overflow";
            return result;
        }
        amount += entry.amount;
    }

    for (const auto& [code, amount] : amounts)
    {
        auto item = catalogByCode.at(code);
        item.amount = static_cast<double>(amount);
        result.items.push_back(std::move(item));
    }
    result.ok = true;
    return result;
}

TakaroItemGrantValidation ValidateTakaroItemGrant(
    const std::string& code,
    int amount,
    const std::vector<TakaroItemDto>& catalog)
{
    TakaroItemGrantValidation result;
    result.code = Trim(code);
    result.amount = amount;
    if (result.code.empty())
    {
        result.error = "item code is required";
        return result;
    }
    if (amount <= 0 || amount > 100000)
    {
        result.error = "item amount must be between 1 and 100000";
        return result;
    }
    const auto matches = std::count_if(catalog.begin(), catalog.end(), [&](const auto& item) {
        return item.code == result.code;
    });
    if (matches != 1)
    {
        result.error = matches == 0 ? "unknown exact item code" : "ambiguous exact item code";
        return result;
    }
    result.ok = true;
    return result;
}

TakaroItemCatalogSourceResolution ResolveTakaroItemCatalogSources(
    const std::vector<TakaroRawItemDefinition>& definitions,
    const std::vector<TakaroItemDto>& catalog)
{
    TakaroItemCatalogSourceResolution result;
    for (const auto& item : catalog)
    {
        size_t matchCount = 0;
        size_t matchIndex = 0;
        for (size_t index = 0; index < definitions.size(); ++index)
        {
            const auto& definition = definitions[index];
            if (!definition.inventoryItem || definition.maxStack <= 0 ||
                Trim(definition.code).empty() || Trim(definition.name).empty()) continue;
            if (Trim(definition.code) != item.code) continue;
            ++matchCount;
            matchIndex = index;
        }
        if (matchCount != 1 || !result.sourceIndexes.emplace(item.code, matchIndex).second)
        {
            result.error = "catalog item does not resolve to exactly one grantable native source";
            result.sourceIndexes.clear();
            return result;
        }
    }
    if (result.sourceIndexes.size() != catalog.size())
    {
        result.error = "catalog/native source cardinality mismatch";
        result.sourceIndexes.clear();
        return result;
    }
    result.ok = true;
    return result;
}

TakaroItemGrantPostcondition ValidateTakaroItemGrantPostcondition(
    const std::optional<int>& before,
    const std::optional<int>& after,
    int requestedAmount)
{
    TakaroItemGrantPostcondition result;
    if (!before || !after || *before < 0 || *after < 0)
    {
        result.error = "connected player pawn or inventory is unavailable";
        return result;
    }
    const auto delta = static_cast<int64_t>(*after) - static_cast<int64_t>(*before);
    if (requestedAmount <= 0 || delta != requestedAmount)
    {
        result.error = "item grant postcondition failed; inventory may be full";
        return result;
    }
    result.ok = true;
    return result;
}

TakaroOperationResult ExecuteTakaroItemOperation(const TakaroOperationRequest& request)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    const auto sdkCatalog = ReadSdkCatalog();
    if (!sdkCatalog.normalized.ok) return Failure(request, sdkCatalog.normalized.error);
    if (request.type == TakaroOperationType::ListItems)
        return TakaroOperationSuccess{request.type, sdkCatalog.normalized.items};

    std::optional<std::string> gameId;
    if (request.type == TakaroOperationType::GetPlayerInventory)
    {
        gameId = ExtractJsonString(request.argsJson, "gameId");
    }
    else if (request.type == TakaroOperationType::GiveItem)
    {
        const auto player = ExtractJsonObject(request.argsJson, "player");
        if (player) gameId = ExtractJsonString(*player, "gameId");
    }
    if (!gameId || gameId->empty()) return Failure(request, "an exact connected player gameId is required");
    auto* character = FindSdkPlayerCharacter(*gameId);
    if (!character)
    {
        if (request.type == TakaroOperationType::GiveItem)
            return Failure(request, ValidateTakaroItemGrantPostcondition(std::nullopt, std::nullopt, 1).error);
        return Failure(request, "connected player pawn is unavailable");
    }

    if (request.type == TakaroOperationType::GetPlayerInventory)
    {
        auto inventory = ReadSdkInventory(character, sdkCatalog);
        if (!inventory.ok) return Failure(request, inventory.error);
        return TakaroOperationSuccess{request.type, inventory.items};
    }
    if (request.type != TakaroOperationType::GiveItem)
        return Failure(request, "operation is not an item operation");

    const auto code = ExtractJsonString(request.argsJson, "item");
    const auto amount64 = ExtractJsonPositiveInteger(request.argsJson, "amount");
    if (!code || !amount64 || *amount64 > (std::numeric_limits<int>::max)())
        return Failure(request, "item and positive integer amount are required");
    // StarRupture items have no quality; Takaro's "quality" field is ignored.
    const auto grant = ValidateTakaroItemGrant(*code, static_cast<int>(*amount64), sdkCatalog.normalized.items);
    if (!grant.ok) return Failure(request, grant.error);
    const auto native = sdkCatalog.nativeByCode.find(grant.code);
    if (native == sdkCatalog.nativeByCode.end()) return Failure(request, "exact item code has no native catalog entry");
    auto* inventory = character->InventoryComponent ? character->InventoryComponent : character->BP_GetInventory();
    if (!inventory) return Failure(request, "connected player has no available inventory");
    const int before = inventory->BP_GetItemAmount(native->second);
    character->ServerAddItem(native->second, grant.amount);
    const int after = inventory->BP_GetItemAmount(native->second);
    const auto postcondition = ValidateTakaroItemGrantPostcondition(before, after, grant.amount);
    if (!postcondition.ok) return Failure(request, postcondition.error);
    return TakaroOperationSuccess{request.type, std::monostate{}};
#else
    return TakaroOperationFailure{request.action + " is unsupported without the pinned StarRupture Game SDK"};
#endif
}
