// The connector's own ban list (lane L2b). Conan's engine blacklist cannot hold an offline or a
// timed ban (spike S2: no reflected Ban/Unban, BanPlayer is admin-gated and needs a live net id,
// and AServerBlacklist has no expiry), so bans live here, keyed by Steam64, and are enforced by
// kicking a banned player at login (K2_PostLogin hook) and in a sweep over the online players.
//
// Durable: <Saved>/Config/Takaro/bans.json, rewritten atomically (tmp + fsync + rename) on every
// change. Pure logic plus file I/O through core/takaro/fileio.h; no game access, so host tests
// cover it directly.
#pragma once

#include "common.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace conan {

struct Ban {
    std::string gameId;  // Steam64
    std::string name;    // last known player name (Takaro's BanDTO needs one)
    std::string reason;
    int64_t expiresAtMs = 0;  // 0 = permanent
    int64_t createdAtMs = 0;
};

class BanList {
public:
    explicit BanList(std::string path);
    // Loads the file. A missing file is an empty list. A corrupt file leaves the list empty and
    // marks the store broken: every change is then refused (never silently drop bans).
    void Load();
    const std::string& path() const { return path_; }
    std::string Error() const;  // non-empty when the store is broken

    // Adds or replaces the ban of b.gameId and saves. False (list unchanged) when saving fails.
    bool Upsert(const Ban& b, std::string& error);
    // Removes and saves; `removed` says whether there was one.
    bool Remove(const std::string& gameId, bool& removed, std::string& error);
    // Drops bans whose expiry is <= nowMs and saves; returns the removed entries.
    std::vector<Ban> Expire(int64_t nowMs, std::string& error);
    // Active bans (expired ones filtered out even before Expire() runs).
    std::vector<Ban> Active(int64_t nowMs) const;
    bool Find(const std::string& gameId, int64_t nowMs, Ban& out) const;
    size_t Size() const;

    static std::string Serialize(const std::vector<Ban>& bans);
    static bool Parse(const std::string& text, std::vector<Ban>& out, std::string& error);

private:
    bool SaveLocked(const std::vector<Ban>& next, std::string& error);
    std::string path_;
    mutable std::mutex mu_;
    std::vector<Ban> bans_;
    std::string error_;
};

// Takaro's BanDTO for listBans: { player: { gameId, name, steamId, platformId }, reason, expiresAt }.
JsonValue BanToJson(const Ban& b);

}  // namespace conan
