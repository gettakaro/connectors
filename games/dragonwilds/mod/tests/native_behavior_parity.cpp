// Behaviour parity of the native Takaro layer with the 0.2.x TypeScript sidecar
// (sidecar/src/__tests__: null-args-matrix, sender-name, listBans-expiry, dragonwilds-mapping,
// events, actions, log-noise-ue, logTail, reconcile). Real NativeBehavior + NativePersistence +
// NativeLog + PluginState/bans; only the game-thread action handlers are stubbed.
// Every id below is a fake fixture value.
#include "native_behavior.h"
#include "native_log.h"
#include "actions.h"
#include "common.h"
#include "gamethread.h"
#include "reflect.h"
#include "state.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

using Json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
const std::string A = "0123456789abcdef0123456789abcdef";   // online player "Limon"
const std::string B = "aa11bb22cc33dd44ee55ff6600112233";   // second online player
const std::string U = "deadbeefdeadbeefdeadbeefdeadbeef";   // never seen
const std::string STEAM = "76561198000000001";

int checks = 0;
void Check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) throw std::runtime_error("FAILED: " + what);
}
Json Parse(const std::string& s) { return Json::parse(s); }
Json ToJson(const JsonValue& v) {
    switch (v.type) {
        case JsonValue::Object: {
            Json j = Json::object();
            for (const auto& kv : v.obj) j[kv.first] = ToJson(kv.second);
            return j;
        }
        case JsonValue::Array: {
            Json j = Json::array();
            for (const auto& x : v.arr) j.push_back(ToJson(x));
            return j;
        }
        case JsonValue::String: return v.str;
        case JsonValue::Number: return v.num;
        case JsonValue::Bool: return v.b;
        default: return nullptr;
    }
}

// ---- game stubs ------------------------------------------------------------------------------
std::map<std::string, Json> last;  // "POST /x" -> body
bool playersOnline = true;
bool locationDown = false;
bool unbanFails = false;
std::set<std::string> gameBans;    // the game's KnownPlayerList flags
std::string commandMode = "ok";
Json PlayersJson() {
    if (!playersOnline) return Json::array();
    return Json::array({Json{{"gameId", A}, {"name", "Hendrik"}, {"characterName", "Limon"}, {"steamId", STEAM},
                             {"epicOnlineServicesId", A}, {"platformId", "epic:" + A}, {"ping", 24}, {"online", true}},
                        Json{{"gameId", B}, {"name", "Guest"}, {"online", true}},
                        Json{{"gameId", "ffffffffffffffffffffffffffffffff"}, {"name", "Offline"}, {"online", false}}});
}
Actions::Result Ok(const char* key, const JsonValue& body) {
    last[key] = ToJson(body);
    return {200, R"({"success":true})"};
}
}  // namespace

bool GameThread::Alive() { return true; }
bool Reflect::Validated() { return true; }
namespace Actions {
Result Players() { return {200, PlayersJson().dump()}; }
Result Player(const std::string&) { return {404, R"({"error":"player not online"})"}; }
Result PlayerLocation(const std::string&) {
    return locationDown ? Result{503, R"({"error":"player has no pawn yet"})"} : Result{200, R"({"x":10.5,"y":20,"z":-3,"yaw":90})"};
}
Result PlayerInventory(const std::string&) {
    return {200, R"([{"code":"Item_Log_Oak","name":"Oak Logs","amount":4,"inventory":"Inv","slot":0},{"code":"Item_Rune_Air","name":"Air Rune","amount":12,"inventory":"Inv","slot":1}])"};
}
Result Items(const std::string&) { return {200, R"([{"code":"Item_Log_Oak","name":"Oak Logs","description":"wood","category":"x"}])"}; }
Result Entities() { return {200, R"([{"code":"AI_Goblin_Melee","name":"Goblin","type":"hostile","description":"AI"},{"code":"AI_Merchant","name":"Merchant","type":"friendly"}])"}; }
Result Locations() { return {200, R"([{"code":"lodestone_ashenfall","name":"Ashenfall Lodestone","position":{"x":0,"y":100,"z":0},"radius":50}])"}; }
Result Bans() {
    PluginState::Get().SetCapability("listBans", "ok");
    std::map<std::string, Json> rows;
    for (const auto& id : gameBans) rows[id] = Json{{"gameId", id}, {"name", ""}, {"reason", ""}, {"expiresAt", nullptr},
                                                   {"enforcedBy", "game"}, {"inGameList", true}, {"inPluginList", false}};
    for (const auto& b : state::BanList()) {
        Json row = {{"gameId", b.gameId}, {"name", b.name}, {"reason", b.reason},
                    {"expiresAt", b.expiresAt.empty() ? Json(nullptr) : Json(b.expiresAt)}, {"enforcedBy", "plugin"},
                    {"inGameList", gameBans.count(b.gameId) > 0}, {"inPluginList", true}};
        rows[b.gameId] = row;
    }
    Json out = Json::array();
    for (auto& kv : rows) out.push_back(kv.second);
    return {200, out.dump()};
}
Result Message(const JsonValue& b) { return Ok("POST /message", b); }
Result Teleport(const JsonValue& b) { return Ok("POST /teleport", b); }
Result Give(const JsonValue& b) { return Ok("POST /give", b); }
Result Kick(const JsonValue& b) { return Ok("POST /kick", b); }
Result Ban(const JsonValue& body) {
    Json j = ToJson(body);
    last["POST /ban"] = j;
    state::BanRecord r;
    r.gameId = j.value("gameId", "");
    r.reason = j.value("reason", "");
    r.expiresAt = j.value("expiresAt", "");
    if (!state::BanAdd(r) || !state::FlushBans()) return {503, R"({"error":"ban persistence"})"};
    gameBans.insert(r.gameId);
    return {200, Json{{"success", true}, {"gameId", r.gameId}}.dump()};
}
Result Unban(const JsonValue& body) {
    Json j = ToJson(body);
    last["POST /unban"] = j;
    std::string id = j.value("gameId", "");
    state::BanRemove(id);
    state::FlushBans();
    if (unbanFails) return {503, R"({"error":"unban failed: KnownPlayerList not writable"})"};
    gameBans.erase(id);
    return {200, Json{{"success", true}, {"gameId", id}}.dump()};
}
Result UnbanIfRevision(const JsonValue& body, uint64_t expected) {
    if (state::BanRevision() != expected) return {409, R"({"error":"ban changed before timed expiry; preserving current ban"})"};
    return Unban(body);
}
size_t PendingBanJobs() { return 0; }
Result Command(const JsonValue& body) {
    Json j = ToJson(body);
    last["POST /command"] = j;
    std::string cmd = j.value("command", "");
    if (cmd.rfind("cheat", 0) == 0)
        return {501, R"({"error":"unimplemented","capability":"executeCommand","detail":"cheat commands need a CheatManager"})"};
    if (cmd.rfind("flyToMoon", 0) == 0) return {400, R"({"error":"unknown command 'flyToMoon'. players | say <msg> | help"})"};
    if (cmd == "fail") return {200, R"({"success":false,"output":"unknown command"})"};
    return {200, Json{{"success", true}, {"output", "ran " + cmd}}.dump()};
}
Result Shutdown() { throw std::runtime_error("shutdown must be deferred until the response is written"); }
}  // namespace Actions

