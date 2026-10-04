// Pure helpers for the Takaro game events (no game, no I/O), so the unit tests cover them:
// the Takaro DTO shapes with a strict key whitelist, chat channel mapping, death cause and weapon
// naming, and the log tail's line splitting, redaction and rate limit.
//
// Takaro validates inbound gameEvents with forbidNonWhitelisted: one extra key drops the whole
// event with only an uncorrelated error frame. Every event leaves through Sanitize().
#pragma once

#include "common.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace conan {
namespace events {

struct PlayerId {
    std::string steam64;  // gameId
    std::string name;     // PlayerState.PlayerNamePrivate (the platform name, as getPlayers reports it)
    std::string ip;       // PlayerState.SavedNetworkAddress, optional
    bool Valid() const;   // a 17-digit Steam64 starting 7656
};

// { gameId, name, steamId, platformId: "steam:<id>", ip? }; name falls back to the Steam64.
JsonValue PlayerJson(const PlayerId& p);

// Conan chat channel -> Takaro ChatChannel. Takaro has no local channel: Global and Local map to
// "global", Clan to "team", anything else to "global".
std::string MapChannel(const std::string& conanChannel);

// Each returns an Object, or Null when a required field is missing (the event is then not sent).
JsonValue ConnectedPayload(const PlayerId& p);  // player-connected and player-disconnected
JsonValue ChatPayload(const PlayerId& p, const std::string& conanChannel, const std::string& msg);
struct Position {
    bool has = false;
    double x = 0, y = 0, z = 0;
};
// attacker only when the killer is another player. msg is a readable summary.
JsonValue DeathPayload(const PlayerId& victim, const PlayerId* attacker, const Position& pos, const std::string& msg);
JsonValue KilledPayload(const PlayerId& killer, const std::string& entity, const std::string& weapon);
JsonValue LogPayload(const std::string& line);

// The whitelist chokepoint: drops every key Takaro's DTO for `type` does not accept (recursively
// for player/attacker/position). Returns Null for an unknown type or a non-object.
JsonValue Sanitize(const std::string& type, const JsonValue& data);

// Readable death cause from the DamageType class name ("DmgTypeHealth_IgnoreArmor_BP_Fall_C" ->
// "fall damage"); empty when unknown.
std::string CauseFromDamageType(const std::string& damageTypeClass);
// Death summary for player-death.msg.
std::string DeathMessage(const std::string& victim, const std::string& killer, const std::string& cause);

// Item display name for entity-killed.weapon. Internal rows (S3 filter: "XX_", "dev_", "test_",
// "deprecated", "do not use") are not player-facing: the bare-fist templates become "Unarmed",
// anything else internal becomes "".
std::string WeaponName(const std::string& itemName, int32_t templateId);
// True for S3's internal-row filter.
bool IsInternalName(const std::string& name);
// Last-resort entity name from an NPC class when neither the character nor the entity catalogue
// names it: "BP_NPC_Wildlife_Rabbit_White_C" -> "Rabbit White". "" when nothing is left.
std::string EntityNameFromClass(const std::string& npcClass);

// ---- log tail helpers ----
// Splits appended bytes into complete lines; keeps a partial last line for the next call.
// Strips CR, a UTF-8 BOM and empty lines; replaces invalid UTF-8 with '?'; caps a line at maxBytes.
class LineSplitter {
public:
    explicit LineSplitter(size_t maxBytes = 2000) : max_(maxBytes) {}
    std::vector<std::string> Feed(const std::string& bytes);
    void Reset() { partial_.clear(); }

private:
    size_t max_;
    std::string partial_;
};
std::string CleanUtf8(const std::string& s);

// Removes secrets from a log line: every literal in `secrets` (length >= 6), and the value of any
// key=value / key: value pair whose key contains password, passwd, token or secret.
std::string RedactLine(const std::string& line, const std::vector<std::string>& secrets);

// Sliding two-window limit, so that every fixed window Takaro counts stays under its log budget
// (hosted Takaro drops `log` events above 50 per 5 s or 300 per 30 s per game server and stores an
// event-rate-limited row instead). A sliding window of the same length bounds any fixed window.
class WindowLimiter {
public:
    WindowLimiter(uint64_t shortMs, size_t shortMax, uint64_t longMs, size_t longMax)
        : shortMs_(shortMs), longMs_(longMs), shortMax_(shortMax), longMax_(longMax) {}
    // countDrop=false: a refusal is not counted (the tail's own summary line).
    bool Allow(uint64_t nowMs, bool countDrop = true);
    uint64_t Dropped() const { return dropped_; }
    uint64_t TakeDroppedSinceLast();
    // Drops not yet reported (TakeDroppedSinceLast would return this).
    uint64_t PendingDropped() const { return dropped_ - reported_; }

private:
    uint64_t shortMs_, longMs_;
    size_t shortMax_, longMax_;
    std::deque<uint64_t> sent_;
    uint64_t dropped_ = 0, reported_ = 0;
};

}  // namespace events
}  // namespace conan
