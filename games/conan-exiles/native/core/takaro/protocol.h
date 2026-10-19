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

// Who the connector is, sent in identify on protocol 0 and 1. Empty fields are left out.
struct ConnectorIdentity {
    std::string game;     // the game slug ("conan-exiles")
    std::string name;     // the connector slug ("takaro-conan-exiles")
    std::string version;  // strict semver, the release version
};

// `capabilities` (a JSON array) set: protocol 1 is offered as protocolVersions [0, 1] with that
// Capability Manifest; empty: the protocol 0 identify, unchanged.
std::string CreateIdentify(const std::string& identityToken, const std::string& registrationToken,
                           const std::string& serverName, const std::string& capabilities = "",
                           const ConnectorIdentity& identity = {});
std::string CreateResponse(const std::string& requestId, const JsonValue& payload);  // Null payload -> {}
std::string CreateErrorResponse(const std::string& requestId, const std::string& error);
std::string CreateGameEvent(const std::string& type, const JsonValue& data);

// Protocol 1 (Takaro docs, "Generic Game Protocol v1").
// The response error codes; anything else is sent as game_error.
constexpr const char* kUnsupported = "unsupported";
constexpr const char* kInvalidArgs = "invalid_args";
constexpr const char* kNotFound = "not_found";
constexpr const char* kGameError = "game_error";
constexpr const char* kInternal = "internal";
bool IsErrorCode(const std::string& code);
std::string CreateResponseV1(const std::string& requestId, const JsonValue& payload);  // Null payload -> null
std::string CreateErrorResponseV1(const std::string& requestId, const std::string& code, const std::string& message);
std::string CreateCapabilities(const std::string& requestId, const std::string& capabilities);
// A protocol 0 gameEvent frame (CreateGameEvent) with streamId and seq added to its payload; the frame
// unchanged when it is not one.
std::string WithStream(const std::string& frame, const std::string& streamId, uint64_t seq);
// ^[A-Za-z0-9_-]{1,128}$
bool IsStreamId(const std::string& id);

// The player id of a request: flat { gameId } / { steamId } / { platformId }, or nested under
// player / playerRef. Modules send explicit JSON null for absent fields. Empty when there is none.
std::string PlayerId(const JsonValue& args);
// "steam:7656..." / "platform:steam:7656..." -> "7656..." (case-insensitive prefix).
std::string StripSteamPrefix(const std::string& id);

// ISO-8601 (or an epoch-ms digit string) to Unix ms; false when it is neither or has no zone.
bool ParseIsoMs(const std::string& text, int64_t& ms);
std::string FormatIsoMs(int64_t ms);  // Date#toISOString

}  // namespace takaro
