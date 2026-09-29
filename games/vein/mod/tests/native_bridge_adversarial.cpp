// Exercises the real NativeBridge/NativeTransport workers against a local TLS peer.
// Only VEIN game APIs are stubbed; no socket, JSON, or queue behavior is simulated.
#include "native_bridge.h"
#include "native_transport.h"
#include "actions.h"
#include "gamethread.h"
#include "reflect.h"
#include "events.h"

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
}

bool GameThread::Alive() { return true; }
bool Reflect::Validated() { return true; }
Actions::Result Actions::Players() { return {200, "[]"}; }
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
Actions::Result Actions::Bans() { return {200,"[]"}; }
Actions::Result Actions::Message(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Teleport(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Give(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Kick(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Ban(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Unban(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::UnbanIfRevision(const JsonValue& body, uint64_t) { return Actions::Unban(body); }
Actions::Result Actions::Command(const JsonValue&) { return {200,"{}"}; }
Actions::Result Actions::Shutdown() { return {200,"{}"}; }
size_t Actions::PendingBanJobs() { return 0; }
void Events::SetRawLogSink(std::function<void(std::string)>, bool, bool) {}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    setenv("TAKARO_NATIVE_GATE", "1", 1);
    setenv("TAKARO_WS_URL", argv[1], 1);
    setenv("TAKARO_CA_FILE", argv[2], 1);
    setenv("TAKARO_IDENTITY_TOKEN", "test-identity", 1);
    setenv("TAKARO_REGISTRATION_TOKEN", "", 1);
    if (!getenv("TAKARO_SERVER_NAME")) setenv("TAKARO_SERVER_NAME", "NativeBridge test", 1);
    if (argc == 3) NativeBridge::SetActionHandler([](const std::string& action, const std::string& args) {
        if (action == "echo") return args;
        if (action == "throw") throw std::runtime_error("intentional handler failure");
        if (action == "badjson") return std::string("not json");
        if (action == "huge") return std::string("\"") + std::string(8 * 1024 * 1024 + 1, 'x') + "\"";
        if (action == "nearhuge") return std::string("\"") + std::string(7900 * 1024, 'x') + "\"";
        if (action == "fetchGamePlayers") {
            auto body = NativeTransport::FetchGamePlayers();
            return body ? *body : std::string("null");
        }
        if (action == "hold") {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [] { return released; });
            return std::string("{\"held\":true}");
        }
        throw std::runtime_error("unknown test action");
        return std::string();
    });
    if (!NativeBridge::Start()) return 3;
    std::cout << "READY" << std::endl;
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "health") std::cout << "HEALTH " << NativeBridge::HealthJson() << std::endl;
        else if (command == "pause") {
            NativeBridge::TestPauseCompletions(true);
            std::cout << "PAUSED" << std::endl;
        } else if (command == "unpause") {
            NativeBridge::TestPauseCompletions(false);
            std::cout << "UNPAUSED" << std::endl;
        }
        else if (command == "release") {
            { std::lock_guard<std::mutex> lock(mu); released = true; }
            cv.notify_all();
            std::cout << "RELEASED" << std::endl;
        } else if (command.rfind("window:", 0) == 0) {
            NativeBridge::TestNoteLocationWindow(command.substr(7));
            std::cout << "WINDOW" << std::endl;
        } else if (command == "quit") break;
    }
    { std::lock_guard<std::mutex> lock(mu); released = true; }
    cv.notify_all();
    NativeBridge::Stop();
    return 0;
}
