#include "starrupture_sdk_bridge.h"
#include "starrupture_catalogs.h"
#include "starrupture_admin.h"
#include "starrupture_items.h"
#include "starrupture_moderation.h"
#include "starrupture_player_pawn.h"
#include "starrupture_player_actions.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#endif

namespace
{
class PinnedSdkOperations final : public ITakaroGameOperations
{
public:
    TakaroOperationResult Execute(const TakaroOperationRequest& request) override
    {
        if (request.type == TakaroOperationType::GetPlayerInventory ||
            request.type == TakaroOperationType::GiveItem ||
            request.type == TakaroOperationType::ListItems)
            return ExecuteTakaroItemOperation(request);
        if (request.type == TakaroOperationType::ListEntities ||
            request.type == TakaroOperationType::ListLocations)
            return ExecuteTakaroCatalogOperation(request);
        if (request.type == TakaroOperationType::SendMessage ||
            request.type == TakaroOperationType::TeleportPlayer ||
            request.type == TakaroOperationType::KickPlayer)
            return ExecuteTakaroPlayerAction(request);
        if (request.type == TakaroOperationType::BanPlayer ||
            request.type == TakaroOperationType::UnbanPlayer ||
            request.type == TakaroOperationType::ListBans)
            return ExecuteTakaroModerationOperation(request);
        if (request.type == TakaroOperationType::Shutdown)
            return ExecuteTakaroAdminOperation(request);
        return TakaroOperationFailure{request.action + " is unsupported until its server-owned postcondition passes live proof"};
    }
};

PinnedSdkOperations g_operations;

bool IsSteamId64(const std::string& gameId)
{
    return gameId.size() == 17 && gameId.rfind("7656", 0) == 0 &&
        std::all_of(gameId.begin(), gameId.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string StablePlatformId(const std::string& gameId)
{
    if (IsSteamId64(gameId)) return "steam:" + gameId;
    std::ostringstream encoded;
    encoded << "starrupture:" << std::hex << std::setfill('0');
    for (const unsigned char ch : gameId) encoded << std::setw(2) << static_cast<unsigned int>(ch);
    return encoded.str();
}

// ECrPlayerProgressionSkill. None and anything newer are not skills a player has.
const char* SkillName(uint8_t skill)
{
    switch (skill)
    {
    case 0: return "movement";
    case 1: return "combat";
    case 2: return "survival";
    default: return nullptr;
    }
}

std::optional<std::vector<TakaroSkillReading>> BuildSkillReadings(
    const std::optional<std::vector<TakaroRawSkill>>& raw)
{
    if (!raw) return std::nullopt;
    std::vector<TakaroSkillReading> readings;
    for (const auto& entry : *raw)
    {
        const auto* name = SkillName(entry.skill);
        if (!name || entry.level < 0 || !std::isfinite(entry.experience) || entry.experience < 0) continue;
        readings.push_back(TakaroSkillReading{name, entry.level, entry.experience});
    }
    return readings;
}

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
// PlayerSkills is only meaningful once the server has restored the character.
// Reading it earlier would report a returning player at a default level.
std::optional<std::vector<TakaroRawSkill>> ReadRawSkills(SDK::ACrPlayerStateBase* playerState, SDK::APawn* pawn)
{
    if (!pawn || !pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass())) return std::nullopt;
    if (!static_cast<SDK::ACrCharacterPlayerBase*>(pawn)->bCharacterSelectedAndInitialized) return std::nullopt;
    auto* controller = playerState->GetPlayerController();
    if (!controller || !controller->IsA(SDK::ACrPlayerControllerBase::StaticClass())) return std::nullopt;
    auto& skills = static_cast<SDK::ACrPlayerControllerBase*>(controller)->PlayerSkills;
    if (skills.Num() == 0) return std::nullopt;
    std::vector<TakaroRawSkill> raw;
    raw.reserve(skills.Num());
    for (const auto& skill : skills)
        raw.push_back(TakaroRawSkill{static_cast<uint8_t>(skill.Skill), skill.Level, skill.Experience});
    return raw;
}

std::optional<TakaroPlayerSnapshot> FromPlayerState(SDK::ACrPlayerStateBase* playerState)
{
    if (!playerState) return std::nullopt;
    TakaroRawPlayerSnapshot raw;
    raw.gameId = playerState->GetUniqueNetIdAsString().ToString();
    raw.name = playerState->GetPlayerName().ToString();
    raw.ping = static_cast<int>(playerState->GetPingInMilliseconds());
    if (auto* pawn = ResolveTakaroPlayerPawn(playerState))
    {
        const auto location = pawn->K2_GetActorLocation();
        raw.hasLocation = true;
        raw.x = location.X;
        raw.y = location.Y;
        raw.z = location.Z;
        raw.skills = ReadRawSkills(playerState, pawn);
    }
    return BuildTakaroPlayerSnapshot(raw);
}
#endif
}

std::optional<TakaroPlayerSnapshot> BuildTakaroPlayerSnapshot(const TakaroRawPlayerSnapshot& raw)
{
    if (raw.gameId.empty() || raw.name.empty()) return std::nullopt;
    TakaroPlayerSnapshot player;
    player.gameId = raw.gameId;
    player.name = raw.name;
    player.platformId = StablePlatformId(raw.gameId);
    if (IsSteamId64(raw.gameId)) player.steamId = raw.gameId;
    player.ping = raw.ping;
    player.hasLocation = raw.hasLocation;
    player.x = raw.x;
    player.y = raw.y;
    player.z = raw.z;
    player.skills = BuildSkillReadings(raw.skills);
    return player;
}

std::optional<TakaroPlayerSnapshot> TakaroPlayerFromController(void* playerController)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto* controller = static_cast<SDK::APlayerController*>(playerController);
    auto* basePlayerState = controller ? controller->PlayerState : nullptr;
    if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) return std::nullopt;
    return FromPlayerState(static_cast<SDK::ACrPlayerStateBase*>(basePlayerState));
