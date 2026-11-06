#pragma once

#include "plugin_state.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct TakaroRawEntityDefinition
{
    std::string uniqueName;
    std::string stableObjectPath;
    std::string localizedName;
    bool loaded = false;
};

enum class TakaroPointOfInterestState
{
    Hidden,
    VisibleUnknown,
    VisibleDiscovered,
    Disabled,
};

struct TakaroRawPointOfInterest
{
    std::array<uint32_t, 4> guid{};
    TakaroPointOfInterestState state = TakaroPointOfInterestState::Hidden;
    std::string type;
    std::string unknownName;
    std::string discoveredName;
    std::string stableActorName;
    TakaroPositionDto position;
    bool live = false;
};

struct TakaroEntityCatalogResult
{
    bool ok = false;
    std::vector<TakaroEntityDto> entities;
    std::string error;
};

struct TakaroCatalogLocation
{
    TakaroPositionDto position;
    std::string name;
    std::string code;
    std::string metadataJson;
};

struct TakaroLocationCatalogResult
{
    bool ok = false;
    std::vector<TakaroCatalogLocation> locations;
    std::string error;
};

TakaroEntityCatalogResult NormalizeTakaroEntityCatalog(
    const std::vector<TakaroRawEntityDefinition>& definitions);
TakaroLocationCatalogResult NormalizeTakaroLocationCatalog(
    const std::vector<TakaroRawPointOfInterest>& pointsOfInterest);
TakaroOperationResult ExecuteTakaroCatalogOperation(const TakaroOperationRequest& request);
