#include "plugin_state.h"
#include "takaro_protocol.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace TakaroProtocol;

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Contains(const std::string& text, const std::string& part)
{
    if (text.find(part) == std::string::npos) throw std::runtime_error("missing " + part + " in " + text);
}
void Absent(const std::string& text, const std::string& part)
{
    if (text.find(part) != std::string::npos) throw std::runtime_error("unexpected " + part + " in " + text);
}

void ManifestAndIdentify()
{
    const auto identify = BuildIdentify({"id\"1", "reg", "My Server", "1.2.3"}, {"abc", 42});
    Contains(identify, "\"identityToken\":\"id\\\"1\"");
    Contains(identify, "\"protocolVersions\":[1]");
    Contains(identify, "\"game\":\"starrupture\"");
    Contains(identify, "{\"name\":\"players.progression\",\"parameters\":{\"source\":\"server\"}}");
    Contains(identify, "\"icon\":{\"revision\":\"abc\",\"size\":42}");
    Absent(identify, "console.execute");
    Absent(identify, "\"map\"");
    Absent(identify, "events.log");
    Absent(BuildCapabilities({}), "\"icon\"");
}

void ArgsShapes()
{
    const auto parsed = ParseRequest(
        R"({"type":"request","requestId":"r1","payload":{"action":"getPlayer","args":"{\"gameId\":\"7\"}"}})");
    Expect(parsed && parsed->action == "getPlayer", "request not parsed");
    Expect(ExtractJsonString(parsed->argsJson, "gameId") == "7", "JSON string args not decoded");
    for (const auto* frame : {
             R"({"requestId":"r","payload":{"action":"getPlayers","args":"{}"}})",
             R"({"requestId":"r","payload":{"action":"getPlayers","args":"[]"}})",
             R"({"requestId":"r","payload":{"action":"getPlayers","args":""}})",
             R"({"requestId":"r","payload":{"action":"getPlayers","args":null}})",
             R"({"requestId":"r","payload":{"action":"getPlayers","args":[]}})",
             R"({"requestId":"r","payload":{"action":"getPlayers"}})"})
    {
        const auto request = ParseRequest(frame);
        Expect(request && request->argsJson == "{}", "empty args shape not normalized");
    }
    const auto object = ParseRequest(R"({"requestId":"r","payload":{"action":"a","args":{"gameId":"9"}}})");
    Expect(object && ExtractJsonString(object->argsJson, "gameId") == "9", "object args lost");
    Expect(!ParseRequest(R"({"payload":{"action":"a"}})"), "request without an id accepted");
    Expect(!ParseRequest(R"({"requestId":"r","payload":null})"), "request without an action accepted");
}

void Validation()
{
    Expect(ValidateArgs("getPlayer", "{}").has_value(), "missing gameId accepted");
    Expect(!ValidateArgs("getPlayer", R"({"gameId":"7"})"), "valid gameId refused");
    Expect(ValidateArgs("getPlayer", R"({"gameId":"  "})").has_value(), "blank gameId accepted");
    Expect(!ValidateArgs("sendMessage", R"({"message":"hi","opts":null})"), "explicit null opts refused");
    Expect(ValidateArgs("sendMessage", "{\"message\":\"" + std::string(2001, 'x') + "\"}").has_value(), "long message");
    Expect(!ValidateArgs("giveItem", R"({"player":{"gameId":"7"},"item":"a","amount":2,"quality":null})"), "null quality");
    Expect(ValidateArgs("giveItem", R"({"player":{"gameId":"7"},"item":"a","amount":0})").has_value(), "zero amount");
    Expect(ValidateArgs("teleportPlayer", R"({"player":{"gameId":"7"},"x":1,"y":2})").has_value(), "missing z");
    Expect(!ValidateArgs("banPlayer", R"({"player":{"gameId":"7","name":"n"},"reason":"r","expiresAt":null})"), "null expiry");
    Expect(!ValidateArgs("banPlayer", R"({"player":{"gameId":"7"}})"), "a ban needs only the player");
    Expect(ValidateArgs("kickPlayer", R"({"player":{"gameId":"7"}})").has_value(), "missing reason");
    Expect(RunsOnGameThread("giveItem") && !RunsOnGameThread("getPlayers"), "routing");
    Expect(!RunsOnGameThread("getPlayerProgression"), "progression must not use the game thread");
    Expect(!RunsOnGameThread("executeConsoleCommand"), "console is unsupported");
}

