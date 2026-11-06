#pragma once

#include "plugin_state.h"
#include <optional>
#include <string>

struct TakaroTeleportValidation
{
    bool ok = false;
    std::string gameId;
    double x = 0;
    double y = 0;
    double z = 0;
    std::string error;
};

struct TakaroKickValidation
{
    bool ok = false;
    std::string gameId;
    std::string reason;
    std::string error;
};

struct TakaroMessageValidation
{
    bool ok = false;
    std::string message;
    std::optional<std::string> recipientGameId;
    std::string senderName;
    std::string rendered;
    std::string error;
};

TakaroTeleportValidation ValidateTakaroTeleport(
    const std::string& gameId, double x, double y, double z);
bool IsTakaroTeleportPostcondition(
    double requestedX, double requestedY, double requestedZ,
    double actualX, double actualY, double actualZ);
TakaroKickValidation ValidateTakaroKick(const std::string& gameId, const std::string& reason);
// A recipient message is rendered for the global chat multicast as
// "[<sender>] [to <recipientDisplayName>] <message>": StarRupture exposes no
// per-player chat RPC, so a direct message is a visible-to-all fallback.
// recipientDisplayName is the recipient's server-reported display name, never
// the gameId; an empty name renders as "player".
TakaroMessageValidation ValidateTakaroMessage(
    const std::string& message,
    const std::optional<std::string>& recipientGameId,
    const std::string& senderName,
    const std::string& recipientDisplayName = "");
TakaroOperationResult ExecuteTakaroPlayerAction(const TakaroOperationRequest& request);
