#pragma once

#include <string>

// A StarRupture dedicated server parks on the DedicatedServerStart map until an
// owner starts the world from the client's Manage Server flow; only ChimeraMain
// is a joinable gameplay world. The lobby name comes from the pinned Update 2
// Game SDK (ADedicatedServerStart_C in DedicatedServerStart_classes.hpp). The
// gameplay name comes from the dedicated server's own map records
// (/Game/Chimera/Maps/ChimeraMain/ChimeraMain); the pinned dump carries no
// level script for it.
inline constexpr const char* TakaroLobbyWorldName = "DedicatedServerStart";
inline constexpr const char* TakaroGameplayWorldName = "ChimeraMain";

enum class TakaroWorldPhase
{
    Unknown,
    Lobby,
    Loading,
    Gameplay,
};

struct TakaroWorldObservation
{
    std::string worldName;
    bool hasGameState = false;
    bool gameStateBegunPlay = false;
};

inline TakaroWorldPhase ClassifyTakaroWorldPhase(const TakaroWorldObservation& world)
{
    if (world.worldName == TakaroLobbyWorldName) return TakaroWorldPhase::Lobby;
    if (world.worldName != TakaroGameplayWorldName) return TakaroWorldPhase::Unknown;
    return world.hasGameState && world.gameStateBegunPlay ? TakaroWorldPhase::Gameplay : TakaroWorldPhase::Loading;
}

inline const char* TakaroWorldPhaseName(TakaroWorldPhase phase)
{
    switch (phase)
    {
    case TakaroWorldPhase::Lobby: return "lobby";
    case TakaroWorldPhase::Loading: return "loading";
    case TakaroWorldPhase::Gameplay: return "gameplay";
    case TakaroWorldPhase::Unknown: break;
    }
    return "unknown";
}

// Operator-facing explanation for a plugin that is up but not ready.
// Returns nullptr once the gameplay world has begun play.
inline const char* TakaroWorldPhaseNotReadyReason(TakaroWorldPhase phase)
{
    switch (phase)
    {
    case TakaroWorldPhase::Lobby:
        return "StarRupture dedicated server is parked on DedicatedServerStart; start the world from the client's Manage Server flow.";
    case TakaroWorldPhase::Loading:
        return "ChimeraMain is loading and has not begun play.";
    case TakaroWorldPhase::Unknown:
        return "No StarRupture gameplay world has begun play.";
    case TakaroWorldPhase::Gameplay: break;
    }
    return nullptr;
}