void Frames()
{
    Contains(BuildResponse("r\"1", "null"), "\"requestId\":\"r\\\"1\",\"payload\":null");
    Contains(BuildError("r", "invalid_args", "bad"), "\"error\":{\"code\":\"invalid_args\",\"message\":\"bad\"}");
}

void Stream()
{
    EventStream stream("s1");
    stream.Push("chat-message", R"({"msg":"a"})");
    stream.Push("log", R"({"msg":"b"})");
    auto* first = stream.NextUnsent();
    Contains(first->frame, "\"streamId\":\"s1\",\"seq\":1");
    first->sent = true;
    stream.NextUnsent()->sent = true;
    Expect(!stream.NextUnsent(), "everything was sent");
    stream.Acknowledge("other", 2);
    Expect(stream.Size() == 2, "another stream's ack was honoured");
    stream.Acknowledge("s1", 1);
    Expect(stream.Size() == 1, "ack did not release the event");
    stream.MarkAllUnsent();
    Expect(stream.NextUnsent()->seq == 2, "unacknowledged event not resent");
    stream.Push("log", "{}");
    Contains(stream.NextUnsent()->frame, "\"seq\":2");
    for (size_t index = 0; index < EventBufferLimit + 5; ++index) stream.Push("log", "{}");
    Expect(stream.Size() == EventBufferLimit, "buffer is not bounded");
    Expect(stream.Dropped() > 0, "dropped events not counted");
}

void Answers()
{
    TakaroPluginState state;
    Contains(state.ReachabilityJson(), "\"connectable\":false");
    Expect(state.PlayerListJson() == "[]", "empty list");
    Expect(state.PlayerJsonFor("x") == "null", "unknown player must be null");
    Expect(state.ProgressionJsonFor("x") == "null", "unknown progression must be null");
    Expect(state.LocationJsonFor("x") == "null", "unknown location must be null");

    TakaroPlayerSnapshot player;
    player.gameId = "76561198000000000";
    player.name = "One";
    player.platformId = "steam:76561198000000000";
    player.steamId = player.gameId;
    player.hasLocation = true;
    player.x = 1;
    state.ReplacePlayers({player});
    Expect(state.ProgressionJsonFor(player.gameId) == "null", "unloaded skills must be unknown, never level 0");
    Contains(state.PlayerListJson(), "\"steamId\":\"76561198000000000\"");
    Absent(state.PlayerListJson(), "location");

    player.skills = std::vector<TakaroSkillReading>{{"movement", 3, 120.5}, {"combat", 1, 0}};
    state.ReplacePlayers({player});
    const auto tracks = state.ProgressionJsonFor(player.gameId);
    Contains(tracks, R"({"key":"skill.movement","name":"Movement","level":3,"experience":120.500})");
    Contains(tracks, R"("key":"skill.combat")");
    Absent(tracks, "\"character\"");
    Absent(state.PlayerListJson(), "skills");

    state.ReplacePlayers({});
    Expect(state.ProgressionJsonFor(player.gameId) == tracks, "reading lost when the player left");
    Contains(state.LocationJsonFor(player.gameId), "\"x\":1");

    player.skills = std::vector<TakaroSkillReading>{};
    state.ReplacePlayers({player});
    Expect(state.ProgressionJsonFor(player.gameId) == "[]", "known player with no tracks must be []");
}
}

int main()
{
    try
    {
        ManifestAndIdentify();
        ArgsShapes();
        Validation();
        Frames();
        Stream();
        Answers();
        std::cout << "takaro_protocol_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
