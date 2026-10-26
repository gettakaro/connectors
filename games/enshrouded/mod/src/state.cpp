#include "state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static const size_t kMaxEvents = 5000;

PluginState& PluginState::Get() {
    static PluginState s;
    return s;
}

void PluginState::SetCapability(const std::string& name, const std::string& status, const std::string& detail) {
    Guard g(lock_);
    caps_[name] = {status, detail};
}

std::string PluginState::CapabilitiesJson() const {
    Guard g(lock_);
    std::string o = "{";
    bool first = true;
    for (auto& kv : caps_) {
        if (!first) o += ",";
        first = false;
        o += JsonStr(kv.first) + ":" + JsonStr(kv.second.first);
    }
    return o + "}";
}

std::string PluginState::CapabilityDetailsJson() const {
    Guard g(lock_);
    std::string o = "{";
    bool first = true;
    for (auto& kv : caps_) {
        if (kv.second.second.empty()) continue;
        if (!first) o += ",";
        first = false;
        o += JsonStr(kv.first) + ":" + JsonStr(kv.second.second);
    }
    return o + "}";
}

// A hook that is installed but has not been called yet says "waiting for ..." in its detail (hooks.cpp, world.cpp).
static bool WaitingForGame(const std::pair<std::string, std::string>& cap) {
    return cap.first == "degraded" && cap.second.find("waiting for") != std::string::npos;
}

std::string PluginState::StartingCapabilitiesJson() const {
    Guard g(lock_);
    std::string o = "[";
    for (auto& kv : caps_)
        if (WaitingForGame(kv.second)) o += (o.size() > 1 ? "," : "") + JsonStr(kv.first);
    return o + "]";
}

void PluginState::SetGameSupport(const std::string& build, bool gamedataSection, bool itemTables,
                                 const std::string& knownBuilds) {
    Guard g(lock_);
    support_ = {true, gamedataSection, itemTables, build, knownBuilds};
}

std::string PluginState::GameSupportNotice() const {
    Guard g(lock_);
    if (!support_.set || (support_.gamedataSection && support_.itemTables)) return "";
    std::string o = "game build " + support_.build + " is not supported by plugin " TAKARO_PLUGIN_VERSION ": ";
    if (!support_.gamedataSection)
        o += "no gamedata section for this build (this plugin has " +
             (support_.knownBuilds.empty() ? std::string("none") : support_.knownBuilds) + ")";
    if (!support_.itemTables)
        o += std::string(support_.gamedataSection ? "" : "; ") + "the item and entity tables are for another build";
    std::string off;
    for (auto& kv : caps_) {
        if (kv.second.first != "degraded") continue;
        // still starting up is not "off"
        if (WaitingForGame(kv.second) || kv.second.second.find("initializing") != std::string::npos) continue;
        off += (off.empty() ? "" : ", ") + kv.first;
    }
    return o + "; capabilities off: " + (off.empty() ? std::string("none") : off) +
           "; update the plugin to a release that supports this game build";
}

void PluginState::PushEvent(const std::string& type, const std::string& dataJson) {
    // caller may or may not hold lock_; use a separate small critical path
    EventRecord e{0, type, dataJson, IsoNowUtc()};
    e.seq = ++seq_;
    events_.push_back(std::move(e));
    while (events_.size() > kMaxEvents) events_.pop_front();
}

