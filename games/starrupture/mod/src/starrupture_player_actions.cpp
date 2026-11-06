#include "starrupture_player_actions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#endif

namespace
{
constexpr double kWorldCoordinateLimit = 20'000'000.0;

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
TakaroOperationFailure Failure(const TakaroOperationRequest& request, const std::string& reason)
{
    return TakaroOperationFailure{request.action + ": " + reason};
}

SDK::ACrPlayerStateBase* FindPlayerState(const std::string& gameId)
{
    auto* world = SDK::UWorld::GetWorld();
    auto* gameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!gameState) return nullptr;
    for (auto* basePlayerState : gameState->PlayerArray)
    {
        if (!basePlayerState || !basePlayerState->IsA(SDK::ACrPlayerStateBase::StaticClass())) continue;
        auto* playerState = static_cast<SDK::ACrPlayerStateBase*>(basePlayerState);
        if (playerState->GetUniqueNetIdAsString().ToString() == gameId) return playerState;
    }
    return nullptr;
}

SDK::ACrPlayerControllerBase* FindController(const std::string& gameId)
{
    auto* playerState = FindPlayerState(gameId);
    if (!playerState) return nullptr;
    auto* controller = playerState->GetPlayerController();
    if (!controller || !controller->IsA(SDK::ACrPlayerControllerBase::StaticClass())) return nullptr;
    return static_cast<SDK::ACrPlayerControllerBase*>(controller);
}

TakaroOperationResult MulticastChat(const TakaroOperationRequest& request, const std::string& rendered)
{
    const auto wideMessage = UtfN::Utf8StringToUtf16String<std::wstring>(rendered);
    const SDK::FString gameMessage(wideMessage.c_str());
    auto* world = SDK::UWorld::GetWorld();
    auto* baseGameState = world ? SDK::UGameplayStatics::GetGameState(world) : nullptr;
    if (!baseGameState || !baseGameState->IsA(SDK::ACrGameStateBase::StaticClass()))
        return Failure(request, "StarRupture game state is unavailable");
    static_cast<SDK::ACrGameStateBase*>(baseGameState)->MulticastClientChatCommit(nullptr, gameMessage);
    return TakaroOperationSuccess{request.type, std::monostate{}};
}

#endif
}

TakaroTeleportValidation ValidateTakaroTeleport(
    const std::string& gameId, double x, double y, double z)
{
    TakaroTeleportValidation result;
    result.gameId = Trim(gameId);
    result.x = x;
    result.y = y;
    result.z = z;
    if (result.gameId.empty())
    {
        result.error = "an exact connected player gameId is required";
        return result;
    }
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::abs(x) > kWorldCoordinateLimit || std::abs(y) > kWorldCoordinateLimit ||
        std::abs(z) > kWorldCoordinateLimit)
    {
        result.error = "teleport coordinates must be finite and inside the configured world bound";
        return result;
    }
    result.ok = true;
    return result;
}

TakaroKickValidation ValidateTakaroKick(const std::string& gameId, const std::string& reason)
{
    TakaroKickValidation result;
    result.gameId = Trim(gameId);
    result.reason = Trim(reason);
    if (result.gameId.empty()) result.error = "an exact connected player gameId is required";
    else if (result.reason.empty() || result.reason.size() > 512)
        result.error = "kick reason must contain 1 through 512 bytes";
    else result.ok = true;
    return result;
}

bool IsTakaroTeleportPostcondition(
    double requestedX, double requestedY, double requestedZ,
    double actualX, double actualY, double actualZ)
{
    // K2_TeleportTo sweeps, so a character asked to arrive above terrain settles on the
    // ground and its Z never matches the request. Judge the horizontal placement tightly
    // and allow the vertical settle: the character owns its final Z, we own the XY.
    constexpr double kHorizontalToleranceCentimetres = 5.0;
    constexpr double kVerticalSettleCentimetres = 300.0;
    const double x = actualX - requestedX;
    const double y = actualY - requestedY;
    const double z = actualZ - requestedZ;
    if (std::sqrt(x * x + y * y) > kHorizontalToleranceCentimetres) return false;
    // Settling is downward: the sweep drops the character onto the ground, it never
    // lifts it far above the requested point.
    return z <= kHorizontalToleranceCentimetres && z >= -kVerticalSettleCentimetres;
}

TakaroMessageValidation ValidateTakaroMessage(
    const std::string& message,
    const std::optional<std::string>& recipientGameId,
    const std::string& senderName,
    const std::string& recipientDisplayName)
{
    TakaroMessageValidation result;
    result.message = Trim(message);
    if (recipientGameId) result.recipientGameId = Trim(*recipientGameId);
    result.senderName = Trim(senderName).empty() ? "Takaro" : Trim(senderName);
    if (result.message.empty() || result.message.size() > 2000)
    {
        result.error = "message must contain 1 through 2000 bytes";
        return result;
    }
    if ((result.recipientGameId && result.recipientGameId->empty()) || result.senderName.size() > 64)
    {
        result.error = "message recipient or sender name is invalid";
        return result;
    }
    result.rendered = "[" + result.senderName + "] ";
    if (result.recipientGameId)
    {
        // Visible-to-all fallback: the game has no per-player chat RPC, so the
        // recipient is named in the text rather than addressed privately.
        std::string displayName = Trim(recipientDisplayName);
        if (displayName.empty() || displayName == *result.recipientGameId) displayName = "player";
        if (displayName.size() > 64) displayName.resize(64);
        result.rendered += "[to " + displayName + "] ";
    }
    result.rendered += result.message;
    result.ok = true;
    return result;
}

