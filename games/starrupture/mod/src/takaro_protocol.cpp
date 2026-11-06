#include "takaro_protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "plugin_state.h"

namespace TakaroProtocol
{
namespace
{
constexpr std::array<const char*, 17> PlainCapabilities = {
    "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
    "items.list", "items.give", "entities.list", "locations.list", "chat.send",
    "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown",
    "events.player-connected", "events.player-disconnected"};
constexpr std::array<const char*, 2> PlainEvents = {
    "events.chat-message", "events.player-death"};
constexpr std::array<const char*, 12> GameThreadActions = {
    "getPlayerInventory", "giveItem", "listItems", "listEntities", "listLocations", "sendMessage",
    "teleportPlayer", "kickPlayer", "banPlayer", "unbanPlayer", "listBans", "shutdown"};

std::string Trimmed(std::string value)
{
    const auto blank = [](unsigned char ch) { return ch == ' ' || (ch >= '\t' && ch <= '\r'); };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), blank));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), blank).base(), value.end());
    return value;
}

std::optional<std::string> NonBlank(const std::string& json, const std::string& key)
{
    auto value = ExtractJsonString(json, key);
    if (!value || Trimmed(*value).empty()) return std::nullopt;
    return value;
}

std::optional<std::string> NeedString(const std::string& json, const std::string& key, size_t limit = 0)
{
    const auto value = NonBlank(json, key);
    if (!value) return "Expected string argument '" + key + "'.";
    if (limit && value->size() > limit)
        return "Expected '" + key + "' to contain at most " + std::to_string(limit) + " characters.";
    return std::nullopt;
}

std::optional<std::string> NeedPlayer(const std::string& json)
{
    const auto player = ExtractJsonObject(json, "player");
    if (!player || !NonBlank(*player, "gameId")) return "Expected argument 'player' with a gameId.";
    return std::nullopt;
}
}

std::string BuildCapabilities(const Icon& icon)
{
    std::string json = "[";
    for (const auto* name : PlainCapabilities) json += "{\"name\":\"" + std::string(name) + "\"},";
    for (const auto* name : PlainEvents) json += "{\"name\":\"" + std::string(name) + "\"},";
    json += "{\"name\":\"players.progression\",\"parameters\":{\"source\":\"server\"}},";
    json += "{\"name\":\"game.profile\",\"parameters\":{\"name\":\"StarRupture\",\"version\":\"" +
        std::string(GameBuild) + "\"";
    if (!icon.revision.empty())
        json += ",\"icon\":{\"revision\":\"" + JsonEscape(icon.revision) + "\",\"size\":" +
            std::to_string(icon.size) + "}";
    return json + "}}]";
}

std::string BuildIdentify(const Identity& identity, const Icon& icon)
{
    return "{\"type\":\"identify\",\"payload\":{\"identityToken\":\"" + JsonEscape(identity.identityToken) +
        "\",\"registrationToken\":\"" + JsonEscape(identity.registrationToken) +
        "\",\"name\":\"" + JsonEscape(identity.serverName) +
        "\",\"game\":\"starrupture\",\"connector\":{\"name\":\"starrupture-connector\",\"version\":\"" +
        JsonEscape(identity.connectorVersion) + "\"},\"protocolVersions\":[1],\"capabilities\":" +
        BuildCapabilities(icon) + "}}";
}

std::optional<Request> ParseRequest(const std::string& frame)
{
    const auto requestId = ExtractJsonString(frame, "requestId");
    const auto payload = ExtractJsonObject(frame, "payload");
    const auto action = payload ? ExtractJsonString(*payload, "action") : std::nullopt;
    if (!requestId || requestId->empty() || !action || action->empty()) return std::nullopt;
    // Takaro sends args as a JSON string; older paths send an object, [], "" or null.
    auto args = ExtractJsonString(*payload, "args");
    if (!args) args = ExtractJsonObject(*payload, "args");
    std::string argsJson = args ? Trimmed(*args) : std::string{};
    if (argsJson.empty() || argsJson.front() != '{') argsJson = "{}";
    return Request{*requestId, *action, std::move(argsJson)};
}

std::string BuildResponse(const std::string& requestId, const std::string& payloadJson)
{
    return "{\"type\":\"response\",\"requestId\":\"" + JsonEscape(requestId) + "\",\"payload\":" + payloadJson + "}";
}

std::string BuildError(const std::string& requestId, const std::string& code, const std::string& message)
{
    return "{\"type\":\"response\",\"requestId\":\"" + JsonEscape(requestId) + "\",\"error\":{\"code\":\"" +
        JsonEscape(code) + "\",\"message\":\"" + JsonEscape(message) + "\"}}";
}

std::optional<std::string> ValidateArgs(const std::string& action, const std::string& argsJson)
{
    if (action == "getPlayer" || action == "getPlayerLocation" || action == "getPlayerInventory" ||
        action == "getPlayerProgression" || action == "unbanPlayer")
        return NeedString(argsJson, "gameId");
    if (action == "sendMessage") return NeedString(argsJson, "message", 2000);
    if (action == "giveItem")
    {
        if (auto problem = NeedPlayer(argsJson)) return problem;
        if (auto problem = NeedString(argsJson, "item")) return problem;
        const auto amount = ExtractJsonFiniteNumber(argsJson, "amount");
        if (!amount || *amount <= 0) return "Expected positive number argument 'amount'.";
        return std::nullopt;
    }
    if (action == "teleportPlayer")
    {
        if (auto problem = NeedPlayer(argsJson)) return problem;
        for (const auto* axis : {"x", "y", "z"})
            if (!ExtractJsonFiniteNumber(argsJson, axis))
                return "Expected finite number argument '" + std::string(axis) + "'.";
        return std::nullopt;
    }
    if (action == "kickPlayer")
    {
        if (auto problem = NeedPlayer(argsJson)) return problem;
        return NeedString(argsJson, "reason", 512);
    }
    if (action == "banPlayer") return NeedPlayer(argsJson);
    return std::nullopt;
}

bool RunsOnGameThread(const std::string& action)
{
    return std::find(GameThreadActions.begin(), GameThreadActions.end(), action) != GameThreadActions.end();
}

void EventStream::Push(const std::string& type, const std::string& dataJson)
{
    const auto seq = nextSeq_++;
    events_.push_back({seq, "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"" + JsonEscape(type) + "\",\"data\":" +
        dataJson + ",\"streamId\":\"" + JsonEscape(streamId_) + "\",\"seq\":" + std::to_string(seq) + "}}", false});
    if (events_.size() <= EventBufferLimit) return;
    events_.pop_front();
    ++dropped_;
}

void EventStream::Acknowledge(const std::string& streamId, uint64_t upTo)
{
    if (streamId != streamId_) return;
    while (!events_.empty() && events_.front().seq <= upTo) events_.pop_front();
}

void EventStream::MarkAllUnsent()
{
    for (auto& event : events_) event.sent = false;
}

BufferedEvent* EventStream::NextUnsent()
{
    const auto found = std::find_if(events_.begin(), events_.end(), [](const auto& event) { return !event.sent; });
    return found == events_.end() ? nullptr : &*found;
}
}
