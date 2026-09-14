// Plugin state: event ring buffer, player registry, capability status, log-line correlation.
#pragma once
#include "common.h"

#include <deque>

struct PlayerRecord {
    std::string steamId;  // SteamID64 as decimal string; used as gameId
    std::string name;     // Steam persona name as printed by the server
    std::string peerId;   // e.g. "0(1)"
    std::string machine;  // machine index as printed in "Machine 'M'"
    std::string handle;   // player handle as printed, e.g. "0(0)"
    std::string group;    // user group name matched from permissions, or "unknown"
    std::vector<std::string> permissions;
    std::string connectedAt;  // ISO time of login
    bool online = false;      // true once logged in
    bool removed = false;     // "Remove Player" seen, waiting for "Removed peer"
};

struct EventRecord {
    uint64_t seq;
    std::string type;
    std::string dataJson;
    std::string ts;
};

class PluginState {
public:
    static PluginState& Get();

    // capabilities: "ok" | "degraded" | "unimplemented"; detail is free text for diagnostics
    void SetCapability(const std::string& name, const std::string& status, const std::string& detail = "");
    std::string CapabilitiesJson() const;
    std::string CapabilityDetailsJson() const;

    void PushEvent(const std::string& type, const std::string& dataJson);
    std::string EventsJson(uint64_t since, size_t limit) const;

    // Fed every formatted server log line (without trailing newline).
    void OnLogLine(int level, const std::string& line);
    // Periodic housekeeping (finalizes pending permission blocks).
    void Housekeep();

    std::string PlayersJson() const;
    bool PlayerJson(const std::string& gameId, std::string& out) const;
    bool IsOnline(const std::string& gameId) const;
    // machine index (as printed in "Machine 'M'") of an online player; false if unknown
    bool OnlineMachine(const std::string& gameId, std::string& machine) const;
    std::vector<std::pair<std::string, std::string>> OnlineMachines() const;  // (steamId, machine)

    // Thread-safe event push for hook code (takes the state lock).
    void EmitEvent(const std::string& type, const std::string& dataJson);
    // Online player lookup by SteamID64 or (case-insensitive) name: fills steamId, name and player JSON.
    bool FindOnline(const std::string& idOrName, std::string& steamId, std::string& name, std::string* json = nullptr) const;
    // Online player JSON by exact name (the Steam persona the server prints); false if none or ambiguous.
    bool PlayerJsonByName(const std::string& name, std::string& json) const;

    uint64_t logLines = 0;

private:
    PluginState() = default;
    void FinalizeLoginLocked();
    std::string PlayerJsonLocked(const PlayerRecord& p, bool online = true) const;
    void LoadGroupsLocked();

    mutable SrwLock lock_;
    std::map<std::string, std::pair<std::string, std::string>> caps_;
    std::deque<EventRecord> events_;
    uint64_t seq_ = 0;

    // player tracking
    std::vector<PlayerRecord> players_;  // pending (not online) + online
    std::string lastLoginMachine_, lastLoginHandle_;
    bool collectingPerms_ = false;
    int loginIdx_ = -1;
    uint64_t lastPermLineMs_ = 0;

    struct Group {
        std::string name;
        bool kickBan, inventories, editWorld, editBase, extendBase;
    };
    std::vector<Group> groups_;
    bool groupsLoaded_ = false;
};