std::string PluginState::EventsJson(uint64_t since, size_t limit) const {
    Guard g(lock_);
    std::string o;
    o.reserve(4096);
    size_t n = 0;
    std::string items;
    uint64_t lastSeq = since;
    // seqs are strictly increasing: skip the already-read prefix without walking it (the lock is shared
    // with the game's log-sink hook)
    auto from = std::upper_bound(events_.begin(), events_.end(), since,
                                 [](uint64_t s, const EventRecord& e) { return s < e.seq; });
    for (auto it = from; it != events_.end(); ++it) {
        const EventRecord& e = *it;
        if (n >= limit) break;
        if (n) items += ",";
        items += "{\"seq\":" + std::to_string(e.seq) + ",\"type\":" + JsonStr(e.type) + ",\"data\":" + e.dataJson +
                 ",\"ts\":" + JsonStr(e.ts) + "}";
        lastSeq = e.seq;
        n++;
    }
    // seq: the cursor to pass as `since` next time (last returned event, or latest if nothing new)
    uint64_t cursor = n ? lastSeq : (seq_ > since ? since : seq_);
    uint64_t oldest = events_.empty() ? seq_ + 1 : events_.front().seq;
    o = "{\"seq\":" + std::to_string(cursor) + ",\"latestSeq\":" + std::to_string(seq_) +
        ",\"truncated\":" + ((since + 1 < oldest && since < seq_) ? "true" : "false") + ",\"events\":[" + items + "]}";
    return o;
}

// ---------------------------------------------------------------------------------------------

static bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

// "0(1)" -> "1"
static std::string ParenPart(const std::string& id) {
    auto a = id.find('('), b = id.find(')');
    if (a == std::string::npos || b == std::string::npos || b <= a) return "";
    return id.substr(a + 1, b - a - 1);
}

// "1(1)" -> "2": the machine number 1.0 prints for peer index 1
static std::string PeerIndexPlusOne(const std::string& id) {
    auto a = id.find('(');
    std::string n = id.substr(0, a);
    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) return "";
    return std::to_string(strtoul(n.c_str(), nullptr, 10) + 1);
}

void PluginState::LoadGroupsLocked() {
    groupsLoaded_ = true;
    std::string path = PluginBaseDir() + "\\enshrouded_server.json";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    std::string text;
    char buf[8192];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, r);
    fclose(f);
    JsonValue root;
    if (!JsonParse(text, root)) {
        PluginLog("groups: enshrouded_server.json parse failed");
        return;
    }
    auto* ug = root.get("userGroups");
    if (!ug || ug->type != JsonValue::Array) return;
    auto flag = [](const JsonValue& o, const char* k) {
        auto* v = o.get(k);
        return v && v->type == JsonValue::Bool && v->b;
    };
    for (auto& g : ug->arr) {
        auto* n = g.get("name");
        if (!n || !n->isStr()) continue;
        groups_.push_back({n->str, flag(g, "canKickBan"), flag(g, "canAccessInventories"), flag(g, "canEditWorld"),
                           flag(g, "canEditBase"), flag(g, "canExtendBase")});
    }
    PluginLog("groups: loaded %zu user groups", groups_.size());
}

void PluginState::FinalizeLoginLocked() {
    collectingPerms_ = false;
    if (loginIdx_ < 0 || loginIdx_ >= (int)players_.size()) {
        loginIdx_ = -1;
        return;
    }
    PlayerRecord& p = players_[loginIdx_];
    loginIdx_ = -1;
    if (!groupsLoaded_) LoadGroupsLocked();
    auto has = [&](const char* perm) {
        for (auto& x : p.permissions)
            if (x == perm) return true;
        return false;
    };
    p.group = "unknown";
    for (auto& g : groups_) {
        if (g.kickBan == has("CanKickBan") && g.inventories == has("CanAccessInventories") &&
            g.editWorld == has("CanEditWorld") && g.editBase == has("CanEditBase") &&
            g.extendBase == has("CanExtendBase")) {
            p.group = g.name;
            break;
        }
    }
    p.loginMs = NowMs();
    p.connectPending = true;
    if (!charNamesAvailable_ || !p.characterName.empty()) EmitConnectedLocked(p);
}

void PluginState::BindMachineLocked(size_t idx, const std::string& machine) {
    // one machine, one account: a peer that held this machine before (a flapping connection) loses it
    for (size_t i = 0; i < players_.size(); i++)
        if (i != idx && !players_[i].online && players_[i].authMachine == machine) players_[i].authMachine.clear();
    players_[idx].authMachine = machine;
    players_[idx].steamAuthed = false;
}