namespace {
struct Harness {
    std::unique_ptr<NativePersistence::Store> store;
    std::unique_ptr<NativeBehavior::Engine> engine;
    void Open() {
        engine.reset();
        store = std::make_unique<NativePersistence::Store>();
        Check(static_cast<bool>(store->Load()), "store load: " + store->LastError());
        engine = std::make_unique<NativeBehavior::Engine>(*store);
        auto loaded = engine->Load();
        Check(static_cast<bool>(loaded), "engine load: " + loaded.error);
    }
    // One full action as the bridge runs it: prepare, resolve + journal bans, execute, apply.
    NativeBehavior::ApplyResult Run(const std::string& action, const Json& args, const std::string& id = "r") {
        auto p = engine->PrepareAction(id, action, args.dump(), 1);
        if (NativeBehavior::Engine::NeedsBanResolution(p)) p.canonicalBanId = NativeBehavior::Engine::ResolveBanTarget(p);
        auto ready = engine->BeforeExecute(p);
        Check(static_cast<bool>(ready), action + " journal: " + ready.error);
        return engine->ApplyOutcome(p, NativeBehavior::Engine::ExecuteAction(p));
    }
    Json Ok(const std::string& action, const Json& args = Json::object()) {
        auto r = Run(action, args);
        Check(r.errorText.empty(), action + " " + args.dump() + " failed: " + r.errorText);
        return Parse(r.payloadJson);
    }
    std::string Err(const std::string& action, const Json& args = Json::object()) {
        auto r = Run(action, args);
        Check(!r.errorText.empty(), action + " " + args.dump() + " should fail, got " + r.payloadJson);
        return r.errorText;
    }
    Json Event(const Json& ring) {
        auto m = engine->MapEvent(ring.dump());
        Check(m.valid, "event mapping: " + m.error);
        return Parse(*m.frame)["payload"]["data"];
    }
};
const Json LIMON = {{"gameId", A}, {"name", "Limon"}, {"epicOnlineServicesId", A}, {"steamId", STEAM},
                    {"platformId", "epic:" + A}, {"ping", 24}};
const Json GUEST = {{"gameId", B}, {"name", "Guest"}, {"epicOnlineServicesId", B}, {"platformId", "epic:" + B}};
void Write(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }
}  // namespace

