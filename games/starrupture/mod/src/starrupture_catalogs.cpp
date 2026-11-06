#include "starrupture_catalogs.h"
#include "starrupture_items.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
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

std::string ObjectLeaf(const std::string& path)
{
    const auto slash = path.find_last_of("/\\");
    auto leaf = path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = leaf.find('.');
    if (dot != std::string::npos) leaf.erase(dot);
    return Trim(leaf);
}

std::string StateName(TakaroPointOfInterestState state)
{
    switch (state)
    {
    case TakaroPointOfInterestState::Hidden: return "hidden";
    case TakaroPointOfInterestState::VisibleUnknown: return "visible-unknown";
    case TakaroPointOfInterestState::VisibleDiscovered: return "visible-discovered";
    case TakaroPointOfInterestState::Disabled: return "disabled";
    }
    return "unknown";
}

int StateRank(TakaroPointOfInterestState state)
{
    switch (state)
    {
    case TakaroPointOfInterestState::VisibleDiscovered: return 4;
    case TakaroPointOfInterestState::VisibleUnknown: return 3;
    case TakaroPointOfInterestState::Hidden: return 2;
    case TakaroPointOfInterestState::Disabled: return 1;
    }
    return 0;
}

std::string Escape(std::string_view value)
{
    std::string escaped;
    for (const unsigned char ch : value)
    {
        if (ch == '"') escaped += "\\\"";
        else if (ch == '\\') escaped += "\\\\";
        else if (ch >= 0x20) escaped += static_cast<char>(ch);
    }
    return escaped;
}

bool ZeroGuid(const std::array<uint32_t, 4>& guid)
{
    return std::all_of(guid.begin(), guid.end(), [](uint32_t part) { return part == 0; });
}

std::string GuidCode(const std::array<uint32_t, 4>& guid)
{
    std::ostringstream code;
    code << "poi:" << std::hex << std::nouppercase << std::setfill('0');
    for (size_t index = 0; index < guid.size(); ++index)
    {
        if (index) code << '-';
        code << std::setw(8) << guid[index];
    }
    return code.str();
}

std::string PointOfInterestName(const TakaroRawPointOfInterest& point)
{
    const auto unknown = Trim(point.unknownName);
    const auto discovered = Trim(point.discoveredName);
    const auto actor = Trim(point.stableActorName);
    if (point.state == TakaroPointOfInterestState::VisibleDiscovered && !discovered.empty()) return discovered;
    if (!unknown.empty()) return unknown;
    if (!discovered.empty()) return discovered;
    return actor;
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
TakaroPointOfInterestState PointOfInterestState(SDK::EPointOfInterestState state)
{
    switch (state)
    {
    case SDK::EPointOfInterestState::VisibleUnknown: return TakaroPointOfInterestState::VisibleUnknown;
    case SDK::EPointOfInterestState::VisibleDiscovered: return TakaroPointOfInterestState::VisibleDiscovered;
    case SDK::EPointOfInterestState::Disabled: return TakaroPointOfInterestState::Disabled;
    case SDK::EPointOfInterestState::Hidden:
    default: return TakaroPointOfInterestState::Hidden;
    }
}

std::string PointOfInterestType(SDK::ECrPointOfInterestType type)
{
    switch (type)
    {
    case SDK::ECrPointOfInterestType::Antena: return "antenna";
    case SDK::ECrPointOfInterestType::AbandonedBase: return "abandoned-base";
    case SDK::ECrPointOfInterestType::Cave: return "cave";
    case SDK::ECrPointOfInterestType::Obelisk: return "obelisk";
    case SDK::ECrPointOfInterestType::OrbitalLander: return "orbital-lander";
    case SDK::ECrPointOfInterestType::ForgottenEngine: return "forgotten-engine";
    case SDK::ECrPointOfInterestType::Undefined:
    default: return "undefined";
    }
}
#endif
}

TakaroEntityCatalogResult NormalizeTakaroEntityCatalog(
    const std::vector<TakaroRawEntityDefinition>& definitions)
{
    struct Candidate { TakaroEntityDto entity; bool localized = false; };
    std::map<std::string, Candidate> byCode;
    for (const auto& definition : definitions)
    {
        if (!definition.loaded) continue;
        auto code = Trim(definition.uniqueName);
        const auto stablePath = Trim(definition.stableObjectPath);
        if (code.empty()) code = stablePath;
        if (code.empty())
            return {false, {}, "loaded enemy definition has no stable unique name or object path"};
        const auto localizedName = Trim(definition.localizedName);
        auto name = localizedName;
        if (name.empty()) name = HumanizeTakaroCode(!Trim(definition.uniqueName).empty() ? Trim(definition.uniqueName) : ObjectLeaf(stablePath));
        if (name.empty()) name = code;

        Candidate candidate{{code, name, std::nullopt, "hostile", std::nullopt}, !localizedName.empty()};
        const auto existing = byCode.find(code);
        if (existing == byCode.end() ||
            (candidate.localized && !existing->second.localized) ||
            (candidate.localized == existing->second.localized && candidate.entity.name < existing->second.entity.name))
            byCode[code] = std::move(candidate);
    }
    TakaroEntityCatalogResult result;
    for (auto& [code, candidate] : byCode)
    {
        (void)code;
        result.entities.push_back(std::move(candidate.entity));
    }
    result.ok = true;
    return result;
}