void PluginState::EmitConnectedLocked(PlayerRecord& p) {
    if (!p.connectPending) return;
    p.connectPending = false;
    PushEvent("player-connected", "{\"player\":" + PlayerJsonLocked(p) + "}");
    PluginLog("player-connected name=%s character=%s peer=%s group=%s", p.name.c_str(), p.characterName.c_str(),
              p.peerId.c_str(), p.group.c_str());
}

void PluginState::OnLogLine(int level, const std::string& line) {
    static const char* kLevels[] = {"fatal", "error", "warning", "info", "verbose", "debug"};
    Guard g(lock_);
    logLines++;

    // Permission block following "logged in with Permissions:"
    if (collectingPerms_) {
        if (StartsWith(line, "\t - ")) {
            players_[loginIdx_].permissions.push_back(line.substr(4));
            lastPermLineMs_ = NowMs();
        } else {
            FinalizeLoginLocked();
        }
    }

    PushEvent("log", "{\"msg\":" + JsonStr(line) + ",\"level\":" + JsonStr(level >= 0 && level <= 5 ? kLevels[level] : "info") + "}");

    // [online] Added peer 0(1) (steamid:7656...)   /  (7656...)
    if (StartsWith(line, "[online] Added peer ")) {
        size_t p = strlen("[online] Added peer ");
        size_t sp = line.find(' ', p);
        if (sp == std::string::npos) return;
        std::string peer = line.substr(p, sp - p);
        size_t o = line.find('(', sp);
        size_t c = line.rfind(')');
        if (o == std::string::npos || c == std::string::npos || c <= o) return;
        std::string id = line.substr(o + 1, c - o - 1);
        if (StartsWith(id, "steamid:")) id = id.substr(8);
        bool digits = !id.empty();
        for (char ch : id) digits = digits && ch >= '0' && ch <= '9';
        if (!digits) {
            PluginLog("added peer with non-steam id '%s' (peer %s) - not tracked", id.c_str(), peer.c_str());
            return;
        }
        for (auto it = players_.begin(); it != players_.end(); ++it) {
            if (it->steamId == id && !it->online) {  // stale pending/removed record for this account
                players_.erase(it);
                break;
            }
        }
        PlayerRecord r;
        r.steamId = id;
        r.peerId = peer;
        players_.push_back(r);
        return;
    }

    // [online] Client '7656...' authenticated by steam
    if (StartsWith(line, "[online] Client '")) {
        size_t a = strlen("[online] Client '");
        size_t b = line.find("' authenticated by steam", a);
        if (b == std::string::npos) return;
        std::string id = line.substr(a, b - a);
        for (auto it = players_.rbegin(); it != players_.rend(); ++it)
            if (it->steamId == id && !it->online && !it->removed) {
                if (it->authMachine.empty()) it->steamAuthed = true;
                break;
            }
        return;
    }

    // [session] Unable to find machine for peer 1(1).   -- printed right before that peer's "Add remote machine index M"
    if (StartsWith(line, "[session] Unable to find machine for peer ")) {
        size_t a = strlen("[session] Unable to find machine for peer ");
        size_t b = line.find('.', a);
        if (b != std::string::npos) machinePeer_ = line.substr(a, b - a);
        return;
    }

    // [session] Add remote machine index 2 (id: 2778328608).
    // The machine is for the peer named just before; without that line, for the one peer that passed Steam auth and
    // has no machine yet. Anything else is not certain: the machine stays unbound and its login is not guessed.
    if (StartsWith(line, "[session] Add remote machine index ")) {
        size_t a = strlen("[session] Add remote machine index ");
        size_t b = line.find(' ', a);
        std::string peer = machinePeer_;
        machinePeer_.clear();
        if (b == std::string::npos || b == a) return;
        std::string machine = line.substr(a, b - a);
        int idx = -1, authed = 0;
        for (size_t i = 0; i < players_.size(); i++) {
            const PlayerRecord& r = players_[i];
            if (r.online || r.removed) continue;
            if (!peer.empty()) {
                if (r.peerId == peer) idx = (int)i;
            } else if (r.steamAuthed && r.authMachine.empty()) {
                idx = (int)i;
                authed++;
            }
        }
        if (peer.empty() && authed != 1) idx = -1;
        if (idx >= 0) {
            BindMachineLocked((size_t)idx, machine);
            unboundMachines_.erase(machine);
        } else if (peer.empty() && authed == 0) {
            return;  // no Steam auth to go by (older logs): the login falls back to the machine number from the peer id
        } else {
            for (auto& r : players_)  // whoever held this machine before no longer does
                if (!r.online && r.authMachine == machine) r.authMachine.clear();
            unboundMachines_.insert(machine);
            if (!peer.empty())
                PluginLog("machine %s is for peer %s, which has no pending steam account - its login stays unresolved",
                          machine.c_str(), peer.c_str());
            else
                PluginLog("machine %s: %d steam-authenticated peers wait for a machine and the game named none - its "
                          "login stays unresolved",
                          machine.c_str(), authed);
        }
        return;
    }

    // [server] Machine '1': Player '0(0)' logged in
    if (StartsWith(line, "[server] Machine '")) {
        size_t a = strlen("[server] Machine '");
        size_t b = line.find('\'', a);
        size_t h = line.find("Player '", b == std::string::npos ? a : b);
        size_t he = h == std::string::npos ? h : line.find('\'', h + 8);
        static const char* kSuffix = "' logged in";
        size_t sl = strlen(kSuffix);
        if (b != std::string::npos && he != std::string::npos && line.size() > sl &&
            line.compare(line.size() - sl, sl, kSuffix) == 0) {
            lastLoginMachine_ = line.substr(a, b - a);
            lastLoginHandle_ = line.substr(h + 8, he - h - 8);
        }
        return;
    }

    // [server] Player 'Limon' logged in with Permissions:
    static const char* kLoginSuffix = "' logged in with Permissions:";
    if (StartsWith(line, "[server] Player '") && line.size() > strlen(kLoginSuffix) + 17 &&
        line.compare(line.size() - strlen(kLoginSuffix), strlen(kLoginSuffix), kLoginSuffix) == 0) {
        std::string name = line.substr(17, line.size() - strlen(kLoginSuffix) - 17);
        const std::string& mach = lastLoginMachine_;
        auto pending = [&](const PlayerRecord& r) { return !r.online && !r.removed; };
        int idx = -1;
        for (size_t i = 0; i < players_.size() && idx < 0 && !mach.empty(); i++)  // the peer the game gave this machine
            if (pending(players_[i]) && players_[i].authMachine == mach) idx = (int)i;
        // the game logged this machine but could not say for whom: never guess, a wrong guess swaps Steam accounts
        bool unbound = !mach.empty() && unboundMachines_.count(mach);
        int candidates = 0, single = -1;
        for (size_t i = 0; i < players_.size() && idx < 0 && !unbound; i++) {
            const PlayerRecord& r = players_[i];
            if (!pending(r) || !r.authMachine.empty()) continue;  // a peer bound to another machine is not this one
            candidates++;
            single = (int)i;
            if (!mach.empty() &&
                (machineIsPeerIndexPlusOne_ ? PeerIndexPlusOne(r.peerId) : ParenPart(r.peerId)) == mach)
                idx = (int)i;  // machine number from the peer id
        }
        if (idx < 0 && !unbound && candidates == 1) idx = single;  // the only steam peer still waiting
        if (!mach.empty()) unboundMachines_.erase(mach);
        if (idx < 0) {
            if (unbound || candidates > 1)
                PluginLog("login for '%s' on machine '%s' matches no single steam peer - identity left unresolved, "
                          "not tracked",
                          name.c_str(), mach.c_str());
            else
                PluginLog("login for '%s' without a pending steam peer - not tracked", name.c_str());
            lastLoginMachine_.clear();
            lastLoginHandle_.clear();
            return;
        }
        for (size_t i = 0; i < players_.size(); i++)  // this machine is taken now
            if ((int)i != idx && !players_[i].online && players_[i].authMachine == mach) players_[i].authMachine.clear();
        PlayerRecord& p = players_[idx];
        p.name = name;
        p.machine = lastLoginMachine_;
        p.handle = lastLoginHandle_;
        p.online = true;
        p.connectedAt = IsoNowUtc();
        p.permissions.clear();
        lastLoginMachine_.clear();
        lastLoginHandle_.clear();
        collectingPerms_ = true;
        loginIdx_ = idx;
        lastPermLineMs_ = NowMs();
        return;
    }

    // [server] Remove Player 'Limon'
    if (StartsWith(line, "[server] Remove Player '") && line.back() == '\'') {
        std::string name = line.substr(24, line.size() - 25);
        for (auto it = players_.begin(); it != players_.end(); ++it) {
            if (it->online && it->name == name) {
                EmitConnectedLocked(*it);
                PushEvent("player-disconnected", "{\"player\":" + PlayerJsonLocked(*it, false) + "}");
                PluginLog("player-disconnected name=%s (Remove Player)", name.c_str());
                it->online = false;
                // keep the peer record until "Removed peer", but never match it to a new login
                it->removed = true;
                break;
            }
        }
        return;
    }

    // [online] Removed peer 0(1)
    if (StartsWith(line, "[online] Removed peer ")) {
        std::string peer = line.substr(22);
        for (auto it = players_.begin(); it != players_.end(); ++it) {
            if (it->peerId == peer) {
                if (it->online) {
                    EmitConnectedLocked(*it);
                    PushEvent("player-disconnected", "{\"player\":" + PlayerJsonLocked(*it, false) + "}");
                    PluginLog("player-disconnected name=%s (Removed peer)", it->name.c_str());
                }
                if (loginIdx_ == (int)(it - players_.begin())) {
                    collectingPerms_ = false;
                    loginIdx_ = -1;
                }
                players_.erase(it);
                break;
            }
        }
        return;
    }
}

