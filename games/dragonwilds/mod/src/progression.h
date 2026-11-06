// Player progression: the plain data behind Takaro's getPlayerProgression.
//
// Takaro polls that action on every player sync, so the answer never touches the game thread: the
// housekeeping job copies each in-world player's skills into a Store, and the request path renders
// the copy. Nothing here knows a game type, so it is tested without a server.
#pragma once
#include "common.h"
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <strings.h>
#include <vector>

namespace Progression {

struct Track {
    std::string key;
    std::string name;
    int level = 0;
    int experience = 0;
    bool hasProgress = false;
    double progress = 0;
};

// A Takaro track key from the game's internal name: "SkillWoodcutting" -> "woodcutting",
// "Fire Making" -> "fireMaking". Empty when no usable letters remain. `character` is reserved for
// a primary level, which this game does not have, so a skill can never take that key.
inline std::string TrackKey(const std::string& internalName) {
    std::string s = internalName;
    if (s.size() > 5 && strncasecmp(s.c_str(), "skill", 5) == 0) s.erase(0, 5);
    std::string key;
    bool upper = false;
    for (unsigned char c : s) {
        if (!isalnum(c)) { upper = !key.empty(); continue; }
        if (key.empty() && !isalpha(c)) continue;
        key += key.empty() ? (char)tolower(c) : upper ? (char)toupper(c) : (char)c;
        upper = false;
    }
    if (key == "character") key = "skillCharacter";
    return key.size() > 128 ? key.substr(0, 128) : key;
}

// Share of the way from the current level to the next, 0..1. False at the top of the curve, where
// there is no next level to measure against.
inline bool Progress(int xp, int levelStartXp, int nextLevelXp, double& out) {
    if (nextLevelXp <= levelStartXp) return false;
    double share = (double(xp) - levelStartXp) / (double(nextLevelXp) - levelStartXp);
    out = share < 0 ? 0 : share > 1 ? 1 : share;
    return true;
}

// Takaro refuses a whole reading over one repeated key.
inline bool UniqueKeys(const std::vector<Track>& tracks) {
    std::set<std::string> keys;
    for (const Track& t : tracks)
        if (!keys.insert(t.key).second) return false;
    return true;
}

inline std::string TracksJson(const std::vector<Track>& tracks) {
    std::string o = "[";
    for (size_t i = 0; i < tracks.size(); i++) {
        const Track& t = tracks[i];
        if (i) o += ",";
        o += "{\"key\":" + JsonStr(t.key) + ",\"name\":" + JsonStr(t.name) + ",\"level\":" + std::to_string(t.level) +
             ",\"experience\":" + std::to_string(t.experience);
        if (t.hasProgress) o += ",\"progress\":" + JsonNum(t.progress);
        o += "}";
    }
    return o + "]";
}

// The last reading per player, tagged with the character it was read from. It outlives a
// disconnect, so a player who just left still answers their last known tracks, but it is never
// answered for any other character: that would be one character's levels on another.
class Store {
public:
    void Put(const std::string& gameId, const std::string& characterId, std::vector<Track> tracks) {
        std::lock_guard<std::mutex> g(lock_);
        rows_[gameId] = {characterId, std::move(tracks)};
    }
    bool Find(const std::string& gameId, const std::string& characterId, std::vector<Track>& out) const {
        std::lock_guard<std::mutex> g(lock_);
        auto it = rows_.find(gameId);
        if (it == rows_.end() || characterId.empty() || it->second.first != characterId) return false;
        out = it->second.second;
        return true;
    }

private:
    mutable std::mutex lock_;
    std::map<std::string, std::pair<std::string, std::vector<Track>>> rows_;
};

// The two server log lines that bound a character's session.
struct EnteredWorld {
    std::string gameId;
    std::string name;
    std::string characterId;  // the game's character GUID; empty if the line carries none
};

inline std::string Between(const std::string& line, const char* open, size_t from = 0) {
    size_t a = line.find(open, from);
    if (a == std::string::npos) return "";
    a += strlen(open);
    size_t end = line.find(']', a);
    return end == std::string::npos ? "" : line.substr(a, end - a);
}

inline bool IsHex(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!isxdigit(c)) return false;
    return true;
}

inline std::string LowerCopy(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// `PlayerChar entered world [Account[XP:<puid>] Character Name[<name>] Guid[DCG:<guid>] Type[0]]`:
// the character has loaded. The only place the server prints its name and GUID.
inline bool ParseEnteredWorld(const std::string& line, EnteredWorld& out) {
    size_t mark = line.find("PlayerChar entered world");
    if (mark == std::string::npos) return false;
    std::string id = Between(line, "Account[XP:", mark), name = Between(line, "Character Name[", mark);
    if (id.size() != 32 || !IsHex(id) || name.empty() || name.size() > 64) return false;
    std::string guid = Between(line, "Guid[DCG:", mark);
    out = {LowerCopy(id), name, IsHex(guid) ? guid : ""};
    return true;
}

// `LogNet: Login request: ... userId: RedpointEOS:<puid> platform: ...`: a new session starts, and
// which character it brings is unknown until it enters the world. Returns the puid, or "".
inline std::string ParseLoginRequest(const std::string& line) {
    if (line.find("Login request:") == std::string::npos) return "";
    static const char* kMark = "userId: RedpointEOS:";
    size_t a = line.find(kMark);
    if (a == std::string::npos) return "";
    std::string id = line.substr(a + strlen(kMark), 32);
    return id.size() == 32 && IsHex(id) ? LowerCopy(id) : "";
}

}  // namespace Progression