int main() {
    try {
        char tmp[] = "/tmp/dragonwilds-behavior-XXXXXX";
        const std::string dir = mkdtemp(tmp);
        setenv("TAKARO_PLUGIN_DATA_DIR", dir.c_str(), 1);
        setenv("TAKARO_STATE_DIR", dir.c_str(), 1);
        unsetenv("TAKARO_SENDER_NAME");
        unsetenv("TAKARO_SERVER_CHAT_NAME");
        unsetenv("TAKARO_SERVER_NAME");
        PluginState::Get().SetCapability("players", "ok");
        PluginState::Get().SetCapability("reflection", "ok");

        // ---- one-shot import of the 0.2.x sidecar state ------------------------------------------
        // The sidecar's real file shapes; its /ban call never carried expiresAt, so the plugin's
        // bans.json says permanent and the expiry lives only in timed-bans.json.
        Write(fs::path(dir) / "event-cursor.json", R"({"seq":452,"bootId":"0f0f0f0f0f0f0f0f"})");
        Write(fs::path(dir) / "known-players.json",
              Json::array({Json{{"gameId", U.substr(0, 31) + "0"}, {"name", "takarotester"},
                                {"epicOnlineServicesId", U.substr(0, 31) + "0"}, {"platformId", "epic:" + U.substr(0, 31) + "0"}}}).dump());
        Write(fs::path(dir) / "online-players.json", "[]");
        Write(fs::path(dir) / "timed-bans.json",
              Json::array({Json{{"gameId", B}, {"expiresAt", "2030-01-01T00:00:00.000Z"}, {"reason", "cooldown"}}}).dump());
        Write(fs::path(dir) / "bans.json",
              Json{{"version", 1}, {"bans", Json::array({Json{{"gameId", B}, {"name", "Guest"}, {"reason", ""},
                                                               {"createdAt", "2026-09-16T12:00:00.000Z"}, {"expiresAt", nullptr}}})}}.dump());
        gameBans.insert(B);
        state::BansLoad();
        Harness h;
        h.Open();
        Check(h.store->Current().scan.seq == 452 && h.store->Current().scan.bootId == "0f0f0f0f0f0f0f0f", "legacy cursor imported");
        Check(Parse(h.engine->Snapshot()->knownPlayersJson).size() == 1, "legacy known players imported");
        auto hydrated = state::ReadBans().records;
        Check(hydrated.size() == 1 && hydrated[0].expiresAt == "2030-01-01T00:00:00.000Z" && hydrated[0].reason == "cooldown",
              "legacy timed ban expiry hydrated into bans.json");
        Check(h.store->Current().legacyBanMigrationDone, "migration marked done in the outbox");

        // ---- sender name (sender-name.test.ts) ------------------------------------------------------
        auto sender = [&](const char* senderEnv, const char* chatEnv, const char* serverEnv, const Json& opts) {
            if (senderEnv) setenv("TAKARO_SENDER_NAME", senderEnv, 1); else unsetenv("TAKARO_SENDER_NAME");
            if (chatEnv) setenv("TAKARO_SERVER_CHAT_NAME", chatEnv, 1); else unsetenv("TAKARO_SERVER_CHAT_NAME");
            if (serverEnv) setenv("TAKARO_SERVER_NAME", serverEnv, 1); else unsetenv("TAKARO_SERVER_NAME");
            Harness x;
            x.Open();
            Json args = {{"message", "hi"}};
            if (!opts.is_null()) args["opts"] = opts;
            x.Ok("sendMessage", args);
            return last["POST /message"].value("senderName", "");
        };
        Check(sender("Sender", nullptr, "ServerName", {{"senderNameOverride", "Override"}}) == "Override", "override wins");
        Check(sender("Sender", nullptr, "ServerName", nullptr) == "Sender", "TAKARO_SENDER_NAME next");
        Check(sender("  Chat Bot  ", nullptr, nullptr, nullptr) == "Chat Bot", "sender name is trimmed");
        Check(sender(nullptr, "Alias", "ServerName", nullptr) == "Alias", "TAKARO_SERVER_CHAT_NAME alias");
        Check(sender(nullptr, nullptr, "ServerName", nullptr) == "ServerName", "server name fallback");
        Check(sender(nullptr, nullptr, nullptr, nullptr) == "Dragonwilds", "default server name");
        Check(sender("", nullptr, nullptr, {{"senderNameOverride", ""}}) == "Dragonwilds", "empty values fall through");
        unsetenv("TAKARO_SENDER_NAME");
        unsetenv("TAKARO_SERVER_NAME");
        h.Open();

        // ---- identity mapping (dragonwilds-mapping.test.ts) ----------------------------------------
        auto connected = [&](const Json& player) {
            return h.Event({{"seq", 1}, {"type", "player-connected"}, {"data", {{"player", player}}}})["player"];
        };
        Check(connected({{"gameId", A}, {"name", "Hendrik"}, {"characterName", "Limon"}}) ==
                  Json({{"gameId", A}, {"name", "Limon"}, {"epicOnlineServicesId", A}, {"platformId", "epic:" + A}}),
              "gameId = PUID, platformId epic:<puid>, name = character name");
        Check(connected({{"gameId", A}, {"name", "Hendrik"}})["name"] == "Hendrik", "name falls back to platform name");
        Check(connected({{"gameId", A}})["name"] == A, "then to the id");
        Check(connected({{"gameId", A}, {"steamId", STEAM}})["steamId"] == STEAM &&
                  connected({{"gameId", A}, {"steamId", STEAM}})["platformId"] == "epic:" + A,
              "SteamID64 kept alongside the PUID");
        Check(connected({{"epicOnlineServicesId", A}})["gameId"] == A && connected({{"platformId", "epic:" + A}})["gameId"] == A &&
                  connected({{"productUserId", A}})["gameId"] == A,
              "PUID from epicOnlineServicesId / platformId / productUserId");
        std::string upper = A;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        Check(connected({{"gameId", upper}})["gameId"] == A, "PUID normalised to lower case");
        Check(connected({{"gameId", STEAM}, {"name", "SteamOnly"}}) ==
                  Json({{"gameId", STEAM}, {"name", "SteamOnly"}, {"steamId", STEAM}, {"platformId", "steam:" + STEAM}}),
              "steam:<id64> only without a PUID");
        Check(connected({{"gameId", A}, {"ping", 42}, {"ip", "10.0.0.2"}})["ping"] == 42, "ping carried");
        Check(!connected({{"gameId", A}, {"ping", "nope"}}).contains("ping"), "non-numeric ping dropped");
        Check(connected({{"gameId", "Player_0"}, {"name", "X"}}) == Json({{"gameId", "Player_0"}, {"name", "X"}}), "degraded id usable");
        Check(!h.engine->MapEvent(Json{{"seq", 1}, {"type", "player-connected"}, {"data", {{"player", Json::object()}}}}.dump()).valid,
              "player without identifier rejected");

        // ---- event whitelist (events.test.ts + forbidNonWhitelisted) ------------------------------
        const Json pluginPlayer = {{"gameId", A}, {"name", "Hendrik"}, {"characterName", "Limon"}, {"platformName", "Hendrik"},
                                   {"characterGuid", "41C4B04F"}};
        const Json takaroPlayer = {{"gameId", A}, {"name", "Limon"}, {"epicOnlineServicesId", A}, {"platformId", "epic:" + A}};
        Json killed = h.Event({{"seq", 2}, {"type", "entity-killed"}, {"ts", "2026-09-17T10:00:00.000Z"},
                               {"data", {{"entity", "Goblin"}, {"entityCode", "BP_AI_Goblin_C"}, {"entityClass", "BP_AI_Goblin_C"},
                                         {"weapon", "Rune Sword"}, {"weaponCode", "Item_Sword_Rune"}, {"weaponSource", "x"},
                                         {"source", "BP_OnDeath"}, {"attribution", "the only player"}, {"player", pluginPlayer}}}});
        Check(killed == Json({{"player", takaroPlayer}, {"entity", "Goblin"}, {"weapon", "Rune Sword"},
                              {"timestamp", "2026-09-17T10:00:00.000Z"}}),
              "entity-killed carries exactly player/entity/weapon (+timestamp): " + killed.dump());
        Check(h.Event({{"seq", 3}, {"type", "entity-killed"}, {"data", {{"player", pluginPlayer}, {"entity", {{"code", "AI_Chicken"}}}}}}) ==
                  Json({{"player", takaroPlayer}, {"entity", "AI_Chicken"}, {"weapon", ""}}),
              "entity object code + empty weapon string");
        Check(h.Event({{"seq", 4}, {"type", "chat-message"}, {"data", {{"player", pluginPlayer}, {"message", "hi"}, {"channel", "Global"}, {"source", "x"}}}}) ==
                  Json({{"player", takaroPlayer}, {"msg", "hi"}, {"channel", "global"}}),
              "chat-message whitelisted");
        Check(h.Event({{"seq", 5}, {"type", "chat-message"}, {"data", {{"msg", "sys"}, {"channel", "team"}}}}) ==
                  Json({{"msg", "sys"}, {"channel", "team"}}), "player-less chat");
        Json death = h.Event({{"seq", 6}, {"type", "player-death"}, {"data", {{"player", pluginPlayer}, {"killerEntity", "AI_Goblin_Melee"},
                                                                              {"source", "telemetry"}}}});
        Check(death == Json({{"player", takaroPlayer}, {"msg", "Limon was killed by AI_Goblin_Melee"}}), "creature killer in msg");
        Check(!h.Event({{"seq", 7}, {"type", "player-death"}, {"data", {{"player", pluginPlayer}}}}).contains("msg"), "no msg without killer");
        Json attacker = {{"gameId", B}, {"characterName", "Bob"}};
        Check(h.Event({{"seq", 8}, {"type", "player-death"}, {"data", {{"player", pluginPlayer}, {"position", {{"x", 1}, {"y", 2}, {"z", 3}}}, {"attacker", attacker}}}}) ==
                  Json({{"player", takaroPlayer}, {"position", {{"x", 1}, {"y", 2}, {"z", 3}}},
                        {"attacker", {{"gameId", B}, {"name", "Bob"}, {"epicOnlineServicesId", B}, {"platformId", "epic:" + B}}}}),
              "player-death with attacker and position");
        Check(h.Event({{"seq", 9}, {"type", "log"}, {"data", {{"line", "LogDominion: saved"}, {"level", "info"}}}}) == Json({{"msg", "LogDominion: saved"}}),
              "log line key");
        Check(!h.engine->MapEvent(Json{{"seq", 10}, {"type", "player-sync"}, {"data", Json::object()}}.dump()).valid, "unknown type dropped");

        // ---- actions (actions.test.ts) ---------------------------------------------------------------
        Json reach = h.Ok("testReachability");
        Check(reach["connectable"] == true, "testReachability connectable");
        Check(h.Ok("getPlayers") == Json::array({LIMON, GUEST}), "getPlayers online only: " + h.Ok("getPlayers").dump());
        for (const Json& args : {Json{{"gameId", A}}, Json{{"player", {{"gameId", A}}}}, Json{{"gameId", "epic:" + A}},
                                 Json{{"steamId", STEAM}},
                                 Json{{"player", {{"gameId", A}, {"id", "pog-row"}, {"inventory", Json::array()}, {"dimension", nullptr}}}}})
            Check(h.Ok("getPlayer", args) == LIMON, "getPlayer " + args.dump());
        Json unknown = h.Ok("getPlayer", {{"gameId", U}});
        Check(unknown == Json({{"gameId", U}, {"name", U}, {"epicOnlineServicesId", U}, {"platformId", "epic:" + U}, {"online", false}}),
              "never-seen player: minimal valid offline IGamePlayer (F7)");
        Check(h.Err("getPlayer", Json::object()).find("player identifier") != std::string::npos, "no identifier is an error");
        playersOnline = false;
        Check(h.Ok("getPlayers").empty(), "nobody online");
        Json offline = h.Ok("getPlayer", {{"gameId", A}});
        Json wantOffline = LIMON;
        wantOffline["online"] = false;
        Check(offline == wantOffline, "left player: last-known record with online:false " + offline.dump());
        Check(h.Ok("getPlayer", {{"player", {{"steamId", STEAM}}}}) == wantOffline, "any identifier resolves the last-known record");
        h.Open();  // restart: the last-known record is persisted
        Check(h.Ok("getPlayer", {{"gameId", A}}) == wantOffline, "last-known record survives a restart");
        playersOnline = true;

        Check(h.Ok("getPlayerLocation", {{"player", {{"gameId", A}}}}) == Json({{"x", 10.5}, {"y", 20}, {"z", -3}}), "location DTO");
        locationDown = true;
        h.Err("getPlayerLocation", {{"gameId", A}});
        auto joinEvent = h.engine->MapEvent(Json{{"seq", 0}, {"type", "player-connected"}, {"data", {{"player", {{"gameId", A}}}}}}.dump());
        Check(static_cast<bool>(h.store->AdmitSynthetic(joinEvent.frame)) && static_cast<bool>(h.engine->ObserveAdmitted(joinEvent)), "admit join");
        Check(h.Ok("getPlayerLocation", {{"gameId", A}}) == Json({{"x", 10.5}, {"y", 20}, {"z", -3}}),
              "last real position answered inside the connect window");
        h.Err("getPlayerLocation", {{"gameId", B}});
        locationDown = false;

        Check(h.Ok("getPlayerInventory", {{"gameId", A}}) ==
                  Json::array({Json{{"code", "Item_Log_Oak"}, {"name", "Oak Logs"}, {"amount", 4}},
                               Json{{"code", "Item_Rune_Air"}, {"name", "Air Rune"}, {"amount", 12}}}),
              "inventory IItemDTO[]");
        Check(h.Ok("listItems") == Json::array({Json{{"code", "Item_Log_Oak"}, {"name", "Oak Logs"}, {"description", "wood"}}}), "listItems");
        Check(h.Ok("listEntities") == Json::array({Json{{"code", "AI_Goblin_Melee"}, {"name", "Goblin"}, {"type", "hostile"}, {"description", "AI"}},
                                                   Json{{"code", "AI_Merchant"}, {"name", "Merchant"}, {"type", "friendly"}}}),
              "listEntities");
        Check(h.Ok("listLocations") == Json::array({Json{{"code", "lodestone_ashenfall"}, {"name", "Ashenfall Lodestone"},
                                                         {"position", {{"x", 0}, {"y", 100}, {"z", 0}}}, {"radius", 50}}}),
              "listLocations");

        // ---- executeConsoleCommand: CommandOutput, never an error frame for a refused command (F1) ----
        Check(h.Ok("executeConsoleCommand", {{"command", "players"}}) ==
                  Json({{"success", true}, {"rawResult", "ran players"}, {"errorMessage", nullptr}}), "command output");
        Check(h.Ok("executeConsoleCommand", {{"command", "fail"}}) ==
                  Json({{"success", false}, {"rawResult", "unknown command"}, {"errorMessage", "unknown command"}}), "plugin success:false");
        Json unknownCommand = h.Ok("executeConsoleCommand", {{"command", "flyToMoon now"}});
        Check(unknownCommand["success"] == false && unknownCommand["rawResult"] == "" &&
                  unknownCommand["errorMessage"].get<std::string>().find("unknown command") != std::string::npos,
              "unknown command -> success:false (F1): " + unknownCommand.dump());
        Json cheat = h.Ok("executeConsoleCommand", {{"command", "cheat " + A + " god"}});
        Check(cheat["success"] == false && cheat["errorMessage"].get<std::string>().find("CheatManager") != std::string::npos,
              "unimplemented command -> success:false");
        Check(h.Err("executeConsoleCommand", Json::object()).find("'command'") != std::string::npos, "missing command");
        {
            auto p = h.engine->PrepareAction("s", "executeConsoleCommand", R"({"command":" Shutdown; "})", 1);
            auto o = NativeBehavior::Engine::ExecuteAction(p);
            Check(o.deferredShutdown && o.errorText.empty(), "console shutdown is answered first, then executed");
            auto q = h.engine->PrepareAction("s2", "shutdown", "[]", 1);
            Check(NativeBehavior::Engine::ExecuteAction(q).deferredShutdown, "shutdown action deferred");
        }
        Check(h.Err("flyToMoon").find("Unknown Takaro action") != std::string::npos, "unknown action");

        // ---- explicit null / wrong-type argument matrix (null-args-matrix.test.ts) ----------------
        const std::vector<Json> NULLISH = {Json(), Json(nullptr), Json(""), Json(0), Json(false), Json::array(), Json::object()};
        const std::vector<bool> absent = {true, false, false, false, false, false, false};
        auto with = [](Json base, const char* key, const Json& v, bool skip) {
            if (!skip) base[key] = v;
            return base;
        };
        for (size_t i = 0; i < NULLISH.size(); i++) {
            h.Ok("teleportPlayer", with({{"gameId", A}, {"x", 1}, {"y", 2}, {"z", 3}}, "dimension", NULLISH[i], absent[i]));
            Check(last["POST /teleport"] == Json({{"gameId", A}, {"x", 1}, {"y", 2}, {"z", 3}}), "teleport dimension " + NULLISH[i].dump());
        }
        h.Ok("teleportPlayer", {{"gameId", A}, {"x", 1}, {"y", 2}, {"z", 3}, {"yaw", nullptr}, {"target", nullptr}});
        Check(last["POST /teleport"] == Json({{"gameId", A}, {"x", 1}, {"y", 2}, {"z", 3}}), "nullish yaw/target omitted");
        h.Ok("teleportPlayer", {{"gameId", A}, {"x", "1"}, {"y", "2"}, {"z", "3"}});
        Check(last["POST /teleport"] == Json({{"gameId", A}, {"x", 1}, {"y", 2}, {"z", 3}}), "numeric strings");
        h.Ok("teleportPlayer", {{"gameId", A}, {"target", "lodestone_ashenfall"}});
        Check(last["POST /teleport"] == Json({{"gameId", A}, {"target", "lodestone_ashenfall"}}), "named target");
        Check(h.Err("teleportPlayer", {{"gameId", A}, {"x", nullptr}, {"y", nullptr}, {"z", nullptr}}).find("numeric") != std::string::npos,
              "missing coordinates: clean error");
        auto optional = [](const Json& v) -> std::string {
            if (v.is_number()) return v.dump();
            if (v.is_string()) return v.get<std::string>();
            return "";
        };
        for (size_t i = 0; i < NULLISH.size(); i++)
            for (size_t k = 0; k < NULLISH.size(); k++) {
                Json args = with(with({{"gameId", B}}, "reason", NULLISH[i], absent[i]), "expiresAt", NULLISH[k], absent[k]);
                h.Ok("banPlayer", args);
                Json body = last["POST /ban"];
                Check(body["gameId"] == B && !body.contains("expiresAt"), "nullish expiresAt is permanent " + args.dump());
                Check(body.value("reason", "") == optional(NULLISH[i]), "reason " + args.dump() + " -> " + body.dump());
                Check(Parse(h.engine->Snapshot()->timedBansJson).empty(), "no timed ban for " + args.dump());
            }
        for (size_t i = 0; i < NULLISH.size(); i++) {
            h.Ok("kickPlayer", with({{"gameId", A}}, "reason", NULLISH[i], absent[i]));
            Json body = last["POST /kick"];
            Check(optional(NULLISH[i]).empty() ? !body.contains("reason") : body["reason"] == optional(NULLISH[i]),
                  "kick reason " + NULLISH[i].dump());
        }
        for (size_t i = 0; i < NULLISH.size(); i++) {
            h.Ok("giveItem", with({{"gameId", A}, {"item", "Item_Log_Oak"}, {"amount", 2}}, "quality", NULLISH[i], absent[i]));
            Check(last["POST /give"] == Json({{"gameId", A}, {"code", "Item_Log_Oak"}, {"amount", 2}}), "give quality " + NULLISH[i].dump());
        }
        h.Ok("giveItem", {{"gameId", A}, {"item", "Item_Log_Oak"}, {"amount", nullptr}});
        Check(last["POST /give"]["amount"] == 1, "null amount defaults to 1");
        h.Ok("giveItem", {{"gameId", A}, {"item", {{"code", "Item_Log_Oak"}}}, {"amount", 3}});
        Check(last["POST /give"] == Json({{"gameId", A}, {"code", "Item_Log_Oak"}, {"amount", 3}}), "nested item object");
        h.Ok("giveItem", {{"gameId", A}, {"name", "Item_Log_Oak"}, {"amount", 1}});
        Check(last["POST /give"]["code"] == "Item_Log_Oak", "item given by name");
        Check(h.Err("giveItem", {{"gameId", A}, {"item", "Item_Log_Oak"}, {"amount", -1}}).find("positive") != std::string::npos, "negative amount");
        for (size_t i = 0; i < NULLISH.size(); i++) {
            h.Ok("sendMessage", with({{"message", "hi"}}, "opts", NULLISH[i], absent[i]));
            Check(last["POST /message"] == Json({{"text", "hi"}, {"senderName", "Dragonwilds"}}), "opts " + NULLISH[i].dump());
            h.Ok("sendMessage", {{"message", "hi"}, {"opts", with({{"senderNameOverride", nullptr}}, "recipient", NULLISH[i], absent[i])}});
            Check(last["POST /message"] == Json({{"text", "hi"}, {"senderName", "Dragonwilds"}}), "recipient " + NULLISH[i].dump());
        }
        h.Ok("sendMessage", {{"message", "Welcome"}, {"opts", {{"recipient", {{"gameId", "epic:" + A}}}, {"senderNameOverride", "Takaro"}}}});
        Check(last["POST /message"] == Json({{"text", "Welcome"}, {"recipientGameId", A}, {"senderName", "Takaro"}}), "directed message");
        Check(h.Err("sendMessage", Json::object()).find("'message'") != std::string::npos, "missing message");

        // ---- bans + timed expiry (listBans-expiry.test.ts) ------------------------------------------
        h.Ok("unbanPlayer", {{"gameId", B}});
        Check(state::ReadBans().records.empty() && gameBans.empty(), "unban clears both lists");
        const int64_t now = 1893456000000;  // 2030-01-01T00:00:00Z
        h.Ok("banPlayer", {{"player", {{"gameId", A}}}, {"reason", "timeout"}, {"expiresAt", "2030-01-01T00:10:00.000Z"}});
        Check(last["POST /ban"] == Json({{"gameId", A}, {"reason", "timeout"}, {"expiresAt", "2030-01-01T00:10:00.000Z"}}),
              "the plugin stores the expiry itself now");
        Json listed = h.Ok("listBans");
        Check(listed.size() == 1 && listed[0]["reason"] == "timeout" && listed[0]["expiresAt"] == "2030-01-01T00:10:00.000Z" &&
                  listed[0]["player"]["gameId"] == A && !listed[0]["player"].contains("inGameList"),
              "listBans reports expiresAt: " + listed.dump());
        h.Ok("banPlayer", {{"gameId", U}});
        listed = h.Ok("listBans");
        Check(listed.is_array() && listed.size() == 2, "offline never-seen id banned");
        for (const auto& row : listed)
            if (row["player"]["gameId"] == U)
                Check(row == Json({{"player", {{"gameId", U}, {"name", U}, {"epicOnlineServicesId", U}, {"platformId", "epic:" + U}}},
                                   {"reason", ""}, {"expiresAt", nullptr}}),
                      "permanent offline ban DTO " + row.dump());
        h.Ok("unbanPlayer", {{"gameId", U}});
        Check(h.engine->DueTimedBans(now).empty(), "not due yet");
        h.Open();  // restart before expiry: the schedule is durable
        Check(Parse(h.engine->Snapshot()->timedBansJson).size() == 1, "timed ban survives a restart");
        auto due = h.engine->DueTimedBans(now + 11 * 60 * 1000);
        Check(due.size() == 1 && due[0].internalExpiry && due[0].expiryPlayer == A, "due after expiry, lifted by the connector");
        Check(static_cast<bool>(h.engine->BeforeExecute(due[0])), "expiry journal");
        auto applied = h.engine->ApplyOutcome(due[0], NativeBehavior::Engine::ExecuteAction(due[0]));
        Check(applied.errorText.empty() && last["POST /unban"] == Json({{"gameId", A}}), "expiry calls the plugin unban");
        Check(state::ReadBans().records.empty() && Parse(h.engine->Snapshot()->timedBansJson).empty() && h.Ok("listBans").empty(),
              "expired ban gone from every list");
        Check(!h.engine->NeedsBanVerification(), "no intent left behind");
        // A failed expiry keeps the schedule and retries later.
        h.Ok("banPlayer", {{"gameId", A}, {"expiresAt", now - 1000}});
        unbanFails = true;
        due = h.engine->DueTimedBans(now);
        Check(due.size() == 1, "epoch-millis expiry due");
        Check(static_cast<bool>(h.engine->BeforeExecute(due[0])), "expiry journal (retry)");
        applied = h.engine->ApplyOutcome(due[0], NativeBehavior::Engine::ExecuteAction(due[0]));
        Check(!applied.errorText.empty() && Parse(h.engine->Snapshot()->timedBansJson).size() == 1, "failed unban keeps the expiry");
        unbanFails = false;
        h.Open();
        // The intent survives; verification finds the game flag still set and the bridge keeps retrying.
        Check(h.engine->NeedsBanVerification(), "unfinished expiry intent survives restart");
        due = h.engine->DueTimedBans(now);
        Check(due.size() == 1, "retried after restart");
        Check(static_cast<bool>(h.engine->BeforeExecute(due[0])), "expiry journal (after restart)");
        applied = h.engine->ApplyOutcome(due[0], NativeBehavior::Engine::ExecuteAction(due[0]));
        Check(applied.errorText.empty() && Parse(h.engine->Snapshot()->timedBansJson).empty() && !h.engine->NeedsBanVerification(),
              "retry after restart lifts it");
        // A newer permanent ban supersedes an older expiry and is never lifted by it.
        h.Ok("banPlayer", {{"gameId", A}, {"expiresAt", "2030-01-01T00:05:00.000Z"}});
        h.Ok("banPlayer", {{"gameId", A}, {"reason", "permanent now"}, {"expiresAt", nullptr}});
        Check(h.engine->DueTimedBans(now + 3600 * 1000).empty() && state::ReadBans().records.size() == 1, "permanent ban stays");
        h.Ok("unbanPlayer", {{"gameId", A}});

        // ---- reconcile after a crash (reconcile.test.ts) ----------------------------------------------
        {
            auto join = h.engine->MapEvent(Json{{"seq", 0}, {"type", "player-connected"}, {"data", {{"player", {{"gameId", A}, {"name", "Limon"}}}}}}.dump());
            Check(static_cast<bool>(h.store->AdmitSynthetic(join.frame)) && static_cast<bool>(h.engine->ObserveAdmitted(join)), "join admitted");
            Check(Parse(h.engine->Snapshot()->onlinePlayersJson).size() == 1, "online tracked");
            Check(h.engine->ReconcileOnline(PlayersJson().dump(), "boot").empty(), "still online: nothing sent");
            h.Open();  // the server process died with the player connected
            auto gone = h.engine->ReconcileOnline("[]", "boot-2");
            Check(gone.size() == 1 && gone[0].valid && gone[0].type == "player-disconnected" && gone[0].playerId == A,
                  "crash -> player-disconnected for the vanished player");
            Check(static_cast<bool>(h.store->AdmitSynthetic(gone[0].frame)) && static_cast<bool>(h.engine->ObserveAdmitted(gone[0])), "admit");
            Check(Parse(h.engine->Snapshot()->onlinePlayersJson).empty(), "online set empty after reconcile");
            locationDown = true;
            Check(h.Ok("getPlayerLocation", {{"gameId", A}}) == Json({{"x", 0}, {"y", 0}, {"z", 0}}),
                  "location lookup for the just-disconnected player answers the origin");
            locationDown = false;
        }

        // ---- log redaction, noise and grammar (log-noise-ue.test.ts, logTail.test.ts) -------------
        {
            NativeLog::Parser log;
            std::string key, detail;
            Check(log.Configure(key, detail), "log config " + key + detail);
            auto R = [&](const std::string& in, const std::string& want) { Check(log.Redact(in) == want, "redact '" + in + "' -> '" + log.Redact(in) + "'"); };
            R("LogDominion: WorldPassword=swordfish", "LogDominion: WorldPassword=[redacted]");
            R("AdminPassword = \"s3cr3t!\"", "AdminPassword = [redacted]");
            R("Settings: Password=abc, ServerName=Takaro", "Settings: Password=[redacted], ServerName=Takaro");
            R("[..]LogDom: ServerPassword: hunter2", "[..]LogDom: ServerPassword: [redacted]");
            R("WorldPassword=a AdminPassword=b", "WorldPassword=[redacted] AdminPassword=[redacted]");
            R("LogNet: Login request: ?p=cGFzc3dvcmQ=?pf=PC?cpx=1?Name=Limon userId: RedpointEOS:" + A + " platform: RedpointEOS",
              "LogNet: Login request: ?p=[redacted]?pf=PC?cpx=1?Name=Limon userId: RedpointEOS:" + A + " platform: RedpointEOS");
            R("LogNet: Join request: /Game/Maps/Server/L_ServerStartup?p=cGFzc3dvcmQ=?pf=PC?cpx=1?Name=Limon?SplitscreenCount=1",
              "LogNet: Join request: /Game/Maps/Server/L_ServerStartup?p=[redacted]?pf=PC?cpx=1?Name=Limon?SplitscreenCount=1");
            R("LogNet: Join request: /Game/Maps/World/L_World?p=c3dvcmRmaXNo", "LogNet: Join request: /Game/Maps/World/L_World?p=[redacted]");
            R("LogNet: Login request: ?p=?pf=PC", "LogNet: Login request: ?p=[redacted]?pf=PC");
            R("LogNet: Login request: ?p=one?pf=PC and again ?p=two", "LogNet: Login request: ?p=[redacted]?pf=PC and again ?p=[redacted]");
            R("the WorldPassword for this server is swordfish", "[redacted: line mentions a password]");
            R("LogNet: Login request: ?Name=X?Ticket=abc123?x=1", "LogNet: Login request: ?Name=X?Ticket=[redacted]?x=1");
            R("LogNet: Join succeeded: Limon", "LogNet: Join succeeded: Limon");
            R("", "");
            for (const char* noise : {"[2026.09.16-15.38.52:100][  0]LogRedpointEOS: Verbose: EOS_Platform_Tick",
                                      "[2026.09.16-15.38.52:100][  0]LogRedpointEOSCore: Verbose: cached 12 entries",
                                      "[2026.09.16-15.38.52:100][  0]LogEOSHTTP: POST https://api.epicgames.dev/... 200",
                                      "[2026.09.16-15.38.52:100][  0]LogEOSAnalytics: flushing 3 events",
                                      "[2026.09.16-15.38.52:100][  0]LogPlayerReporting: SendBackendEvent(PlayerJoined)", "", "   "})
                Check(log.Noise(noise), std::string("noise: ") + noise);
            for (const char* signal : {"[2026.09.16-15.38.52:100][  0]LogNet: Join succeeded: Limon",
                                       "[2026.09.16-15.38.52:100][  0]LogDominionSaveGame: World saved",
                                       "[2026.09.16-15.38.52:100][  0]LogRedpointEOS: Warning: connection lost"})
                Check(!log.Noise(signal), std::string("signal: ") + signal);
            // filtered mode: noise produces no event, a password line produces only a redacted one
            Check(log.Feed("[x]LogRedpointEOS: Verbose: tick").empty(), "noise never becomes a log event");
            auto out = log.Feed("LogDominion: WorldPassword=swordfish AdminPassword=hunter2");
            Check(out.size() == 1 && out[0].type == "log" && out[0].dataJson.find("swordfish") == std::string::npos &&
                      out[0].dataJson.find("hunter2") == std::string::npos,
                  "a cleartext password never leaves");
        }
        {
            setenv("DRAGONWILDS_LOG_TAIL", "always", 1);
            setenv("DRAGONWILDS_LOG_EVENTS", "none", 1);
            NativeLog::Parser log;
            std::string key, detail;
            Check(log.Configure(key, detail) && log.TailConnections(), "tail always");
            const std::string DCG = "41C4B04F4C9C038FEB767888BADD003F";
            std::vector<std::string> join = {
                "LogNet: NotifyAcceptingConnection accepted from: 192.168.129.15:58308",
                "LogNet: Login request: ?p=cGFzc3dvcmQ=?pf=PC?cpx=1?Name=Limon userId: RedpointEOS:" + A + " platform: RedpointEOS",
                "LogNet: Join succeeded: Limon",
                "LogDominionPlayerControllerBase: PlayerChar entered world [Account[XP:" + A + "] Character Name[takarotester] Guid[DCG:" + DCG + "] Type[0]]",
            };
            std::vector<NativeLog::Parsed> events;
            for (auto& l : join)
                for (auto& e : log.Feed(l)) events.push_back(e);
            Check(events.size() == 1 && events[0].type == "player-connected" &&
                      Parse(events[0].dataJson) == Json({{"player", {{"gameId", A}, {"name", "takarotester"}}}}),
                  "join only from `PlayerChar entered world`");
            Check(log.Feed(join[3]).empty(), "duplicate entered-world line ignored");
            auto leave = log.Feed("LogDominionPlayerController: ClientRequestDisconnect : DisconnectMe : PlayerStateSave result[true] - state saved for Account[XP:" +
                                  A + "] Character Name[takarotester] Guid[DCG:" + DCG + "] Type[0]");
            Check(leave.size() == 1 && leave[0].type == "player-disconnected", "clean leave");
            Check(log.Feed("LogNet: UChannel::CleanUp: ChIndex == 0. Closing connection. UniqueId: RedpointEOS:" + A).empty(),
                  "CleanUp after a leave is no duplicate");
            log.Feed(join[3]);
            auto drop = log.Feed("LogNet: UChannel::CleanUp: ChIndex == 0. UniqueId: RedpointEOS:" + A);
            Check(drop.size() == 1 && drop[0].type == "player-disconnected", "hard drop fallback");
            log.Feed("LogDominionPlayerControllerBase: PlayerChar entered world [Account[XP:" + B + "] Character Name[Sir Reginald III] Guid[DCG:BEEF] Type[0]]\r");
            auto nameOnly = log.Feed("LogDominionPlayerController: ClientRequestDisconnect : DisconnectMe : Character Name[Sir Reginald III]");
            Check(nameOnly.size() == 1 && Parse(nameOnly[0].dataJson)["player"]["gameId"] == B, "name-only leave, CRLF, spaces");
            Check(log.Feed("LogDominionPlayerController: ClientRequestDisconnect : DisconnectMe : Character Name[Ghost]").empty(),
                  "an unattributable leave is dropped, never guessed");
            Check(log.Feed("takarotester: takaro-l0b-175345").empty(), "chat is never sourced from the log");
            unsetenv("DRAGONWILDS_LOG_TAIL");
            unsetenv("DRAGONWILDS_LOG_EVENTS");
        }
        {
            setenv("DRAGONWILDS_LOG_TAIL", "sometimes", 1);
            NativeLog::Parser log;
            std::string key, detail;
            Check(!log.Configure(key, detail) && key == "DRAGONWILDS_LOG_TAIL", "bad tail mode rejected");
            unsetenv("DRAGONWILDS_LOG_TAIL");
            setenv("DRAGONWILDS_LOG_EVENTS", "most", 1);
            NativeLog::Parser log2;
            Check(!log2.Configure(key, detail) && key == "DRAGONWILDS_LOG_EVENTS", "bad events mode rejected");
            unsetenv("DRAGONWILDS_LOG_EVENTS");
        }
        // auto mode: the tail owns connections only while the plugin's players capability is degraded
        Check(!h.engine->SuppressRingConnection("player-connected"), "healthy: hooks own connections");
        PluginState::Get().SetCapability("players", "degraded", "test");
        Check(h.engine->SuppressRingConnection("player-connected") && !h.engine->SuppressRingConnection("chat-message"),
              "degraded: the log tail takes connections over");
        PluginState::Get().SetCapability("players", "ok");

        std::cout << "native behavior parity: " << checks << " checks passed" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (after " << checks << " checks)" << std::endl;
        return 1;
    }
}