void PluginState::Housekeep() {
    Guard g(lock_);
    if (collectingPerms_ && NowMs() - lastPermLineMs_ > 750) FinalizeLoginLocked();
    // held player-connected events go out here, on the plugin thread, never from the game-thread name refresh
    for (auto& p : players_) {
        if (p.online && p.level >= 0 && p.level != p.levelLogged) {
            p.levelLogged = p.level;
            PluginLog("character level: steam=%s persona=%s level=%d", p.steamId.c_str(), p.name.c_str(), p.level);
        }
        if (!p.online || !p.connectPending) continue;
        if (!p.characterName.empty()) {
            EmitConnectedLocked(p);
        } else if (NowMs() - p.loginMs >= connectNameWaitMs_) {
            PluginLog("character name for %s not readable after %llu ms; reporting the Steam name", p.name.c_str(),
                      (unsigned long long)connectNameWaitMs_);
            EmitConnectedLocked(p);
        }
    }
}

void PluginState::SetCharacterNamesAvailable(bool on) {
    Guard g(lock_);
    charNamesAvailable_ = on;
}

void PluginState::SetConnectNameWaitMs(uint64_t ms) {
    Guard g(lock_);
    connectNameWaitMs_ = ms;
}

std::string PluginState::PlayerJsonLocked(const PlayerRecord& p, bool online) const {
    const std::string& id = p.steamId;
    std::string perms = "[";
    for (size_t i = 0; i < p.permissions.size(); i++) perms += (i ? "," : "") + JsonStr(p.permissions[i]);
    perms += "]";
    const std::string& shown = p.characterName.empty() ? p.name : p.characterName;
    std::string level = p.level < 0 ? "" : ",\"level\":" + std::to_string(p.level) + ",\"experience\":" +
                                               std::to_string(p.experience) + ",\"experienceRequired\":" +
                                               std::to_string(p.experienceRequired);
    return "{\"gameId\":" + JsonStr(id) + ",\"name\":" + JsonStr(shown) + ",\"steamId\":" + JsonStr(id) + level +
           ",\"peerId\":" + JsonStr(p.peerId) + ",\"group\":" + JsonStr(p.group) + ",\"permissions\":" + perms +
           ",\"connectedAt\":" + JsonStr(p.connectedAt) + ",\"online\":" + (online ? "true" : "false") + "}";
}

