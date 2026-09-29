// Takaro protocol shapes for Enshrouded: a line-for-line port of the former sidecar's
// sidecar/src/enshrouded/mapping.ts, sidecar/src/takaro/protocol.ts and the log helpers in
// sidecar/src/bridge.ts. Pure functions; the parity fixtures in tests/fixtures/ pin the outputs
// to what the sidecar produced for the same inputs.
#pragma once
#include "common.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace native {

// Error thrown by the ported mappers and actions. `kind` mirrors the sidecar's error classes so the
// response text is the same: ActionError, PluginHttpError, PluginUnimplementedError, PluginUnreachableError
// and plain Error (malformed plugin data).
enum class ErrorKind { Action, PluginHttp, PluginUnimplemented, PluginUnreachable, Plain };
struct NativeError : std::runtime_error {
    ErrorKind kind;
    int status;
    NativeError(ErrorKind k, const std::string& message, int httpStatus = 0)
        : std::runtime_error(message), kind(k), status(httpStatus) {}
};
[[noreturn]] void Fail(const std::string& message);  // ActionError

extern const char* const kGameServerActions[17];
extern const char* const kGameEventTypes[6];
bool IsGameEventType(const std::string& type);

// ---- mapping.ts ----
JsonValue MapPlayer(const JsonValue* raw);
JsonValue MapPosition(const JsonValue* raw);
JsonValue MapInventoryItem(const JsonValue* raw);
JsonValue MapItemDefinition(const JsonValue* raw);
std::string MapEntityType(const JsonValue* raw);
JsonValue MapEntity(const JsonValue* raw);
JsonValue MapLocation(const JsonValue* raw);
JsonValue MapBan(const JsonValue* raw);
struct MappedEvent {
    std::string type;
    JsonValue data;
};
// A plugin /events entry ({type, data}) as a Takaro gameEvent payload; nullopt for unknown types.
// Throws NativeError for malformed player/position data (the poller drops such events).
std::optional<MappedEvent> MapPluginEvent(const std::string& type, const JsonValue* data);

// ---- protocol.ts ----
// Takaro sends args as [], {}, a JSON string ("{}", "[]", "{...}"), "" or null.
JsonValue NormalizeArgs(const JsonValue* value);
std::string CreateIdentify(const std::string& identityToken, const std::string& registrationToken,
                           const std::string& serverName);
std::string CreateResponse(const std::string& requestId, const JsonValue& payload);
std::string CreateErrorResponse(const std::string& requestId, const std::string& error);
std::string CreateGameEvent(const std::string& type, const JsonValue& data);

// ---- adapter.ts helpers ----
// Accepts flat { gameId } / { steamId } / { platformId } or nested { player: {...} } / { playerRef: {...} }.
std::string PlayerId(const JsonValue& args);
std::string StripSteamPrefix(const std::string& id);  // id.replace(/^steam:/i, '')
// Array itself, or the first array under items|entities|locations|bans|data, else empty.
const std::vector<JsonValue>& ListOf(const JsonValue& value);

// ---- bridge.ts log helpers ----
enum class LogEventsMode { All, Filtered, None };
enum class LogTailMode { Auto, Always, Never };
bool ParseLogEventsMode(const std::string& text, LogEventsMode& out);
bool ParseLogTailMode(const std::string& text, LogTailMode& out);
bool ShouldForwardLog(LogEventsMode mode, const JsonValue& data);
// health == nullptr means the plugin health is unknown.
bool ShouldTailLog(LogTailMode mode, const JsonValue* health);

// ISO-8601 (and epoch-ms digit strings) to Unix ms; false when it is neither.
bool ParseIsoMs(const std::string& text, int64_t& ms);
std::string FormatIsoMs(int64_t ms);  // Date#toISOString

}  // namespace native