TakaroOperationResult ExecuteTakaroPlayerAction(const TakaroOperationRequest& request)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    if (request.type == TakaroOperationType::SendMessage)
    {
        const auto message = ExtractJsonString(request.argsJson, "message");
        const auto opts = ExtractJsonObject(request.argsJson, "opts");
        std::optional<std::string> recipientGameId;
        bool directMessageRequested = false;
        std::string senderName = "Takaro";
        if (opts)
        {
            if (const auto recipient = ExtractJsonObject(*opts, "recipient"))
            {
                directMessageRequested = true;
                recipientGameId = ExtractJsonString(*recipient, "gameId");
            }
            if (const auto sender = ExtractJsonString(*opts, "senderNameOverride")) senderName = *sender;
        }
        if (!message) return Failure(request, "message is required");
        if (directMessageRequested && !recipientGameId)
            return Failure(request, "direct message recipient gameId is required");
        std::string recipientDisplayName;
        if (recipientGameId && !Trim(*recipientGameId).empty())
        {
            // The recipient must be connected; the vanilla client has no
            // per-player chat RPC (ClientTeamMessage only feeds a developer
            // console shipping clients lack), so the message is multicast with
            // the recipient named in the text.
            auto* playerState = FindPlayerState(Trim(*recipientGameId));
            if (!playerState) return Failure(request, "direct message recipient is not connected");
            recipientDisplayName = playerState->GetPlayerName().ToString();
        }
        const auto validated = ValidateTakaroMessage(*message, recipientGameId, senderName, recipientDisplayName);
        if (!validated.ok) return Failure(request, validated.error);
        return MulticastChat(request, validated.rendered);
    }
    const auto player = ExtractJsonObject(request.argsJson, "player");
    const auto gameId = player ? ExtractJsonString(*player, "gameId") : std::nullopt;
    if (request.type == TakaroOperationType::TeleportPlayer)
    {
        const auto x = ExtractJsonFiniteNumber(request.argsJson, "x");
        const auto y = ExtractJsonFiniteNumber(request.argsJson, "y");
        const auto z = ExtractJsonFiniteNumber(request.argsJson, "z");
        if (!gameId || !x || !y || !z) return Failure(request, "player and finite x, y, z are required");
        const auto teleport = ValidateTakaroTeleport(*gameId, *x, *y, *z);
        if (!teleport.ok) return Failure(request, teleport.error);
        auto* controller = FindController(teleport.gameId);
        auto* pawn = controller ? controller->K2_GetPawn() : nullptr;
        if (!controller || !pawn) return Failure(request, "connected player controller or pawn is unavailable");
        if (!pawn->IsA(SDK::ACrCharacterPlayerBase::StaticClass()))
            return Failure(request, "connected pawn is not a StarRupture player character");
        auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(pawn);
        const SDK::FVector requested(teleport.x, teleport.y, teleport.z);
        const auto rotation = character->K2_GetActorRotation();
        const auto location = character->K2_GetActorLocation();
        if (IsTakaroTeleportPostcondition(
                teleport.x, teleport.y, teleport.z, location.X, location.Y, location.Z))
            return TakaroOperationSuccess{request.type, std::monostate{}};
        // Name the postcondition and the position actually read back, so an expiry says
        // what was wrong instead of implying the work never reached the game thread.
        std::ostringstream pending;
        pending << "teleport postcondition not met: the character was last read back at ("
                << location.X << ", " << location.Y << ", " << location.Z
                << ") for a request to (" << teleport.x << ", " << teleport.y << ", "
                << teleport.z << ")";
        const auto pendingReason = pending.str();
        if (request.attempt > 0)
            return TakaroOperationDeferred{pendingReason};
        if (!character->K2_TeleportTo(requested, rotation))
            return Failure(request, "server-owned character teleport rejected the requested location");
        character->ClientTeleportTo(requested, rotation, false, false);
        return TakaroOperationDeferred{pendingReason};
    }
    if (request.type == TakaroOperationType::KickPlayer)
    {
        const auto reason = ExtractJsonString(request.argsJson, "reason");
        if (!gameId || !reason) return Failure(request, "player and reason are required");
        const auto kick = ValidateTakaroKick(*gameId, *reason);
        if (!kick.ok) return Failure(request, kick.error);
        auto* controller = FindController(kick.gameId);
        if (!controller) return Failure(request, "connected player controller is unavailable");
        const auto wideReason = UtfN::Utf8StringToUtf16String<std::wstring>(kick.reason);
        const SDK::FString reasonString(wideReason.c_str());
        const auto reasonText = SDK::UKismetTextLibrary::Conv_StringToText(reasonString);
        controller->ClientReturnToMainMenuWithTextReason(reasonText);
        return TakaroOperationSuccess{request.type, std::monostate{}};
    }
    return Failure(request, "operation is not a player action");
#else
    return TakaroOperationFailure{request.action + " is unsupported without the pinned StarRupture Game SDK"};
#endif
}
