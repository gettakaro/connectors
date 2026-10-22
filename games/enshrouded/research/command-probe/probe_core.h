// Research probe for gettakaro/connectors#202: can the Enshrouded server recognise a Takaro player command in
// chat, keep it out of the broadcast, and bind it to the sender's SteamID64? Not a production command framework:
// the catalog is a local stub file, the "receipt" is a local log line, nothing is sent to Takaro.
//
// Pure decision logic (no Windows, no game), so the host test links it directly. The game-side glue that feeds it
// lives in probe_glue.inc and is compiled only with -DTAKARO_COMMAND_PROBE.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace probe {

struct Command {
    std::string name;  // lower case
    std::vector<std::string> aliases;
    bool enabled = true;
    std::string permission;  // empty = everyone
};

struct Catalog {
    std::string revision;
    std::string prefix = "@";
    std::string nonce;             // the one magic probe trigger (claimed like a command name)
    uint64_t issuedAtMs = 0;       // wall clock, Unix ms
    uint64_t maxAgeMs = 0;         // 0 = never stale
    bool available = true;         // false = "Takaro unreachable before send"
    bool failDuringSend = false;   // true = the stub invocation fails after it was accepted
    std::vector<Command> commands;
    std::map<std::string, std::vector<std::string>> grants;  // steamId -> permissions
    // trigger (name or alias, lower case) -> command index; -1 when two commands claim it
    std::map<std::string, int> triggers;
};

// Parses the stub catalog file and runs the collision preflight: the nonce must be set, must not be a reserved
// name, and must not equal any command name or alias. Duplicate names/aliases are kept but marked ambiguous (-1)
// so they are still claimed (kept private) and rejected, never forwarded to two commands.
bool ParseCatalog(const std::string& text, Catalog& out, std::string& err);

// Names a Takaro deployment always owns, even if the catalog does not list them.
bool IsReserved(const std::string& lowerName);

// The sender, as the glue resolved it from the game's own data. `valid` only when every check passed.
struct Sender {
    bool valid = false;
    std::string reason;   // why not valid: out-of-range, stale-handle, not-logged-in, no-entity, ...
    std::string steamId;  // SteamID64 (decimal)
    int machine = -1;     // chat machine index of the sender, -1 unknown (no private reply possible)
};

enum class Kind {
    Pass,    // not ours: the game broadcasts it unchanged
    Invoke,  // claimed and accepted: suppress, run the stub invocation, reply privately
    Reject,  // claimed but refused: suppress, reply privately with the reason, no invocation
};

struct Decision {
    Kind kind = Kind::Pass;
    std::string trigger;  // the typed name (lower case)
    std::string command;  // catalog command name it resolved to
    std::string args;     // text after the name, leading spaces removed
    std::string reason;   // Pass: why ignored; Reject: why refused
};

// `text` is the exact chat payload the server received. It is trimmed the way the connector trims chat-message
// `msg` before Takaro matches its prefix, so anything Takaro would run as a command is claimed here too.
Decision Decide(const std::string& text, const Catalog* catalog, uint64_t nowMs, const Sender& sender);

// One-line, player-facing text for a decision (the private reply).
std::string ReplyText(const Decision& d, const std::string& invocationId, bool failed);

std::string Hex(const std::string& bytes);

}  // namespace probe
