// Research probe for silent namespaced commands (gettakaro/connectors #205). Not a product feature:
// it does nothing unless TAKARO_CONAN_COMMAND_PROBE names a directory, and it never talks to Takaro.
// Findings: docs/research/native-commands/conan-exiles.md. Stub: games/conan-exiles/research/command-probe/.
//
// A player's chat line that starts with the catalog prefix and names a catalog trigger (or alias) is
// vetoed in ConanPlayerController.ServerSendChatMessage (Before), so the game neither broadcasts nor
// logs it and no chat-message event is sent. The invocation goes to the stub as a file; the stub's
// replies come back as files and are sent to the caller only (ClientReceiveChatMessage, targeted).
//
// Files in the probe directory (catalog discovery and invocation are separate):
//   catalog.json      stub -> connector: {"revision","prefix","commands":[{"trigger","aliases":[]}]}
//   stub.json         stub -> connector heartbeat: {"heartbeatMs": <unix ms>}; offline after 3 s
//   inbox/<id>.json   connector -> stub: one invocation (never written while the stub is offline)
//   outbox/<id>.json  stub -> connector: {"replies":["text", ...]}; read once, then deleted
// Ids carry a per-boot random part, so a reply left from an earlier boot is never delivered.
// Offline or late (5 s) invocations fail privately at once and are never replayed.
#pragma once

#include "conan/events_payload.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace conan {
namespace cmdprobe {

struct Catalog {
    std::string revision;
    std::string prefix;                          // e.g. "/tk"; compared case-insensitively
    std::map<std::string, std::string> trigger;  // lower-case trigger or alias -> canonical trigger
    int duplicates = 0;                          // names listed more than once (first one wins)
};
// False (and `err`) on malformed JSON, a missing prefix or no commands.
bool ParseCatalog(const std::string& text, Catalog& out, std::string& err);

// Splits arguments on spaces; "double quotes" group words, \" is a literal quote.
std::vector<std::string> SplitArgs(const std::string& raw);

struct Classified {
    bool namespaced = false;  // starts with the prefix followed by a space or the end
    bool known = false;       // the first word is a catalog trigger or alias
    std::string word;         // as typed
    std::string trigger;      // canonical
    std::string argsRaw;
    std::vector<std::string> args;
};
Classified Classify(const Catalog& c, const std::string& message);

// Invocation file content (pure, for the host tests).
std::string InvocationJson(const std::string& id, const std::string& revision, const Classified& k,
                           const events::PlayerId& p, const std::string& channel, int64_t atUnixMs);

bool Enabled();
// Game thread, from the ServerSendChatMessage hook. True = claimed: veto the RPC, send no event.
// `identified` false still claims a known command (it must never go public) but cannot reply.
bool Offer(const events::PlayerId& p, bool identified, const std::string& channel, const std::string& message);

// Starts the worker when TAKARO_CONAN_COMMAND_PROBE is set (and subscribes the broadcast check).
void Start();
void Stop();
std::string HealthJson();

}  // namespace cmdprobe
}  // namespace conan
