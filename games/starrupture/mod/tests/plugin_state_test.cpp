#include "plugin_state.h"
#include "plugin_hook_requirements.h"
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Contains(const std::string& value, const std::string& expected) { Expect(value.find(expected) != std::string::npos, expected.c_str()); }

TakaroOperationResult EmptySuccess(TakaroOperationType type)
{
    switch (type)
    {
    case TakaroOperationType::GetPlayer:
        return TakaroOperationSuccess{type, std::optional<TakaroPlayerSnapshot>{}};
    case TakaroOperationType::GetPlayers:
        return TakaroOperationSuccess{type, std::vector<TakaroPlayerSnapshot>{}};
    case TakaroOperationType::GetPlayerLocation:
        return TakaroOperationSuccess{type, std::optional<TakaroPositionDto>{}};
    case TakaroOperationType::GetPlayerInventory:
    case TakaroOperationType::ListItems:
        return TakaroOperationSuccess{type, std::vector<TakaroItemDto>{}};
    case TakaroOperationType::ListEntities:
        return TakaroOperationSuccess{type, std::vector<TakaroEntityDto>{}};
    case TakaroOperationType::ListLocations:
        return TakaroOperationSuccess{type, std::vector<TakaroLocationDto>{}};
    case TakaroOperationType::ExecuteConsoleCommand:
        return TakaroOperationSuccess{type, TakaroCommandOutputDto{"status", true, std::nullopt}};
    case TakaroOperationType::TestReachability:
        return TakaroOperationSuccess{type, TakaroReachabilityDto{true, std::nullopt, std::nullopt}};
    case TakaroOperationType::ListBans:
        return TakaroOperationSuccess{type, std::vector<TakaroBanDto>{}};
    case TakaroOperationType::GetMapInfo:
        return TakaroOperationSuccess{type, TakaroMapInfoDto{}};
    case TakaroOperationType::GetMapTile:
        return TakaroOperationSuccess{type, std::string{}};
    case TakaroOperationType::GiveItem:
    case TakaroOperationType::SendMessage:
    case TakaroOperationType::TeleportPlayer:
    case TakaroOperationType::KickPlayer:
    case TakaroOperationType::BanPlayer:
    case TakaroOperationType::UnbanPlayer:
    case TakaroOperationType::Shutdown:
        return TakaroOperationSuccess{type, std::monostate{}};
    }
    throw std::runtime_error("unhandled Takaro operation type");
}

class FakeOperations final : public ITakaroGameOperations
{
public:
    TakaroOperationResult Execute(const TakaroOperationRequest& request) override
    {
        requests.push_back(request);
        if (state) state->HealthJson();
        if (onExecute) onExecute(request);
        if (resultFactory) return resultFactory(request);
        return EmptySuccess(request.type);
    }
    TakaroPluginState* state = nullptr;
    std::function<void(const TakaroOperationRequest&)> onExecute;
    std::function<TakaroOperationResult(const TakaroOperationRequest&)> resultFactory;
    std::vector<TakaroOperationRequest> requests;
};
}

