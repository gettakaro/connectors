#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include "plugin_state.h"

struct TakaroRawSkill
{
    uint8_t skill = 0;
    int level = 0;
    float experience = 0;
};

struct TakaroRawPlayerSnapshot
{
    std::string gameId;
    std::string name;
    int ping = 0;
    bool hasLocation = false;
    double x = 0;
    double y = 0;
    double z = 0;
    std::optional<std::vector<TakaroRawSkill>> skills;
};

std::optional<TakaroPlayerSnapshot> BuildTakaroPlayerSnapshot(const TakaroRawPlayerSnapshot& raw);
std::optional<TakaroPlayerSnapshot> TakaroPlayerFromController(void* playerController);
std::optional<TakaroPlayerSnapshot> TakaroPlayerFromState(void* playerState);
// Observes the current world from the game thread: its object name and whether
// its game state reports begun play. Empty when no world exists yet.
std::optional<TakaroWorldObservation> ObserveTakaroWorld();
bool ReconcileTakaroPlayersFromWorld(
    std::vector<TakaroPlayerSnapshot>& players,
    std::map<void*, std::string>& controllerGameIds,
    std::map<std::string, bool>* playerDeadStates = nullptr,
    std::map<std::string, TakaroPositionDto>* playerLastDeathPositions = nullptr);
ITakaroGameOperations& TakaroGameOperations();
