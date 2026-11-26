// Host harness: the production Takaro half (core/takaro bridge + outbox + config, the Conan adapter
// and coverage registry, and the libwebsockets transport) compiled from the same sources as the
// library, with only the game thread replaced: sendMessage goes to a fake chat that prints what it
// would have sent. tests/wire_test.py drives it against tests/fake_takaro.py.
//
// Usage: harness <wss-url> <ca-file> <state-dir> ready|refused
//        harness --registry | --pins     (print the compiled-in tables for tests/drift_test.py)
// env:    TAKARO_HARNESS_REGION   -> the loaded map the adapter reports (conan/map.h), default none
// stdin:  emit <type> <json-data>  -> queue a game event
//         region <name>            -> change the loaded map
//         health                   -> print "HEALTH <json>"
//         quit                     -> stop cleanly (outbox persisted) and exit 0
//         exit                     -> keep emitting log events and call exit(0) with the bridge still
//                                     running, as the engine does: the static destructors run, then
//                                     0.5 s pass (the engine's own teardown) before the process ends
// stdout: READY, CHAT <json>, HEALTH <json>, BYE
#include "common.h"
#include "conan/adapter.h"
#include "conan/coverage.h"
#include "pins/pins.h"
#include "takaro/bridge.h"
#include "takaro/config.h"
#include "takaro/json_util.h"
#include "takaro/outbox.h"
#include "transport_lws.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {
conan::Adapter* g_adapter = nullptr;  // for the atexit handler of the `exit` command
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--registry") {
        std::cout << conan::RegistryJson() << "\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--pins") {
        std::cout << pins::TableJson() << "\n";
        return 0;
    }
    if (argc != 5) {
        std::cerr << "usage: harness <wss-url> <ca-file> <state-dir> ready|refused\n";
        return 2;
    }
    SetNativeLogPath(std::string(argv[3]) + "/harness.log");
    std::mutex outMu;
    auto say = [&](const std::string& line) {
        std::lock_guard<std::mutex> g(outMu);
        std::cout << line << std::endl;
    };

    takaro::EnvFn env = [](const char* n) {
        const char* v = getenv(n);
        return v ? std::string(v) : std::string();
    };
    std::string fileText;
    takaro::Config cfg = takaro::LoadConfig(argv[3], env, fileText, false);
    cfg.url = argv[1];
    cfg.caFile = argv[2];
    cfg.stateDir = argv[3];
    if (!cfg.enabled) {
        std::cerr << "config: " << cfg.disabledReason << "\n";
        return 3;
    }

    conan::AdapterOptions ao;
    ao.version = "harness";
    ao.ready = std::string(argv[4]) == "ready";
    if (ao.ready) ao.build = "25639945";
    if (!ao.ready) ao.refusal = "unsupported server build (harness); this connector is pinned to build 25639945";
    ao.chat = [&](const conan::ChatRequest& r) {
        conan::ChatOutcome o;
        if (r.message.find("FAIL") != std::string::npos) {
            o.error = "No online Conan players are available for chat";
            return o;
        }
        o.success = true;
        o.online = 1;
        o.delivered = 1;
        say("CHAT " + takaro::ObjBuilder()
                          .S("message", r.message)
                          .S("recipient", r.recipient)
                          .S("sender", r.sender)
                          .Done());
        return o;
    };
    std::mutex regionMu;
    std::string region = env("TAKARO_HARNESS_REGION");
    ao.region = [&] {
        std::lock_guard<std::mutex> g(regionMu);
        return region;
    };
    conan::Adapter adapter(ao);
    takaro::Store store(cfg.stateDir);
    takaro::LwsConfig lc;
    lc.url = cfg.url;
    lc.caFile = cfg.caFile;
    lc.reconnectBaseMs = cfg.reconnectBaseMs;
    lc.reconnectMaxMs = cfg.reconnectMaxMs;
    takaro::LwsTransport transport(lc);
    takaro::BridgeOptions bo;
    bo.config = cfg;
    bo.game = &adapter;
    bo.transport = &transport;
    bo.store = &store;
    bo.healthFile = cfg.stateDir + "/health.json";
    takaro::Bridge bridge(bo);
    bridge.Start();
    // Registered first, so it runs after every static destructor registered later (atexit is LIFO):
    // the marker event and the events of the next 0.5 s are sent with those statics already gone.
    g_adapter = &adapter;
    std::atexit([] {
        takaro::GameEvent ev;
        ev.type = "log";
        takaro::ParseJson("{\"msg\":\"teardown started\"}", ev.data);
        g_adapter->Emit(std::move(ev));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    });
    say("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        FlushNativeLogs();
        if (line == "quit") break;
        if (line == "exit") {
            std::thread([&adapter] {
                for (int i = 0;; i++) {
                    takaro::GameEvent ev;
                    ev.type = "log";
                    takaro::ParseJson("{\"msg\":\"exiting " + std::to_string(i) + "\"}", ev.data);
                    adapter.Emit(std::move(ev));
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }).detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            FlushNativeLogs();
            std::exit(0);
        }
        if (line == "health") {
            say("HEALTH " + bridge.HealthJson());
            continue;
        }
        if (line.compare(0, 7, "region ") == 0) {
            std::lock_guard<std::mutex> g(regionMu);
            region = line.substr(7);
            continue;
        }
        if (line.compare(0, 5, "emit ") == 0) {
            size_t sp = line.find(' ', 5);
            takaro::GameEvent ev;
            ev.type = line.substr(5, sp == std::string::npos ? std::string::npos : sp - 5);
            if (sp == std::string::npos || !takaro::ParseJson(line.substr(sp + 1), ev.data)) {
                say("ERROR bad emit");
                continue;
            }
            adapter.Emit(std::move(ev));
            continue;
        }
        say("ERROR unknown command");
    }
    bridge.Stop();
    FlushNativeLogs();
    say("BYE");
    return 0;
}
