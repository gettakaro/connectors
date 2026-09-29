// Pure parsing helpers for the VEIN server log (lane L2).
//
// Everything here is a free function over std::string with no process state, no game pointers and
// no UObject touch, so tests/unit_test.cpp exercises the real code rather than a copy of it.
// events.cpp owns the file handle, the rotation logic and the emitting; this file owns the grammar.
//
// Grammar source: context/games/vein/research/2026-09-17-log-grammar.md (lane L0b, from the
// vanilla client's own log during a local session - the *shapes* are the game's, the dedicated
// server still has to confirm them, see docs/events-design.md).
#pragma once
#include "common.h"

namespace EventsParse {

// One UE log line split into its parts. The first ~20 lines of a log file are written before
// LogTimes initialises and carry no `[timestamp][frame]` prefix, so `hasPrefix` may be false while
// the line is still perfectly good.
struct LogLine {
    bool hasPrefix = false;
    std::string timestamp;  // "2026.09.17-05.58.10:857", empty without a prefix
    std::string frame;      // "27", empty without a prefix
    std::string category;   // "LogVeinChat", empty when the line has no `Category: ` part
    std::string message;    // everything after "Category: "
};
LogLine SplitLogLine(const std::string& raw);

// `LogVeinChat: [<SteamID64>] <SteamPersona> (aka <CharacterName>): <message>`
// The `(aka …)` part is optional (a player who has not selected a character yet). The channel
// (Local / Global / Radio) is NOT on the line, so `channel` is left to the caller.
struct ChatLine {
    std::string gameId;        // SteamID64
    std::string platformName;  // Steam persona
    std::string characterName; // may be empty
    std::string msg;
    bool ok = false;
};
ChatLine ParseChatLine(const std::string& raw);

// `LogVein: [] Player <SteamPersona> selected character <32-hex> (aka <CharacterName>)`
struct CharacterSelectLine {
    std::string platformName;
    std::string characterId;    // 32 hex
    std::string characterName;
    bool ok = false;
};
CharacterSelectLine ParseCharacterSelectLine(const std::string& raw);

// The server's own join lines, which carry both the SteamID64 and the display name:
//   `LogNet: Login request: ?Password=<pw>?Name=<DisplayName>??ID=<SteamID64>?Ticket=<ticket>...`
//   `LogNet: Join request: /Game/...?Password=<pw>?Name=<DisplayName>??ID=<SteamID64>?...`
//   `LogVein: Player <SteamID64> (gamer-<32hex>) authenticated successfully.`
// This is the log-only fallback for player-connected: it works even when no vtable is hooked.
// NOTE: the raw line carries the join password and the Steam auth ticket in cleartext, so it is
// parsed *before* redaction and only the parsed fields ever leave this process.
struct JoinLine {
    std::string gameId;
    std::string name;   // Steam display name; empty on the `authenticated successfully` form
    bool ok = false;
};
JoinLine ParseJoinLine(const std::string& raw);

// `LogVein: Player <SteamID64> (gamer-<32hex>) failed to authenticate: <reason>` - the server
// refused the login (banned, wrong password, full, ...). It follows the `Login request:` line, so
// it cancels the log-only join fallback for that id.
struct LoginRefusedLine {
    std::string gameId;
    std::string reason;
    bool ok = false;
};
LoginRefusedLine ParseLoginRefusedLine(const std::string& raw);

// `LogVein: PlayerState ID changed to <SteamID64>`. The game emits an empty variant first; that one
// is rejected (ok=false), which is exactly why this is a function and not a substring test.
struct PlayerStateIdLine {
    std::string gameId;
    bool ok = false;
};
PlayerStateIdLine ParsePlayerStateIdLine(const std::string& raw);

// True for a 17-digit decimal SteamID64 starting with 7656119.
bool LooksLikeSteamId64(const std::string& s);

// Redaction on top of common.cpp's Redact(): the login/travel URL carries the Steam auth session
// ticket (`?Ticket=<base64>`) and the join URL the world password (`?p=<base64>`). Both are
// removed before a line can reach the ring buffer.
std::string RedactLogLine(const std::string& in);

// UE categories that are pure noise on a dedicated server; dropping them keeps the `log` capability
// from drowning the sidecar's poller.
bool IsNoise(const std::string& line);

// ---- lane L3e: chat channel ------------------------------------------------------------------
// VEIN's chat RPCs carry an `EChatSegment` (DWARF: `enum class EChatSegment : unsigned char
// { All=0, Local=1, Global=2, Radio=3 }`). L6b finding 3: the connector emitted a constant
// "global", so Takaro's `onlyGlobalChat` relayed local chat too. These two turn the raw byte into
// the display name (`chatSegment`) and the lower-case wire value (`channel`). Anything we do not
// recognise - including "the call carried no segment at all", -1 - stays "global", which is what
// every consumer defaulted to before this existed.
const char* ChatSegmentName(int v);   // "All"/"Local"/"Global"/"Radio", nullptr when unknown
const char* ChatChannelName(int v);   // "all"/"local"/"global"/"radio", "global" when unknown

}  // namespace EventsParse
