// Player progression: the plain data behind Takaro's getPlayerProgression.
//
// Takaro polls that action on every player sync, so the answer never touches the game thread: the
// housekeeping job copies each in-world player's skills into a Store, and the request path renders
// the copy. Nothing here knows a game type, so it is tested without a server.
#pragma once
#include "common.h"
#include <cctype>
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

// The last reading per player. It outlives a disconnect, so a player who just left still answers
// their last known tracks.
class Store {
public:
    void Put(const std::string& gameId, std::vector<Track> tracks) {
        std::lock_guard<std::mutex> g(lock_);
        rows_[gameId] = std::move(tracks);
    }
    bool Find(const std::string& gameId, std::vector<Track>& out) const {
        std::lock_guard<std::mutex> g(lock_);
        auto it = rows_.find(gameId);
        if (it == rows_.end()) return false;
        out = it->second;
        return true;
    }

private:
    mutable std::mutex lock_;
    std::map<std::string, std::vector<Track>> rows_;
};

}  // namespace Progression
