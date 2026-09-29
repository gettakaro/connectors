// Fallback join/leave source when the plugin's log-sink hook is degraded: tails
// enshrouded_server.log. A port of the former sidecar's sidecar/src/enshrouded/logTail.ts.
// The bridge thread drives it (Poll), so no file I/O happens on a game thread.
#pragma once
#include "common.h"

#include <map>
#include <string>
#include <vector>

namespace native {

struct LogJoinEvent {
    std::string type;  // player-connected | player-disconnected
    JsonValue data;    // {player: {gameId, name, steamId, platformId}}
};

// Stateful parser (research/log-events.md): SteamID only appears on `[online] Added peer P (steamid:X)`,
// the name only on `Player '<name>' logged in with Permissions` ~30 s later; they are correlated by machine
// index (peer "0(1)" <-> "Machine '1'") with a fallback to the oldest unnamed peer.
class LogParser {
public:
    std::vector<LogJoinEvent> Feed(const std::string& rawLine);

private:
    struct Peer {
        std::string peerId, machine, steamId, name;
        bool named = false;
        uint64_t order = 0;
    };
    std::vector<Peer*> Ordered();
    std::map<std::string, Peer> peers_;
    std::string lastMachine_;
    bool haveLastMachine_ = false;
    uint64_t order_ = 0;
};

// Polling tailer that starts at end of file and survives the server rotating the log (file shrinks or is replaced).
class LogTailer {
public:
    explicit LogTailer(std::string file, bool fromStart = false) : file_(std::move(file)), fromStart_(fromStart) {}
    void Start();  // next Poll re-anchors at EOF
    void Stop() { running_ = false; }
    bool Running() const { return running_; }
    std::vector<LogJoinEvent> Poll(std::string* error = nullptr);
    const std::string& File() const { return file_; }

private:
    std::string file_;
    bool fromStart_;
    bool running_ = false;
    int64_t offset_ = -1;
    std::string identity_;
    std::string partial_;
    LogParser parser_;
};

}  // namespace native
