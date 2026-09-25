// Exercises the real NativeBridge/NativeTransport workers against a local TLS peer.
// Only VEIN game APIs are stubbed; no socket, JSON, or queue behavior is simulated.
#include "native_bridge.h"
#include "actions.h"
#include "gamethread.h"
#include "reflect.h"
#include "events.h"
#include "state.h"
#include "native_persistence.h"
#include <atomic>
#include <thread>
#include <map>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
std::mutex mu;
std::condition_variable cv;
bool released = false;
std::atomic<bool> holdPlayers{false};
std::atomic<unsigned> shutdownCalls{0};
std::atomic<size_t> pendingBanJobs{0};
std::atomic<bool> lateBanNext{false};
std::atomic<bool> holdBanNext{false};
std::atomic<bool> engineOnlyBanNext{false};
std::thread lateBanThread;
std::atomic<bool> pauseRename{false};
std::atomic<bool> failDirectorySync{false};
std::atomic<bool> failUnbanNext{false};
std::mutex gameBanMu;
std::map<std::string, std::string> gameBans;
void PersistGameBan(const std::string& id, bool add, const std::string& reason = {}) {
    std::lock_guard<std::mutex> lock(gameBanMu);
    if (add) gameBans[id] = reason; else gameBans.erase(id);
    std::string json = "[";
    for (const auto& value : gameBans)
        json += (json.size() > 1 ? "," : "") + std::string("{\"gameId\":") +
                JsonStr(value.first) + ",\"reason\":" + JsonStr(value.second) + "}";
    if (!WriteFileAtomic(PluginDataDir() + "/game-bans.fixture.json", json + "]"))
        throw std::runtime_error("fixture game ban write failed");
}
}

