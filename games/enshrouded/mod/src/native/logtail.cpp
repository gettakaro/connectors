#include "native/logtail.h"

#include "native/fileio.h"
#include "native/json_util.h"

#include <algorithm>
#include <regex>

namespace native {

namespace {

const std::regex& ReAddedPeer() {
    static const std::regex r("\\[online\\] Added peer (\\d+\\((\\d+)\\)) \\(steamid:(\\d+)\\)");
    return r;
}
const std::regex& ReRemovedPeer() {
    static const std::regex r("\\[online\\] Removed peer (\\d+\\(\\d+\\))");
    return r;
}
const std::regex& ReMachineLogin() {
    static const std::regex r("\\[server\\] Machine '(\\d+)': Player '[^']*' logged in");
    return r;
}
const std::regex& RePlayerLogin() {
    static const std::regex r("\\[server\\] Player '(.+)' logged in with Permissions");
    return r;
}
const std::regex& ReRemovePlayer() {
    static const std::regex r("\\[server\\] Remove Player '(.+)'$");
    return r;
}

JsonValue ToPlayer(const std::string& steamId, const std::string& name, bool named) {
    JsonValue p = JObj();
    Put(p, "gameId", JStr(steamId));
    Put(p, "name", JStr(named ? name : steamId));
    Put(p, "steamId", JStr(steamId));
    Put(p, "platformId", JStr("steam:" + steamId));
    JsonValue d = JObj();
    Put(d, "player", p);
    return d;
}

}  // namespace

std::vector<LogJoinEvent> LogParser::Feed(const std::string& rawLine) {
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::smatch m;
    if (std::regex_search(line, m, ReAddedPeer())) {
        Peer p;
        p.peerId = m[1];
        p.machine = m[2];
        p.steamId = m[3];
        auto it = peers_.find(p.peerId);
        // Map#set on an existing key keeps its insertion position
        p.order = it != peers_.end() ? it->second.order : ++order_;
        peers_[p.peerId] = p;
        return {};
    }
    if (std::regex_search(line, m, ReMachineLogin())) {
        lastMachine_ = m[1];
        haveLastMachine_ = true;
        return {};
    }
    if (std::regex_search(line, m, RePlayerLogin())) {
        std::string name = m[1];
        Peer* pick = nullptr;
        Peer* first = nullptr;
        for (auto* p : Ordered())
            if (!p->named) {
                if (!first) first = p;
                if (!pick && haveLastMachine_ && p->machine == lastMachine_) pick = p;
            }
        if (!pick) pick = first;
        haveLastMachine_ = false;
        lastMachine_.clear();
        if (!pick) return {};  // join happened before we started tailing; no SteamID to report
        pick->name = name;
        pick->named = true;
        return {{"player-connected", ToPlayer(pick->steamId, pick->name, true)}};
    }
    if (std::regex_search(line, m, ReRemovePlayer())) {
        std::string name = m[1];
        for (auto* p : Ordered())
            if (p->named && p->name == name) {
                LogJoinEvent ev{"player-disconnected", ToPlayer(p->steamId, p->name, true)};
                peers_.erase(p->peerId);
                return {ev};
            }
        return {};
    }
    if (std::regex_search(line, m, ReRemovedPeer())) {
        auto it = peers_.find(m[1]);
        if (it == peers_.end()) return {};
        Peer p = it->second;
        peers_.erase(it);
        // Named peer that vanished without a `Remove Player` line (crash/timeout) still counts as a disconnect.
        if (p.named) return {{"player-disconnected", ToPlayer(p.steamId, p.name, true)}};
        return {};
    }
    return {};
}

std::vector<LogParser::Peer*> LogParser::Ordered() {
    std::vector<Peer*> v;
    for (auto& kv : peers_) v.push_back(&kv.second);
    std::sort(v.begin(), v.end(), [](const Peer* a, const Peer* b) { return a->order < b->order; });
    return v;
}

void LogTailer::Start() {
    if (running_) return;
    running_ = true;
    offset_ = -1;
}

std::vector<LogJoinEvent> LogTailer::Poll(std::string* error) {
    std::vector<LogJoinEvent> out;
    if (!running_) return out;
    FileStat st = StatFile(file_);
    if (!st.exists) {
        if (error) *error = "log file not found: " + file_;
        return out;
    }
    if (offset_ < 0) {
        offset_ = fromStart_ ? 0 : (int64_t)st.size;
        identity_ = st.identity;
    } else if (st.identity != identity_ || st.size < (uint64_t)offset_) {
        offset_ = 0;
        identity_ = st.identity;
        partial_.clear();
        parser_ = LogParser();
    }
    if (st.size <= (uint64_t)offset_) return out;
    std::string chunk;
    // Bounded read per poll so a huge backlog cannot stall the bridge thread.
    uint64_t length = std::min<uint64_t>(st.size - (uint64_t)offset_, 4u << 20);
    if (!ReadFileRange(file_, (uint64_t)offset_, length, chunk)) {
        if (error) *error = "log file read failed: " + file_;
        return out;
    }
    offset_ += (int64_t)chunk.size();
    std::string text = partial_ + chunk;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) break;
        for (auto& ev : parser_.Feed(text.substr(start, nl - start))) out.push_back(std::move(ev));
        start = nl + 1;
    }
    partial_ = text.substr(start);
    return out;
}

}  // namespace native