int main()
{
    try
    {
        auto now = std::chrono::steady_clock::time_point{};
        TakaroPluginState state([&] { return now; });
        state.Configure({"status"}, {"save "}, 2, 2, 2);

        // Readiness follows the observed world: the DedicatedServerStart lobby and a
        // still-loading ChimeraMain never report reachable, whatever reconciliation says.
        Contains(state.HealthJson(), "\"reachable\":false");
        Contains(state.HealthJson(), "\"phase\":\"unknown\"");
        Contains(state.HealthJson(), "\"world\":null");
        state.SetWorldPhase(TakaroWorldPhase::Lobby, "DedicatedServerStart");
        state.SetReady(true);
        Expect(state.WorldPhase() == TakaroWorldPhase::Lobby, "world phase was not retained");
        Contains(state.HealthJson(), "\"reachable\":false");
        Contains(state.HealthJson(), "\"phase\":\"lobby\"");
        Contains(state.HealthJson(), "\"world\":\"DedicatedServerStart\"");
        Contains(state.HealthJson(), "Manage Server");
        state.SetWorldPhase(TakaroWorldPhase::Loading, "ChimeraMain");
        state.SetReady(true);
        Contains(state.HealthJson(), "\"reachable\":false");
        Contains(state.HealthJson(), "\"phase\":\"loading\"");
        state.SetWorldPhase(TakaroWorldPhase::Gameplay, "ChimeraMain");
        Contains(state.HealthJson(), "\"reachable\":false");
        state.SetReady(true);
        Contains(state.HealthJson(), "\"reachable\":true");
        Contains(state.HealthJson(), "\"phase\":\"gameplay\"");
        Contains(state.HealthJson(), "\"world\":\"ChimeraMain\"");
        Contains(state.HealthJson(), "\"reason\":null");
        // Leaving the gameplay world drops readiness without a separate SetReady call.
        state.SetWorldPhase(TakaroWorldPhase::Lobby, "DedicatedServerStart");
        Contains(state.HealthJson(), "\"reachable\":false");
        state.SetWorldPhase(TakaroWorldPhase::Gameplay, "ChimeraMain");
        state.SetReady(true);
        state.ClearWorldState();
        Contains(state.HealthJson(), "\"phase\":\"unknown\"");
        Contains(state.HealthJson(), "\"world\":null");
        state.SetWorldPhase(TakaroWorldPhase::Gameplay, "ChimeraMain");
        state.SetReady(true);
        Contains(state.HealthJson(), "\"reachable\":true");
        Expect(EssentialTakaroHookGroupsAvailable(true, true, true), "complete essential hooks rejected");
        Expect(!EssentialTakaroHookGroupsAvailable(false, true, true), "missing engine hooks accepted");
        Expect(!EssentialTakaroHookGroupsAvailable(true, false, true), "missing world hooks accepted");
        Expect(!EssentialTakaroHookGroupsAvailable(true, true, false), "missing player hooks accepted");

        std::string parsedAction;
        std::string parsedArgs;
        int64_t parsedDeadlineUnixMs = 0;
        std::string parseError;
        Expect(ParseActionRequest("{\"action\":\"sendMessage\",\"args\":{\"message\":\"hello\"},\"deadlineUnixMs\":4102444800000}",
            parsedAction, parsedArgs, parsedDeadlineUnixMs, parseError),
            "valid action request rejected");
        Expect(parsedAction == "sendMessage" && parsedArgs == "{\"message\":\"hello\"}", "action request parsed incorrectly");
        Expect(parsedDeadlineUnixMs == 4102444800000LL, "action deadline parsed incorrectly");
        Expect(!ParseActionRequest("{\"action\":\"sendMessage\",\"action\":\"executeConsoleCommand\",\"args\":{},\"deadlineUnixMs\":4102444800000}",
            parsedAction, parsedArgs, parsedDeadlineUnixMs, parseError), "duplicate action accepted");
        Expect(!ParseActionRequest("{\"outer\":{\"action\":\"sendMessage\"},\"args\":{},\"deadlineUnixMs\":4102444800000}",
            parsedAction, parsedArgs, parsedDeadlineUnixMs, parseError),
            "nested action accepted");
        Expect(!ParseActionRequest("{\"action\":\"sendMessage\",\"args\":{},\"deadlineUnixMs\":4102444800000} trailing",
            parsedAction, parsedArgs, parsedDeadlineUnixMs, parseError),
            "trailing JSON accepted");
        const auto nestedPlayer = ExtractJsonObject("{\"player\":{\"gameId\":\"stable-id\"},\"amount\":2}", "player");
        Expect(nestedPlayer && ExtractJsonString(*nestedPlayer, "gameId") == std::optional<std::string>{"stable-id"},
            "nested player object was not extracted exactly");
        Expect(ExtractJsonPositiveInteger("{\"amount\":2}", "amount") == std::optional<int64_t>{2},
            "positive integer was not extracted");
        Expect(!ExtractJsonPositiveInteger("{\"amount\":2.5}", "amount"), "fractional integer was accepted");
        Expect(ExtractJsonFiniteNumber("{\"x\":-123.5}", "x") == std::optional<double>{-123.5},
            "finite signed coordinate was rejected");
        Expect(ExtractJsonFiniteNumber("{\"x\":1.25e2}", "x") == std::optional<double>{125.0},
            "finite exponent coordinate was rejected");
        Expect(!ExtractJsonFiniteNumber("{\"x\":\"NaN\"}", "x"), "non-number coordinate was accepted");
        Expect(!ExtractJsonObject("{\"outer\":{\"player\":{}}}", "player"), "nested object confused with top-level key");

        const auto unproved = state.CreateAction("sendMessage", "{\"message\":\"hello\"}");
        Contains(unproved, "actionId");
        const auto unprovedId = ExtractJsonString(unproved, "actionId").value();
        state.ProcessPendingActions();
        Contains(state.ActionJson(unprovedId), "unsupported by the pinned SDK bridge");

        FakeOperations operations;
        operations.state = &state;
        state.SetGameOperations(&operations);
        const auto blocked = state.CreateAction("executeConsoleCommand", "{\"command\":\"shutdown\"}");
        state.ProcessPendingActions();
        Contains(state.ActionJson(ExtractJsonString(blocked, "actionId").value()), "blocked by plugin allowlist");
        Expect(operations.requests.empty(), "blocked command reached SDK boundary");

        const auto allowed = state.CreateAction("executeConsoleCommand", "{\"command\":\" STATUS \"}");
        state.ProcessPendingActions();
        Contains(state.ActionJson(ExtractJsonString(allowed, "actionId").value()), "completed");
        Expect(operations.requests.size() == 1, "allowlisted typed command did not reach SDK boundary");
        Expect(operations.requests.back().type == TakaroOperationType::ExecuteConsoleCommand,
            "console action received the wrong typed operation");

        const auto message = state.CreateAction("sendMessage", "{}");
        state.ProcessPendingActions();
        Contains(state.ActionJson(ExtractJsonString(message, "actionId").value()), "completed");
        Expect(operations.requests.back().type == TakaroOperationType::SendMessage,
            "message action received the wrong typed operation");
        Contains(state.CreateAction("unknown", "{}"), "unknown action");

        // Multi-tick operations retain their correlation and advance a bounded execution phase.
        operations.resultFactory = [](const TakaroOperationRequest& request) -> TakaroOperationResult {
            if (request.attempt == 0) return TakaroOperationDeferred{};
            return TakaroOperationSuccess{request.type, std::monostate{}};
        };
        const auto deferred = state.CreateAction("teleportPlayer", "{}");
        const auto deferredId = ExtractJsonString(deferred, "actionId").value();
        state.ProcessPendingActions();
        Contains(state.ActionJson(deferredId), "\"state\":\"pending\"");
        Expect(operations.requests.back().attempt == 0, "first deferred attempt was not zero");
        const auto deferredWorkToken = operations.requests.back().workToken;
        state.ProcessPendingActions();
        Contains(state.ActionJson(deferredId), "\"state\":\"completed\"");
        Expect(operations.requests.back().attempt == 1, "deferred attempt did not advance");
        Expect(operations.requests.back().workToken == deferredWorkToken,
            "deferred operation changed its correlation token");

        operations.resultFactory = [](const TakaroOperationRequest&) -> TakaroOperationResult {
            return TakaroOperationDeferred{};
        };
        const auto deferredTimeout = state.CreateAction("teleportPlayer", "{}");
        const auto deferredTimeoutId = ExtractJsonString(deferredTimeout, "actionId").value();
        state.ProcessPendingActions();
        const auto requestsBeforeDeferredTimeout = operations.requests.size();
        now += std::chrono::seconds(3);
        state.ProcessPendingActions();
        Contains(state.ActionJson(deferredTimeoutId), "expired before game-thread processing");
        Expect(operations.requests.size() == requestsBeforeDeferredTimeout,
            "expired deferred operation reached the game boundary again");
        // A deferred operation that names why it is still waiting reports that reason when it
        // expires, instead of claiming the work never reached the game thread.
        operations.resultFactory = [](const TakaroOperationRequest&) -> TakaroOperationResult {
            return TakaroOperationDeferred{"teleport postcondition not met: the character was last "
                                           "read back at (1, 2, 3) for a request to (1, 2, 400)"};
        };
        const auto reasonedTimeout = state.CreateAction("teleportPlayer", "{}");
        const auto reasonedTimeoutId = ExtractJsonString(reasonedTimeout, "actionId").value();
        state.ProcessPendingActions();
        now += std::chrono::seconds(3);
        state.ProcessPendingActions();
        Contains(state.ActionJson(reasonedTimeoutId), "teleport postcondition not met");
        Contains(state.ActionJson(reasonedTimeoutId), "last read back at (1, 2, 3)");

        operations.resultFactory = {};
        now = std::chrono::steady_clock::time_point{};

        const auto completedShutdown = state.CreateAction("shutdown", "{}");
        const auto completedShutdownId = ExtractJsonString(completedShutdown, "actionId").value();
        state.ProcessPendingActions();
        const auto shutdownIdentity = state.CompletedActionIdentity(completedShutdownId);
        Expect(shutdownIdentity && shutdownIdentity->type == TakaroOperationType::Shutdown &&
                shutdownIdentity->workToken > 0 && shutdownIdentity->generation > 0,
            "completed shutdown was not eligible for explicit acknowledgement");
        Expect(!state.CompletedActionIdentity("missing-action"), "missing action was acknowledgement-eligible");

        const auto expired = state.CreateAction("sendMessage", "{\"message\":\"later\"}");
        now += std::chrono::seconds(3);
        state.ProcessPendingActions();
        Contains(state.ActionJson(ExtractJsonString(expired, "actionId").value()), "expired before game-thread processing");

        state.CreateAction("sendMessage", "{\"message\":\"one\"}");
        state.CreateAction("sendMessage", "{\"message\":\"two\"}");
        Contains(state.CreateAction("sendMessage", "{\"message\":\"three\"}"), "action queue full");
        state.ProcessPendingActions();

        // Terminal records are count-bounded even when time does not advance.
        for (int index = 0; index < 12; ++index)
        {
            state.CreateAction("shutdown", "{}");
            state.ProcessPendingActions();
        }
        Expect(state.RetainedActionCount() <= 4, "terminal action records grew without a count bound");

        // Only authoritative player lifecycle events are portable after Task 2; log remains unsupported.
        const auto firstInstanceEvents = state.EventsJson(0, 99);
        Contains(firstInstanceEvents, "\"instanceId\":");
        state.AddLogEvent("info", "must not be exposed");
        Contains(state.EventsJson(0, 99), "\"events\":[]");

        TakaroPlayerSnapshot invalid;
        invalid.name = "Name";
        state.UpsertPlayer(invalid);
        Contains(state.PlayersJson(), "\"players\":[]");
        TakaroPlayerSnapshot valid;
        valid.gameId = "stable-id";
        valid.name = "Player \\\"One\\\"";
        valid.hasLocation = true;
        valid.x = 100;
        state.AddPlayerConnected(valid);
        Contains(state.PlayersJson(), "stable-id");
        Contains(state.PlayersJson(), "\\\\\\\"One");
        const auto connectedEvents = state.EventsJson(0, 99);
        Contains(connectedEvents, "\"type\":\"player-connected\"");
        Contains(connectedEvents, "\"data\":{\"player\":{\"gameId\":\"stable-id\",\"name\":\"Player \\\\\\\"One\\\\\\\"\"");
        Expect(connectedEvents.find("location") == std::string::npos,
            "lifecycle event exposed Player Snapshot-only location fields");
        state.AddPlayerDisconnected(valid);
        Contains(state.PlayersJson(), "\"players\":[]");
        const auto lifecycleEvents = state.EventsJson(0, 99);
        Contains(lifecycleEvents, "\"type\":\"player-disconnected\"");

        TakaroPlayerSnapshot second = valid;
        second.gameId = "stable-id-2";
        second.name = "Two";
        second.platformId = "starrupture:stable-id-2";
        state.AddChatMessage(valid, "hello \"sector\"");
        auto emittedEvents = state.EventsJson(0, 99);
        Contains(emittedEvents, "\"type\":\"chat-message\"");
        Contains(emittedEvents, "\"msg\":\"hello \\\"sector\\\"\"");
        Contains(emittedEvents, "\"channel\":\"global\"");

        TakaroPositionDto deathPosition{1.0, 2.0, 3.0, std::nullopt};
        state.AddPlayerDeath(valid, second, deathPosition);
        emittedEvents = state.EventsJson(0, 99);
        Contains(emittedEvents, "\"type\":\"player-death\"");
        Contains(emittedEvents, "\"attacker\":{\"gameId\":\"stable-id-2\"");
        Contains(emittedEvents, "\"position\":{\"x\":1,\"y\":2,\"z\":3}");

        state.AddEntityKilled(valid, "Mite \"Scout\"", "Rifle");
        emittedEvents = state.EventsJson(0, 99);
        Contains(emittedEvents, "\"type\":\"entity-killed\"");
        Contains(emittedEvents, "\"entity\":\"Mite \\\"Scout\\\"\"");
        Contains(emittedEvents, "\"weapon\":\"Rifle\"");

        state.ReplacePlayers({valid, second});
        Contains(state.PlayersJson(), "stable-id-2");
        state.ReplacePlayers({second});
        Expect(state.PlayersJson().find("stable-id\"") == std::string::npos, "reconcile retained a missed leave");

        // Ending every session (deferred quit committed, world ending) reports one
        // disconnect per snapshot player with the reason as the event message,
        // empties the snapshot, and never repeats for a re-observed connection.
        state.ReplacePlayers({valid, second});
        const auto eventsBeforeEnd = state.EventsJson(0, 99);
        const auto ended = state.EndAllPlayerSessions("server \"quit\"");
        Expect(ended.size() == 2, "ending sessions did not report every snapshot player");
        Contains(state.PlayersJson(), "\"players\":[]");
        auto endedEvents = state.EventsJson(0, 99);
        Expect(endedEvents != eventsBeforeEnd, "ending sessions emitted nothing");
        Contains(endedEvents, "\"type\":\"player-disconnected\",\"data\":{\"player\":{\"gameId\":\"stable-id\"");
        Contains(endedEvents, "\"type\":\"player-disconnected\",\"data\":{\"player\":{\"gameId\":\"stable-id-2\"");
        Contains(endedEvents, "\"msg\":\"server \\\"quit\\\"\"");
        state.ReplacePlayers({valid, second});
        Expect(state.EndAllPlayerSessions("again").empty(), "a re-observed connection was ended twice");
        Expect(state.EventsJson(0, 99).find("\"msg\":\"again\"") == std::string::npos,
            "a re-observed connection produced a duplicate disconnect");
        state.AddPlayerConnected(valid);
        Expect(state.EndAllPlayerSessions("rejoined").size() == 1,
            "a session that reconnected after being ended was not ended again");
        Contains(state.EventsJson(0, 99),
            "\"type\":\"player-disconnected\",\"data\":{\"player\":{\"gameId\":\"stable-id\"");
        Contains(state.EventsJson(0, 99), "\"msg\":\"rejoined\"");
        state.ClearWorldState();
        Contains(state.HealthJson(), "\"reachable\":false");
        Contains(state.PlayersJson(), "\"players\":[]");

        // Every Generic Connector action has a stable typed operation before any game mutation exists.
        const std::vector<std::pair<std::string, TakaroOperationType>> operationsByName = {
            {"getPlayer", TakaroOperationType::GetPlayer}, {"getPlayers", TakaroOperationType::GetPlayers},
            {"getPlayerLocation", TakaroOperationType::GetPlayerLocation},
            {"getPlayerInventory", TakaroOperationType::GetPlayerInventory}, {"giveItem", TakaroOperationType::GiveItem},
            {"listItems", TakaroOperationType::ListItems}, {"listEntities", TakaroOperationType::ListEntities},
            {"listLocations", TakaroOperationType::ListLocations},
            {"executeConsoleCommand", TakaroOperationType::ExecuteConsoleCommand},
            {"sendMessage", TakaroOperationType::SendMessage}, {"teleportPlayer", TakaroOperationType::TeleportPlayer},
            {"testReachability", TakaroOperationType::TestReachability}, {"kickPlayer", TakaroOperationType::KickPlayer},
            {"banPlayer", TakaroOperationType::BanPlayer}, {"unbanPlayer", TakaroOperationType::UnbanPlayer},
            {"listBans", TakaroOperationType::ListBans}, {"shutdown", TakaroOperationType::Shutdown},
            {"getMapInfo", TakaroOperationType::GetMapInfo}, {"getMapTile", TakaroOperationType::GetMapTile},
        };
        state.Configure({"status"}, {}, 8, 2, 2);
        state.SetGameOperations(&operations);
        operations.requests.clear();
        for (const auto& [name, expectedType] : operationsByName)
        {
            const auto args = name == "executeConsoleCommand" ? "{\"command\":\"status\"}"
                : "{\"marker\":\"" + name + "\"}";
            const auto created = state.CreateAction(name, args);
            Contains(created, "actionId");
            state.ProcessPendingActions();
            Expect(operations.requests.back().type == expectedType, "operation name mapped to the wrong type");
            Expect(operations.requests.back().action == name, "typed request lost its action name");
            if (name != "executeConsoleCommand") Contains(operations.requests.back().argsJson, name);
        }

        // Typed success values must satisfy the native DTO contract before JSON serialization.
        state.Configure({"status"}, {}, 8, 2, 64);
        const auto expectInvalidResult = [&](const std::string& action, TakaroOperationType type, TakaroResultValue value) {
            operations.resultFactory = [type, value](const TakaroOperationRequest&) {
                return TakaroOperationResult{TakaroOperationSuccess{type, value}};
            };
            const auto created = state.CreateAction(action, action == "executeConsoleCommand" ? "{\"command\":\"status\"}" : "{}");
            const auto id = ExtractJsonString(created, "actionId").value();
            state.ProcessPendingActions();
            const auto result = state.ActionJson(id);
            Expect(result.find("invalid typed result") != std::string::npos,
                (action + " accepted invalid DTO: " + result).c_str());
        };
        TakaroPlayerSnapshot blankPlayer;
        blankPlayer.gameId = " ";
        blankPlayer.name = "Player";
        expectInvalidResult("getPlayer", TakaroOperationType::GetPlayer,
            std::optional<TakaroPlayerSnapshot>{blankPlayer});
        TakaroPlayerSnapshot nonFinitePlayer;
        nonFinitePlayer.gameId = "stable-id";
        nonFinitePlayer.name = "Player";
        nonFinitePlayer.hasLocation = true;
        nonFinitePlayer.x = std::numeric_limits<double>::infinity();
        expectInvalidResult("getPlayers", TakaroOperationType::GetPlayers,
            std::vector<TakaroPlayerSnapshot>{nonFinitePlayer});
        expectInvalidResult("getPlayerLocation", TakaroOperationType::GetPlayerLocation,
            std::optional<TakaroPositionDto>{TakaroPositionDto{std::numeric_limits<double>::quiet_NaN(), 0, 0, std::nullopt}});
        expectInvalidResult("listItems", TakaroOperationType::ListItems,
            std::vector<TakaroItemDto>{TakaroItemDto{"", "item-code", std::nullopt, 1, std::nullopt}});
        expectInvalidResult("getPlayerInventory", TakaroOperationType::GetPlayerInventory,
            std::vector<TakaroItemDto>{TakaroItemDto{"Item", "item-code", std::nullopt,
                std::numeric_limits<double>::infinity(), std::nullopt}});
        expectInvalidResult("listEntities", TakaroOperationType::ListEntities,
            std::vector<TakaroEntityDto>{TakaroEntityDto{"", "Enemy"}});
        expectInvalidResult("listLocations", TakaroOperationType::ListLocations,
            std::vector<TakaroLocationDto>{TakaroLocationDto{TakaroPositionDto{}, " "}});
        expectInvalidResult("listLocations", TakaroOperationType::ListLocations,
            std::vector<TakaroLocationDto>{TakaroLocationDto{
                TakaroPositionDto{0, std::numeric_limits<double>::infinity(), 0, std::nullopt}, "POI"}});
        expectInvalidResult("listLocations", TakaroOperationType::ListLocations,
            std::vector<TakaroLocationDto>{TakaroLocationDto{
                TakaroPositionDto{}, "POI", "poi:stable", "{not-json}"}});
        expectInvalidResult("executeConsoleCommand", TakaroOperationType::ExecuteConsoleCommand,
            TakaroCommandOutputDto{"", true, std::nullopt});
        expectInvalidResult("testReachability", TakaroOperationType::TestReachability,
            TakaroReachabilityDto{true, std::nullopt, std::numeric_limits<double>::quiet_NaN()});
        expectInvalidResult("testReachability", TakaroOperationType::TestReachability,
            TakaroReachabilityDto{true, std::nullopt, -1});
        TakaroBanDto blankBan;
        blankBan.player.gameId = "stable-id";
        blankBan.player.name = "Player";
        blankBan.reason = "";
        expectInvalidResult("listBans", TakaroOperationType::ListBans, std::vector<TakaroBanDto>{blankBan});
        TakaroBanDto datedBan = blankBan;
        datedBan.reason = "reason";
        const std::vector<std::string> invalidBanExpiries = {
            "tomorrow",
            "2025-02-29T23:59:59.999Z",
            "2024-04-31T23:59:59.999Z",
            "2024-02-29T24:00:00.000Z",
            "2024-02-29T23:60:00.000Z",
            "2024-02-29T23:59:60.000Z",
            "2024-02-29T23:59:59Z",
            "2024-02-29T23:59:59.999z",
            "2024-02-29T23:59:59.999+00:00",
            "2024-02-29T23:59:59.999+24:00",
        };
        for (const auto& expiresAt : invalidBanExpiries)
        {
            datedBan.expiresAt = expiresAt;
            expectInvalidResult("listBans", TakaroOperationType::ListBans,
                std::vector<TakaroBanDto>{datedBan});
        }
        operations.resultFactory = [&](const TakaroOperationRequest&) {
            datedBan.expiresAt = "2024-02-29T23:59:59.999Z";
            return TakaroOperationResult{TakaroOperationSuccess{
                TakaroOperationType::ListBans, std::vector<TakaroBanDto>{datedBan}}};
        };
        const auto normalizedBanAction = state.CreateAction("listBans", "{}");
        state.ProcessPendingActions();
        Contains(state.ActionJson(ExtractJsonString(normalizedBanAction, "actionId").value()),
            "2024-02-29T23:59:59.999Z");
        operations.resultFactory = [&](const TakaroOperationRequest&) {
            datedBan.expiresAt.reset();
            return TakaroOperationResult{TakaroOperationSuccess{
                TakaroOperationType::ListBans, std::vector<TakaroBanDto>{datedBan}}};
        };
        const auto undatedBanAction = state.CreateAction("listBans", "{}");
        state.ProcessPendingActions();
        const auto undatedBanResult = state.ActionJson(ExtractJsonString(undatedBanAction, "actionId").value());
        Contains(undatedBanResult, "\"state\":\"completed\"");
        Expect(undatedBanResult.find("expiresAt") == std::string::npos,
            "optional ban expiry was serialized when absent");
        operations.resultFactory = [](const TakaroOperationRequest& request) {
            if (request.type == TakaroOperationType::ListEntities)
                return TakaroOperationResult{TakaroOperationSuccess{request.type,
                    std::vector<TakaroEntityDto>{{"Mite", "Burrowing Mite", std::nullopt, "hostile", std::nullopt}}}};
            return TakaroOperationResult{TakaroOperationSuccess{request.type,
                std::vector<TakaroLocationDto>{{
                    TakaroPositionDto{10, -20, 30, std::nullopt}, "Echo Cavern",
                    "poi:00000001-00000002-00000003-00000004",
                    "{\"state\":\"visible-discovered\",\"type\":\"cave\"}"}}}};
        };
        const auto entityCatalogAction = state.CreateAction("listEntities", "{}");
        state.ProcessPendingActions();
        const auto entityCatalogJson = state.ActionJson(ExtractJsonString(entityCatalogAction, "actionId").value());
        Contains(entityCatalogJson, "\"type\":\"hostile\"");
        const auto locationCatalogAction = state.CreateAction("listLocations", "{}");
        state.ProcessPendingActions();
        const auto locationCatalogJson = state.ActionJson(ExtractJsonString(locationCatalogAction, "actionId").value());
        Contains(locationCatalogJson, "\"code\":\"poi:00000001-00000002-00000003-00000004\"");
        Contains(locationCatalogJson, "\"metadata\":{\"state\":\"visible-discovered\",\"type\":\"cave\"}");
        expectInvalidResult("getMapInfo", TakaroOperationType::GetMapInfo,
            TakaroMapInfoDto{false, 0, std::numeric_limits<double>::infinity(), 0, 0, 0});
        expectInvalidResult("getMapInfo", TakaroOperationType::GetMapInfo,
            TakaroMapInfoDto{false, 0, -1, 0, 0, 0});
        expectInvalidResult("getMapInfo", TakaroOperationType::GetMapInfo,
            TakaroMapInfoDto{true, 0, 1, 100, 100, 1});
        operations.resultFactory = {};

        // World end rejects queued work and prevents stale completion.
        const auto beforeWorldReset = state.CreateAction("shutdown", "{}");
        const auto beforeWorldResetId = ExtractJsonString(beforeWorldReset, "actionId").value();
        state.ClearWorldState();
        Contains(state.ActionJson(beforeWorldResetId), "world reset before game-thread processing");

        // Shutdown rejects new work and converts every queued action into a correlated failure.
        const auto beforeShutdown = state.CreateAction("shutdown", "{}");
        const auto beforeShutdownId = ExtractJsonString(beforeShutdown, "actionId").value();
        state.BeginShutdown();
        Contains(state.ActionJson(beforeShutdownId), "plugin is shutting down");
        Contains(state.CreateAction("shutdown", "{}"), "plugin is shutting down");
        state.Configure({}, {}, 8, 2, 2);
        state.SetGameOperations(&operations);

        // A result that arrives after its deadline is never reported as success.
        const auto timeoutAction = state.CreateAction("shutdown", "{}");
        const auto timeoutActionId = ExtractJsonString(timeoutAction, "actionId").value();
        operations.onExecute = [&](const TakaroOperationRequest&) { now += std::chrono::seconds(3); };
        state.ProcessPendingActions();
        Contains(state.ActionJson(timeoutActionId), "timed out during game-thread processing");

        // Reset must never reuse public correlation, even while an old operation is still returning.
        now = std::chrono::steady_clock::time_point{};
        state.Reset();
        state.Configure({}, {}, 8, 2, 2);
        state.SetGameOperations(&operations);
        const auto staleAction = state.CreateAction("shutdown", "{}");
        operations.onExecute = [&](const TakaroOperationRequest&) {
            state.Reset();
            state.Configure({}, {}, 8, 2, 2);
            state.SetGameOperations(&operations);
            const auto replacement = state.CreateAction("shutdown", "{}");
            const auto replacementId = ExtractJsonString(replacement, "actionId").value();
            Expect(replacementId != ExtractJsonString(staleAction, "actionId").value(),
                "reset reused a public action correlation id");
        };
        state.ProcessPendingActions();
        const auto staleActionId = ExtractJsonString(staleAction, "actionId").value();
        Contains(state.ActionJson(staleActionId), "\"state\":\"missing\"");

        Contains(JsonEscape(std::string("x\n\x01", 3)), "\\n\\u0001");
        state.Reset();
        Contains(state.HealthJson(), "\"reachable\":false");
        Contains(state.EventsJson(0, 10), "\"events\":[]");
        Expect(state.EventsJson(0, 10) != firstInstanceEvents, "bridge instance id survived state reset");
        Contains(state.CreateAction("shutdown", "{}"), "plugin is shutting down");
        std::cout << "plugin_state_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "plugin_state_test: " << error.what() << "\n";
        return 1;
    }
}
