// Research probe for a Blueprint "Takaro SDK" that other server mods can call. Not a product
// feature: it does nothing unless TAKARO_CONAN_BLUEPRINT_PROBE names a directory, and it never talks
// to Takaro (a stub in games/conan-exiles/research/blueprint-probe/ does).
// Findings: docs/research/blueprint-sdk/conan-exiles.md.
//
// The question is which Blueprint -> native call paths the connector can see without a fourth
// pinned address. The probe has three parts:
//
// 1. Console route. A mod calls KismetSystemLibrary.ExecuteConsoleCommand("takaro <verb> ...").
//    No console command of that name exists, so the engine writes
//    "Command not recognized: takaro ..." to ConanSandbox.log. The probe tails the log on its own
//    worker thread (no game-thread cost on the connector side) and hands each request to the stub:
//      takaro run <steam64> <text...>        run a Takaro command as that player
//      takaro event <steam64|-> <name> <json...>  a custom event
//    The stub's replies come back as files and go to the player privately (targeted chat).
//
// 2. Stand-in driver. `drive/<name>.json` {"command": "...", "mode": "pc" | "pc-admin" | "none"}
//    makes the probe call ExecuteConsoleCommand on the game thread with exactly the arguments a
//    Blueprint node would pass (the player's controller with or without Conan's admin flag, or no
//    player). The result (game-thread ms, log lines of that frame) goes to drive/<name>.result.json.
//    It stands in for the Blueprint call until a cooked mod exists; it is not a Blueprint.
//
// 3. ProcessEvent oracle. Counting subscriptions (HookDispatch health shows hits and handler time)
//    on native library functions that Blueprints call directly, on native->Blueprint events, and on
//    the SDK mod's own Blueprint functions (inert until that mod is loaded).
//
// Files in the probe directory:
//   stub.json         stub -> connector heartbeat {"heartbeatMs"}; offline after 3 s
//   inbox/<id>.json   connector -> stub: one request
//   outbox/<id>.json  stub -> connector: {"replies":["text", ...]}; read once, then deleted
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace conan {
namespace bpprobe {

struct Request {
    bool ok = false;
    std::string verb;     // "run" or "event"
    std::string steam64;  // "" for an event without a player ("-")
    std::string text;     // run: the command text
    std::string name;     // event: the event name
    std::string json;     // event: the payload text as written (not validated here)
    std::string error;
};
// One server log line. ok=false with an empty error: not a probe line. ok=false with an error:
// a probe line that is malformed.
Request ParseLogLine(const std::string& line);

std::string RequestJson(const std::string& id, const Request& r, int64_t atUnixMs, uint64_t logLagMs);

bool Enabled();
void Start(const std::string& savedDir);
void Stop();
std::string HealthJson();

}  // namespace bpprobe
}  // namespace conan
