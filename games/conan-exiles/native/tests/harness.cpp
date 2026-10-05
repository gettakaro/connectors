// Host harness: the production Takaro half (core/takaro bridge + outbox + config, the Conan adapter
// and coverage registry, and the libwebsockets transport) compiled from the same sources as the
// library, with only the game thread replaced: sendMessage goes to a fake chat that prints what it
// would have sent. tests/wire_test.py drives it against tests/fake_takaro.py.
//
// Usage: harness <wss-url> <ca-file> <state-dir> ready|refused
//        harness --registry | --pins     (print the compiled-in tables for tests/drift_test.py)
// stdin:  emit <type> <json-data>  -> queue a game event
//         health                   -> print "HEALTH <json>"
//         quit                     -> stop cleanly (outbox persisted) and exit 0
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

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

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
    say("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        FlushNativeLogs();
        if (line == "quit") break;
        if (line == "health") {
            say("HEALTH " + bridge.HealthJson());
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