std::string PluginState::PlayersJson() const {
    Guard g(lock_);
    std::string o = "[";
    bool first = true;
    for (size_t i = 0; i < players_.size(); i++) {
        auto& p = players_[i];
        // listed once logged in, its permission block finalized and its player-connected sent, so getPlayers
        // never shows a name before the event does
        if (!p.online || p.connectPending || (collectingPerms_ && loginIdx_ == (int)i)) continue;
        if (!first) o += ",";
        first = false;
        o += PlayerJsonLocked(p);
    }
    return o + "]";
}

bool PluginState::PlayerJson(const std::string& gameId, std::string& out) const {
    Guard g(lock_);
    for (auto& p : players_)
        if (p.online && p.steamId == gameId) {
            out = PlayerJsonLocked(p);
            return true;
        }
    return false;
}

void PluginState::SetCharacterName(const std::string& persona, const std::string& character) {
    Guard g(lock_);
    for (auto& p : players_)
        if (p.online && p.name == persona && p.characterName != character) {
            p.characterName = character;
            PluginLog("character name: steam=%s persona=%s character=%s", p.steamId.c_str(), persona.c_str(), character.c_str());
        }
}

void PluginState::SetLevel(const std::string& persona, int level, int experience, int experienceRequired) {
    Guard g(lock_);
    for (auto& p : players_)
        if (p.online && p.name == persona) {
            p.level = level;
            p.experience = experience;
            p.experienceRequired = experienceRequired;
        }
}

