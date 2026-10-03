// Takaro Generic connector protocol shapes (identify, response, error response, gameEvent) and the
// argument normalisation every action needs. Ported from the Enshrouded native connector
// (games/enshrouded/mod/src/native/mapping.h); pure functions, covered by tests/unit_test.cpp.
#pragma once
#include "common.h"

#include <cstdint>
#include <string>

namespace takaro {

// The 19 actions Takaro's Generic game server can request, and the 6 event types it accepts.
constexpr int kActionCount = 19;
extern const char* const kActions[kActionCount];
constexpr int kEventTypeCount = 6;
extern const char* const kEventTypes[kEventTypeCount];
bool IsAction(const std::string& action);
bool IsEventType(const std::string& type);

// Takaro sends args as [], {}, an object, a JSON-encoded string ("{...}", "[]", ""), or null.
// The result is always an object (possibly empty).
JsonValue NormalizeArgs(const JsonValue* value);

std::string CreateIdentify(const std::string& identityToken, const std::string& registrationToken,
                           const std::string& serverName);
std::string CreateResponse(const std::string& requestId, const JsonValue& payload);  // Null payload -> {}
std::string CreateErrorResponse(const std::string& requestId, const std::string& error);
std::string CreateGameEvent(const std::string& type, const JsonValue& data);

// The player id of a request: flat { gameId } / { steamId } / { platformId }, or nested under
// player / playerRef. Modules send explicit JSON null for absent fields. Empty when there is none.
std::string PlayerId(const JsonValue& args);
// "steam:7656..." / "platform:steam:7656..." -> "7656..." (case-insensitive prefix).
std::string StripSteamPrefix(const std::string& id);

// ISO-8601 (or an epoch-ms digit string) to Unix ms; false when it is neither or has no zone.
bool ParseIsoMs(const std::string& text, int64_t& ms);
std::string FormatIsoMs(int64_t ms);  // Date#toISOString

}  // namespace takaro