#else
    (void)playerController;
    return std::nullopt;
#endif
}

std::optional<TakaroPlayerSnapshot> TakaroPlayerFromState(void* playerState)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto* object = static_cast<SDK::UObject*>(playerState);
    if (!object || !object->IsA(SDK::ACrPlayerStateBase::StaticClass())) return std::nullopt;
    return FromPlayerState(static_cast<SDK::ACrPlayerStateBase*>(object));
#else
    (void)playerState;
    return std::nullopt;
#endif
}

std::optional<TakaroWorldObservation> ObserveTakaroWorld()
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto* world = SDK::UWorld::GetWorld();
    if (!world) return std::nullopt;
    TakaroWorldObservation observation;
    observation.worldName = world->GetName();
    auto* gameState = SDK::UGameplayStatics::GetGameState(world);
    observation.hasGameState = gameState != nullptr;
    observation.gameStateBegunPlay = gameState && gameState->bReplicatedHasBegunPlay;
    return observation;
#else
    return std::nullopt;
#endif
}

bool ReconcileTakaroPlayersFromWorld(
    std::vector<TakaroPlayerSnapshot>& players,
    std::map<void*, std::string>& controllerGameIds,
    std::map<std::string, bool>* playerDeadStates,
    std::map<std::string, TakaroPositionDto>* playerLastDeathPositions)
{
    players.clear();
    controllerGameIds.clear();
    if (playerDeadStates) playerDeadStates->clear();
    if (playerLastDeathPositions) playerLastDeathPositions->clear();
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto* world = SDK::UWorld::GetWorld();
    auto* gameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!gameState) return false;
    for (auto* basePlayerState : gameState->PlayerArray)
    {
        if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) continue;
        auto player = FromPlayerState(static_cast<SDK::ACrPlayerStateBase*>(basePlayerState));
        if (!player) continue;
        if (playerDeadStates || playerLastDeathPositions)
        {
            auto* pawn = ResolveTakaroPlayerPawn(
                static_cast<SDK::ACrPlayerStateBase*>(basePlayerState));
            if (pawn && pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
            {
                auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);
                if (playerDeadStates) (*playerDeadStates)[player->gameId] = character->bDead;
                if (playerLastDeathPositions)
                {
                    const auto& death = character->LastDeathTransform.Translation;
                    (*playerLastDeathPositions)[player->gameId] =
                        TakaroPositionDto{death.X, death.Y, death.Z, std::nullopt};
                }
            }
        }
        if (auto* playerController = basePlayerState->GetPlayerController())
            controllerGameIds[playerController] = player->gameId;
        players.push_back(std::move(*player));
    }
    return true;
#else
    return false;
#endif
}

ITakaroGameOperations& TakaroGameOperations() { return g_operations; }
