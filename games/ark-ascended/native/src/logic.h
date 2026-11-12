// Rules and durable state that need no game object, kept apart from plugin.cpp so they can be tested on their own.
#pragma once
#include "tj_json.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace takaro {

// ISO 8601 with a Z or numeric offset, optional fraction, to Unix seconds.
bool ParseIso8601(const std::string& text, int64_t& epochSeconds);

// Empty when the text can go to the game console, else why not.
std::string ConsoleRefusal(const std::string& command);

// True for lines that are safe to forward as a log event: no credentials, addresses or command lines.
bool LogLineAllowed(const std::string& line);

struct CatalogRow {
    std::string code, name;
};
tj::Value CatalogJson(const std::vector<CatalogRow>& rows);
// Case-insensitive substring match on code or name.
tj::Value CatalogSearch(const std::vector<CatalogRow>& rows, const std::string& term);

// The item a giveItem names, whichever field Takaro used. Empty when it is not a /Game class path.
std::string ItemCodeOf(const tj::Value& args);
// The catalogue code for an item given by its display name (case-insensitive), as Takaro's own give and many modules
// send it. Empty when no item or more than one item has that name; `matches` tells which.
std::string ItemCodeByName(const tj::Value& args, const std::vector<CatalogRow>& rows, size_t& matches);

// Dino costumes the account owns sit in the inventory list with no flag marking them, but the game shows them on
// the Cosmetics tab, not as something the player carries.
bool IsCosmeticCode(const std::string& code);

// False for the origin, which is what the game answers once the pawn is being torn down on a disconnect.
bool IsRealPosition(double x, double y, double z);

// The survivor's level as the game shows it. XP can pass the next threshold without changing it: the level rises when
// the player spends the point.
struct ProgressReading {
    int level = 0;
    double experience = 0;
    std::optional<double> progress;
};
// Empty when the game gave values that cannot be a real reading.
std::optional<ProgressReading> MakeProgressReading(int level, double experience, double previous, double next);
// Fraction of the way between the thresholds. Empty at the level cap, where they are equal.
std::optional<double> ProgressBetween(double experience, double previous, double next);
// Null for a player the game does not know, [] for a known player with no reading yet.
tj::Value ProgressionJson(bool knownPlayer, const std::optional<ProgressReading>& reading);

// One survivor. PlayerDataID stays through deaths and restarts; Create New Survivor replaces it.
struct Survivor {
    std::string id, name;
};
// Empty when the game has no id for the player yet. The name falls back on the account name.
std::optional<Survivor> MakeSurvivor(uint64_t playerDataId, const std::string& name, const std::string& accountName);
tj::Value SurvivorJson(const Survivor& survivor);
// True when the player was already playing another survivor.
bool IsSurvivorSwitch(const std::optional<Survivor>& known, const Survivor& seen);

// The game's BanList.txt: one "id,name,minutes" line per ban.
struct NativeBan {
    std::string id, name;
};
std::vector<NativeBan> ParseBanList(const std::string& text);

// Writes through a temporary file so a crash never leaves half a file.
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& text);

struct Ban {
    std::string reason, expiresAt;  // expiresAt empty = permanent
    int64_t expiresEpoch = 0;
};

class BanStore {
public:
    explicit BanStore(std::filesystem::path file = {});
    // False when the ban could not be written; the store is then unchanged.
    bool Set(const std::string& id, Ban ban);
    void Erase(const std::string& id);
    std::map<std::string, Ban> Rows() const;
    std::vector<std::string> Expired(int64_t now) const;

private:
    bool Save() const;
    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::string, Ban> rows_;
};

// Who has been seen, so getPlayer can answer for a player who is offline.
class KnownPlayers {
public:
    explicit KnownPlayers(std::filesystem::path file = {});
    // Returns true when this changed what is stored.
    bool Remember(const std::string& id, const std::string& name);
    std::string Name(const std::string& id) const;

private:
    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::string, std::string> names_;
};

}  // namespace takaro