bool GameThread::Alive() { return true; }
bool Reflect::Validated() { return true; }
Actions::Result Actions::Players() {
    if (holdPlayers) {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [] { return released; });
    }
    return {200, R"([{"gameId":"76561198000000001","name":"Test player","characterName":"Character alias","online":true}])"};
}
Actions::Result Actions::PlayerLocation(const std::string& id) {
    if (id == "76561198000000003")
        return {200, "{\"x\":1,\"y\":2,\"z\":3,\"yaw\":90,\"ageMs\":0}"};
    return {501, "{\"error\":\"location unavailable\"}"};
}
Actions::Result Actions::Player(const std::string&) { return {404,"{}"}; }
Actions::Result Actions::PlayerInventory(const std::string&) { return {200,"[]"}; }
Actions::Result Actions::Items(const std::string&) { return {200,"[]"}; }
Actions::Result Actions::Entities() { return {200,"[]"}; }
Actions::Result Actions::Locations() { return {200,"[]"}; }
Actions::Result Actions::Bans() {
    PluginState::Get().SetCapability("listBans", "ok");
    std::map<std::string, std::string> ids;
    { std::lock_guard<std::mutex> lock(gameBanMu); ids = gameBans; }
    const auto gameIds = ids;
    for (const auto& ban : state::BanList()) ids[ban.gameId] = ban.reason;
    std::string json = "[";
    for (const auto& id : ids)
        json += (json.size() > 1 ? "," : "") + std::string("{\"gameId\":") +
                JsonStr(id.first) + ",\"reason\":" + JsonStr(id.second) +
                ",\"enforcedBy\":" + JsonStr(gameIds.count(id.first) ? "game" : "plugin") + "}";
    return {200, json + "]"};
}
Actions::Result Actions::Message(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Teleport(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Give(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Kick(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Ban(const JsonValue& body) {
    state::BanRecord ban;
    const auto* id = body.get("gameId");
    if (!id || !id->isStr()) return {400, R"({"error":"missing id"})"};
    ban.gameId = id->str;
    if (const auto* reason = body.get("reason"); reason && reason->isStr()) ban.reason = reason->str;
    if (const auto* expiry = body.get("expiresAt"); expiry && expiry->isStr()) ban.expiresAt = expiry->str;
    if (engineOnlyBanNext.exchange(false)) {
        ++pendingBanJobs;
        PersistGameBan(ban.gameId, true, ban.reason);
        if (!WriteFileAtomic(PluginDataDir() + "/engine-ban-written", "game saved; plugin state not saved"))
            throw std::runtime_error("cannot write engine crash marker");
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [] { return released; });
        --pendingBanJobs;
    }
    if (holdBanNext.exchange(false)) {
        ++pendingBanJobs;
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [] { return released; });
        --pendingBanJobs;
    }
    if (lateBanNext.exchange(false)) {
        ++pendingBanJobs;
        // Simulate a started game job that changed enforcement memory but is
        // still inside the engine when the caller's timeout expires.
        state::BanAdd(ban);
        PersistGameBan(ban.gameId, true, ban.reason);
        lateBanThread = std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            state::FlushBans();
            --pendingBanJobs;
        });
        return {504, R"({"error":"game-thread job timed out"})"};
    }
    if (!state::BanAdd(ban) || !state::FlushBans()) return {503, R"({"error":"ban persistence failed"})"};
    PersistGameBan(ban.gameId, true, ban.reason);
    return {200,"{}"};
}
size_t Actions::PendingBanJobs() { return pendingBanJobs.load(); }
Actions::Result Actions::Unban(const JsonValue& body) {
    const auto* id = body.get("gameId");
    if (!id || !id->isStr()) return {400, R"({"error":"missing id"})"};
    state::BanRemove(id->str);
    if (failUnbanNext.exchange(false)) {
        state::FlushBans();
        return {409, R"({"error":"game ban still listed after failed engine write"})"};
    }
    PersistGameBan(id->str, false);
    return state::FlushBans() ? Actions::Result{200,"{}"} : Actions::Result{503, R"({"error":"ban persistence failed"})"};
}
Actions::Result Actions::Command(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::UnbanIfRevision(const JsonValue& body, uint64_t expectedRevision) {
    if (state::BanRevision() != expectedRevision)
        return {409, R"({"error":"ban changed before timed expiry; preserving current ban"})"};
    return Unban(body);
}
Actions::Result Actions::Shutdown() { ++shutdownCalls; return {200,"{}"}; }
void Events::SetRawLogSink(std::function<void(std::string)>, bool, bool) {}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    unsetenv("TAKARO_NATIVE_GATE");
    unsetenv("TAKARO_NATIVE_DISABLE");
    setenv("TAKARO_WS_URL", argv[1], 1);
    setenv("TAKARO_CA_FILE", argv[2], 1);
    setenv("TAKARO_PLUGIN_DATA_DIR", argv[3], 1);
    setenv("TAKARO_STATE_DIR", argv[3], 1);
    setenv("TAKARO_IDENTITY_TOKEN", "test-identity", 1);
    setenv("TAKARO_REGISTRATION_TOKEN", "", 1);
    const std::string marker = std::string(argv[3]) + "/before-rename";
    NativePersistence::TestBeforeRename([marker](const std::string& path) {
        if (path.find("event-outbox.json") == std::string::npos || !pauseRename.exchange(false)) return;
        if (!WriteFileAtomic(marker, "temp fsynced; rename pending"))
            throw std::runtime_error("cannot write test synchronization marker");
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [] { return released; });
    });
    NativePersistence::TestDirectorySyncFailure([](const std::string& path) {
        return path.find("event-outbox.json") != std::string::npos && failDirectorySync.load();
    });
    state::BansLoad();
    std::string gameBanData;
    JsonValue gameBanRows;
    if (ReadFile(PluginDataDir() + "/game-bans.fixture.json", gameBanData) &&
        JsonParse(gameBanData, gameBanRows) && gameBanRows.type == JsonValue::Array)
        for (const auto& row : gameBanRows.arr) {
            const auto* id = row.get("gameId");
            const auto* reason = row.get("reason");
            if (id && id->isStr()) gameBans[id->str] = reason && reason->isStr() ? reason->str : "";
        }
    PluginState::Get().SetCapability("gameThread", "ok");
    if (!NativeBridge::Start()) {
        std::cout << "FAILED " << NativeBridge::HealthJson() << std::endl;
        return 3;
    }
    std::cout << "READY" << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "health") std::cout << "HEALTH " << NativeBridge::HealthJson() << std::endl;
        else if (command == "hold") { holdPlayers = true; std::cout << "HELD" << std::endl; }
        else if (command == "lateban") { lateBanNext = true; std::cout << "LATEBAN" << std::endl; }
        else if (command == "holdban") { holdBanNext = true; std::cout << "HOLDBAN" << std::endl; }
        else if (command == "engine-only-ban") { engineOnlyBanNext = true; std::cout << "ENGINEONLY" << std::endl; }
        else if (command == "external-permanent") {
            JsonValue body;
            JsonParse(R"({"gameId":"76561198000000001","reason":"external newer permanent"})", body);
            auto result = Actions::Ban(body);
            std::cout << "EXTERNAL " << result.status << std::endl;
        }
        else if (command.rfind("external-timed:", 0) == 0) {
            JsonValue body;
            JsonParse("{\"gameId\":\"76561198000000001\",\"reason\":\"external newer timed\",\"expiresAt\":" +
                      JsonStr(command.substr(15)) + "}", body);
            auto result = Actions::Ban(body);
            std::cout << "EXTERNAL " << result.status << std::endl;
        }
        else if (command == "arm-write") { pauseRename = true; std::cout << "ARMED" << std::endl; }
        else if (command == "fail-dir-sync") { failDirectorySync = true; std::cout << "SYNCFAILED" << std::endl; }
        else if (command == "restore-dir-sync") { failDirectorySync = false; std::cout << "SYNCRESTORED" << std::endl; }
        else if (command == "fail-unban") { failUnbanNext = true; std::cout << "FAILUNBAN" << std::endl; }
        else if (command == "release") {
            { std::lock_guard<std::mutex> lock(mu); released = true; }
            cv.notify_all();
            std::cout << "RELEASED" << std::endl;
        } else if (command == "shutdowns") std::cout << "SHUTDOWNS " << shutdownCalls.load() << std::endl;
        else if (command.rfind("emit:", 0) == 0) {
            PluginState::Get().EmitEvent("chat-message", "{\"msg\":" + JsonStr(command.substr(5)) + ",\"channel\":\"global\"}");
            std::cout << "EMITTED" << std::endl;
        } else if (command == "quit") break;
    }
    { std::lock_guard<std::mutex> lock(mu); released = true; }
    cv.notify_all();
    if (lateBanThread.joinable()) lateBanThread.join();
    NativeBridge::Stop();
    return 0;
}
