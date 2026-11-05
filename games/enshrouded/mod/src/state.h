// Plugin state: event ring buffer, player registry, capability status, log-line correlation.
#pragma once
#include "common.h"
#include "console_args.h"

#include <deque>
#include <set>

struct PlayerRecord {
    std::string steamId;  // SteamID64 as decimal string; used as gameId
    std::string name;     // Steam persona name as printed by the server (matches the game's player slot)
    std::string characterName;  // in-game character name, read from the game once the player's entity exists
    int level = -1;             // character level and experience (ecs::NetworkPlayerAttributes), -1 until read
    int experience = -1;
    int experienceRequired = -1;
    int levelLogged = -1;       // last level written to plugin.log (logged from Housekeep, off the game thread)
    std::string peerId;   // e.g. "0(1)"
    std::string machine;  // machine index as printed in "Machine 'M'"
    std::string authMachine;  // machine index the game gave this peer ("Add remote machine index M"), when certain
    bool steamAuthed = false;  // "Client '<id>' authenticated by steam" seen and no machine bound yet
    std::string handle;   // player handle as printed, e.g. "0(0)"
    std::string group;    // user group name matched from permissions, or "unknown"
    std::vector<std::string> permissions;
    std::string connectedAt;  // ISO time of login
    bool online = false;      // true once logged in
    bool removed = false;     // "Remove Player" seen, waiting for "Removed peer"
    bool connectPending = false;  // logged in; player-connected waits for the character name (bounded)
    uint64_t loginMs = 0;
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
    // JSON array of the capabilities that are only waiting for the game to start running (hook installed, its
    // first call not seen yet, e.g. the game thread before the world has loaded). Empty once the world runs.
    std::string StartingCapabilitiesJson() const;
    // Game build support, set once at startup. `knownBuilds` lists the builds this plugin has gamedata for.
    void SetGameSupport(const std::string& build, bool gamedataSection, bool itemTables, const std::string& knownBuilds);
    // Empty when the running game build is supported; otherwise one line for owners: the build, why it is not
    // supported, the capabilities that are off right now, and what to do.
    std::string GameSupportNotice() const;

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
    // How "Machine 'M'" maps to "Added peer N(G)": 1024233 prints G == M, 1.0 prints M == N + 1 (gamedata loginMachine).
    void SetMachineIsPeerIndexPlusOne(bool on);
    // Character name of the online player whose Steam persona name is `persona` (reported to Takaro as "name").
    void SetCharacterName(const std::string& persona, const std::string& character);
    // Character level of the online player whose Steam persona name is `persona`. Called from the game thread: it
    // only stores the values; Housekeep logs level changes.
    void SetLevel(const std::string& persona, int level, int experience, int experienceRequired);
    // The game build exposes character names: player-connected then waits (up to connectNameWaitMs) for the
    // character name, so Takaro never sees the Steam persona first and records a rename on every join.
    void SetCharacterNamesAvailable(bool on);
    void SetConnectNameWaitMs(uint64_t ms);
    bool OnlineMachine(const std::string& gameId, std::string& machine) const;
    std::vector<std::pair<std::string, std::string>> OnlineMachines() const;  // (steamId, machine)

    // Thread-safe event push for hook code (takes the state lock).
    void EmitEvent(const std::string& type, const std::string& dataJson);
    // Online player lookup by SteamID64 or (case-insensitive) name: fills steamId, name and player JSON.
    bool FindOnline(const std::string& idOrName, std::string& steamId, std::string& name, std::string* json = nullptr) const;
    // Online players for console argument matching (console_args.h).
    std::vector<console::Player> OnlinePlayers() const;
    // Online player JSON by exact name (the Steam persona the server prints); false if none or ambiguous.
    bool PlayerJsonByName(const std::string& name, std::string& json) const;

    uint64_t logLines = 0;

private:
    PluginState() = default;
    void FinalizeLoginLocked();
    void EmitConnectedLocked(PlayerRecord& p);
    void BindMachineLocked(size_t idx, const std::string& machine);
    std::string PlayerJsonLocked(const PlayerRecord& p, bool online = true) const;
    void LoadGroupsLocked();

    mutable SrwLock lock_;
    std::map<std::string, std::pair<std::string, std::string>> caps_;
    struct GameSupport {
        bool set = false, gamedataSection = true, itemTables = true;
        std::string build, knownBuilds;
    } support_;
    std::deque<EventRecord> events_;
    uint64_t seq_ = 0;

    // player tracking
    std::vector<PlayerRecord> players_;  // pending (not online) + online
    std::string lastLoginMachine_, lastLoginHandle_;
    std::set<std::string> unboundMachines_;  // machines the game added without a certain peer
    std::string machinePeer_;  // peer of the last "Unable to find machine for peer N(G)." (the next machine is its)
    bool machineIsPeerIndexPlusOne_ = false;  // 1.0: Machine 'M' is peer N(..) with M = N + 1
    bool collectingPerms_ = false;
    int loginIdx_ = -1;
    uint64_t lastPermLineMs_ = 0;
    bool charNamesAvailable_ = false;
    uint64_t connectNameWaitMs_ = 5000;

    struct Group {
        std::string name;
        bool kickBan, inventories, editWorld, editBase, extendBase;
    };
    std::vector<Group> groups_;
    bool groupsLoaded_ = false;
};