TakaroLocationCatalogResult NormalizeTakaroLocationCatalog(
    const std::vector<TakaroRawPointOfInterest>& pointsOfInterest)
{
    struct Candidate { TakaroCatalogLocation location; int stateRank = 0; };
    std::map<std::string, Candidate> byCode;
    for (const auto& point : pointsOfInterest)
    {
        if (!point.live) continue;
        if (ZeroGuid(point.guid)) return {false, {}, "live POI has no stable non-zero GUID"};
        if (!std::isfinite(point.position.x) || !std::isfinite(point.position.y) || !std::isfinite(point.position.z))
            return {false, {}, "live POI has invalid coordinates"};
        const auto code = GuidCode(point.guid);
        auto name = PointOfInterestName(point);
        if (name.empty()) name = code;
        const auto type = Trim(point.type).empty() ? "undefined" : Trim(point.type);
        TakaroCatalogLocation location{
            point.position,
            name,
            code,
            "{\"state\":\"" + StateName(point.state) + "\",\"type\":\"" + Escape(type) + "\"}",
        };
        Candidate candidate{std::move(location), StateRank(point.state)};
        const auto existing = byCode.find(code);
        if (existing == byCode.end() || candidate.stateRank > existing->second.stateRank ||
            (candidate.stateRank == existing->second.stateRank && candidate.location.name < existing->second.location.name))
            byCode[code] = std::move(candidate);
    }
    TakaroLocationCatalogResult result;
    for (auto& [code, candidate] : byCode)
    {
        (void)code;
        result.locations.push_back(std::move(candidate.location));
    }
    result.ok = true;
    return result;
}

TakaroOperationResult ExecuteTakaroCatalogOperation(const TakaroOperationRequest& request)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    if (request.type == TakaroOperationType::ListEntities)
    {
        std::vector<TakaroRawEntityDefinition> definitions;
        const int objectCount = SDK::UObject::GObjects->Num();
        if (objectCount < 0 || objectCount > 2000000)
            return TakaroOperationFailure{request.action + ": StarRupture object registry has an invalid bounded size"};
        for (int index = 0; index < objectCount; ++index)
        {
            auto* object = SDK::UObject::GObjects->GetByIndex(index);
            if (!object || object->IsDefaultObject() ||
                !object->IsA(SDK::UMassEnemyAiTypeDefinitionAsset::StaticClass())) continue;
            auto* definition = static_cast<SDK::UMassEnemyAiTypeDefinitionAsset*>(object);
            const auto uniqueName = definition->UniqueName.IsNone()
                ? std::string{} : definition->UniqueName.ToString();
            definitions.push_back({uniqueName, definition->GetFullName(), {}, true});
            if (definitions.size() > 10000)
                return TakaroOperationFailure{request.action + ": enemy definition catalog exceeded its bounded size"};
        }
        const auto catalog = NormalizeTakaroEntityCatalog(definitions);
        if (!catalog.ok) return TakaroOperationFailure{request.action + ": " + catalog.error};
        return TakaroOperationSuccess{request.type, catalog.entities};
    }
    if (request.type == TakaroOperationType::ListLocations)
    {
        auto* world = SDK::UWorld::GetWorld();
        if (!world) return TakaroOperationFailure{request.action + ": StarRupture world is unavailable"};
        SDK::TArray<SDK::AActor*> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(
            world, SDK::ACrPointOfInterestMarkerActor::StaticClass(), &actors);
        if (actors.Num() < 0 || actors.Num() > 10000)
            return TakaroOperationFailure{request.action + ": POI actor catalog has an invalid bounded size"};
        std::vector<TakaroRawPointOfInterest> points;
        points.reserve(static_cast<size_t>(actors.Num()));
        for (auto* actor : actors)
        {
            if (!actor || !actor->IsA(SDK::ACrPointOfInterestMarkerActor::StaticClass())) continue;
            auto* point = static_cast<SDK::ACrPointOfInterestMarkerActor*>(actor);
            const auto position = point->K2_GetActorLocation();
            TakaroRawPointOfInterest raw;
            raw.guid = {
                static_cast<uint32_t>(point->Guid.A), static_cast<uint32_t>(point->Guid.B),
                static_cast<uint32_t>(point->Guid.C), static_cast<uint32_t>(point->Guid.D),
            };
            raw.state = PointOfInterestState(point->GetPointOfInterestState());
            raw.type = PointOfInterestType(point->GetPointOfInterestType());
            // The pinned generated POI text fields produced an invalid FString array
            // assertion in the dedicated-server runtime. Keep the stable actor fallback
            // until a reflection-owned text read seam is available.
            raw.stableActorName = point->GetName();
            raw.position = {position.X, position.Y, position.Z, std::nullopt};
            raw.live = true;
            points.push_back(std::move(raw));
        }
        const auto catalog = NormalizeTakaroLocationCatalog(points);
        if (!catalog.ok) return TakaroOperationFailure{request.action + ": " + catalog.error};
        std::vector<TakaroLocationDto> locations;
        locations.reserve(catalog.locations.size());
        for (const auto& item : catalog.locations)
            locations.push_back({item.position, item.name, item.code, item.metadataJson});
        return TakaroOperationSuccess{request.type, locations};
    }
    return TakaroOperationFailure{request.action + " is not a catalog operation"};
#else
    return TakaroOperationFailure{request.action + " is unsupported without the pinned StarRupture Game SDK"};
#endif
}