void PluginState::SetMachineIsPeerIndexPlusOne(bool on) {
    Guard g(lock_);
    machineIsPeerIndexPlusOne_ = on;
}

bool PluginState::OnlineMachine(const std::string& gameId, std::string& machine) const {
    Guard g(lock_);
    for (auto& p : players_)
        if (p.online && p.steamId == gameId && !p.machine.empty()) {
            machine = p.machine;
            return true;
        }
    return false;
}

std::vector<std::pair<std::string, std::string>> PluginState::OnlineMachines() const {
    Guard g(lock_);
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& p : players_)
        if (p.online && !p.machine.empty()) out.push_back({p.steamId, p.machine});
    return out;
}

bool PluginState::IsOnline(const std::string& gameId) const {
    std::string tmp;
    return PlayerJson(gameId, tmp);
}

void PluginState::EmitEvent(const std::string& type, const std::string& dataJson) {
    Guard g(lock_);
    PushEvent(type, dataJson);
}

static std::string LowerStr(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool PluginState::FindOnline(const std::string& idOrName, std::string& steamId, std::string& name, std::string* json) const {
    Guard g(lock_);
    const PlayerRecord* hit = nullptr;
    for (auto& p : players_)
        if (p.online && p.steamId == idOrName) hit = &p;
    if (!hit) {
        int n = 0;
        for (auto& p : players_)
            if (p.online && (LowerStr(p.name) == LowerStr(idOrName) ||
                             (!p.characterName.empty() && LowerStr(p.characterName) == LowerStr(idOrName)))) {
                hit = &p;
                n++;
            }
        if (n != 1) hit = nullptr;
    }
    if (!hit) return false;
    steamId = hit->steamId;
    name = hit->name;
    if (json) *json = PlayerJsonLocked(*hit);
    return true;
}

bool PluginState::PlayerJsonByName(const std::string& name, std::string& json) const {
    Guard g(lock_);
    int n = 0;
    for (auto& p : players_)
        if (p.online && p.name == name) {
            json = PlayerJsonLocked(p);
            n++;
        }
    return n == 1;
}
