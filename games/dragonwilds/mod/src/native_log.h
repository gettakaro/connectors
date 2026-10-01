// RSDragonwilds.log grammar, redaction and `log` event filtering for the native bridge.
//
// Runs on the bridge worker only (PCRE2, never the game thread). Fed every complete line by the
// plugin's background log tailer (Events::SetRawLogSink). Produces:
//   * `log` events: every line that is not Unreal/EOS noise (DRAGONWILDS_LOG_EVENTS=filtered, the
//     default), every line (all) or none (none) - always redacted first;
//   * `player-connected` / `player-disconnected` from the join/leave grammar, but only while the
//     log tail owns connections (DRAGONWILDS_LOG_TAIL=always, or auto while the plugin's `players`
//     capability is degraded). Otherwise the plugin's PostLogin/OnNetCleanup hooks own them.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace NativeLog {
struct Parsed {
    std::string type, dataJson;
};
class Parser {
public:
    Parser();
    ~Parser();
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    // Compiles the grammar and reads DRAGONWILDS_LOG_TAIL / DRAGONWILDS_LOG_EVENTS. On failure
    // `errorKey` names the offending setting.
    bool Configure(std::string& errorKey, std::string& detail);
    std::vector<Parsed> Feed(const std::string& rawLine);
    bool TailConnections() const;
    std::string LastError() const;
    // The world password is printed in cleartext (WorldPassword=..., and base64 as ?p=<...> in
    // Login/Join request URLs). Any *Password / *Token / *Ticket assignment is masked; a line that
    // mentions a password in any other shape is dropped wholesale.
    std::string Redact(const std::string& rawLine) const;
    bool Noise(const std::string& line) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace NativeLog
