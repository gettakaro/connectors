#include "state.h"

#include <cctype>
#include <cerrno>

PluginState& PluginState::Get() {
    static PluginState s;
    return s;
}

void PluginState::SetCapability(const std::string& name, const std::string& status, const std::string& detail) {
    Guard g(lock_);
    caps_[name] = {status, detail};
}

std::string PluginState::Capability(const std::string& name) const {
    Guard g(lock_);
    auto it = caps_.find(name);
    return it == caps_.end() ? std::string("unimplemented") : it->second.first;
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

void PluginState::EmitEvent(const std::string& type, const std::string& dataJson) {
    EmitEventDeferred(type, [dataJson] { return dataJson; });
}

void PluginState::EmitEventDeferred(const std::string& type, std::function<std::string()> serialize) {
    auto event = std::make_shared<EventRecord>(EventRecord{0, type, std::move(serialize), IsoNowUtc()});
    Guard g(lock_);
    event->seq = ++seq_;
    events_.push_back(std::move(event));
    while (events_.size() > kMaxEvents) events_.pop_front();
}

std::string PluginState::EventsJson(uint64_t since, size_t limit) const {
    std::vector<std::shared_ptr<const EventRecord>> snapshot;
    uint64_t latest, oldest;
    {
        Guard g(lock_);
        latest = seq_;
        oldest = events_.empty() ? seq_ + 1 : events_.front()->seq;
        for (const auto& event : events_) {
            if (event->seq <= since) continue;
            if (snapshot.size() >= limit) break;
            snapshot.push_back(event);
        }
    }
    std::string items;
    size_t n = 0;
    uint64_t lastSeq = since;
    for (const auto& entry : snapshot) {
        const EventRecord& e = *entry;
        if (n) items += ",";
        items += "{\"seq\":" + std::to_string(e.seq) + ",\"type\":" + JsonStr(e.type) + ",\"data\":" + e.serialize() +
                 ",\"ts\":" + JsonStr(e.ts) + "}";
        lastSeq = e.seq;
        n++;
    }
    // `seq` is the cursor to pass back as `since`: the last returned event, or the current latest.
    uint64_t cursor = n ? lastSeq : (latest > since ? since : latest);
    return "{\"bootId\":" + JsonStr(BootId()) + ",\"seq\":" + std::to_string(cursor) +
           ",\"latestSeq\":" + std::to_string(latest) + ",\"truncated\":" +
           ((since + 1 < oldest && since < latest) ? "true" : "false") + ",\"events\":[" + items + "]}";
}

uint64_t PluginState::LatestSeq() const {
    Guard g(lock_);
    return seq_;
}

size_t PluginState::Buffered() const {
    Guard g(lock_);
    return events_.size();
}

// ---------------------------------------------------------------------------------------------
// Injected-message markers (L3 writes, L2's chat hook reads).

namespace {
Mutex g_injLock;
std::deque<std::pair<std::string, uint64_t>> g_injected;  // {text, expiresAtMs}
const uint64_t kInjectedTtlMs = 10000;
}  // namespace

void state::MarkInjectedMessage(const std::string& text) {
    if (text.empty()) return;
    Guard g(g_injLock);
    uint64_t now = NowMs();
    while (!g_injected.empty() && g_injected.front().second <= now) g_injected.pop_front();
    if (g_injected.size() > 64) g_injected.pop_front();
    g_injected.push_back({text, now + kInjectedTtlMs});
}

bool state::ConsumeInjectedMessage(const std::string& text) {
    Guard g(g_injLock);
    uint64_t now = NowMs();
    while (!g_injected.empty() && g_injected.front().second <= now) g_injected.pop_front();
    for (size_t i = 0; i < g_injected.size(); i++) {
        if (g_injected[i].first != text) continue;
        g_injected.erase(g_injected.begin() + (long)i);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Plugin-side ban list (lane L3b), persisted to <PluginDataDir>/bans.json.

namespace {
Mutex g_banLock;
Mutex g_banWriteLock;
std::map<std::string, state::BanRecord> g_bans;  // key = lower-cased gameId
bool g_bansLoaded = false;
std::string g_bansPath;
uint64_t g_banRevision = 0, g_banSavedRevision = 0;
std::string g_banError;
bool g_banLoadFailed = false;

std::string LowerId(const std::string& s) {
    std::string o;
    for (char c : s) o += (char)tolower((unsigned char)c);
    return o;
}

// Serialize an owned snapshot without holding the lock used by PreLogin.
std::string BansSerialize(const std::vector<state::BanRecord>& records) {
    std::string o = "{\"version\":1,\"bans\":[";
    bool first = true;
    for (const auto& b : records) {
        if (!first) o += ",";
        first = false;
        o += "{\"gameId\":" + JsonStr(b.gameId) + ",\"name\":" + JsonStr(b.name) + ",\"reason\":" + JsonStr(b.reason) +
             ",\"createdAt\":" + JsonStr(b.createdAt) +
             ",\"expiresAt\":" + (b.expiresAt.empty() ? std::string("null") : JsonStr(b.expiresAt)) + "}";
    }
    o += "]}\n";
    return o;
}
}  // namespace

std::string state::BansPath() {
    Guard g(g_banLock);
    if (g_bansPath.empty()) g_bansPath = PluginDataDir() + "/bans.json";
    return g_bansPath;
}

void state::BansLoad() {
    Guard g(g_banLock);
    if (g_bansLoaded) return;
    g_bansLoaded = true;
    if (g_bansPath.empty()) g_bansPath = PluginDataDir() + "/bans.json";
    std::string text;
    if (!ReadFile(g_bansPath, text)) {
        if (errno != ENOENT) {
            g_banLoadFailed = true;
            g_banError = "cannot read game-enforcement bans.json";
            PluginLog("state: %s", g_banError.c_str());
            return;
        }
        PluginLog("state: no plugin ban list at %s (starting empty)", g_bansPath.c_str());
        return;
    }
    JsonValue v;
    if (!JsonParse(text, v)) {
        g_banLoadFailed = true;
        g_banError = "game-enforcement bans.json is corrupt; refusing to overwrite it";
        PluginLog("state: %s", g_banError.c_str());
        return;
    }
    const JsonValue* arr = v.get("bans");
    if (!arr || arr->type != JsonValue::Array) {
        g_banLoadFailed = true;
        g_banError = "game-enforcement bans.json has no bans array";
        return;
    }
    for (const JsonValue& e : arr->arr) {
        const JsonValue* id = e.get("gameId");
        if (!id || !id->isStr() || id->str.empty()) continue;
        BanRecord b;
        b.gameId = LowerId(id->str);
        const JsonValue* n = e.get("name");
        const JsonValue* r = e.get("reason");
        const JsonValue* c = e.get("createdAt");
        const JsonValue* x = e.get("expiresAt");
        if (n && n->isStr()) b.name = n->str;
        if (r && r->isStr()) b.reason = r->str;
        if (c && c->isStr()) b.createdAt = c->str;
        if (x && x->isStr()) b.expiresAt = x->str;
        g_bans[b.gameId] = b;
    }
    PluginLog("state: loaded %zu plugin ban(s) from %s", g_bans.size(), g_bansPath.c_str());
}

bool state::IsBanned(const std::string& gameId) {
    if (gameId.empty()) return false;
    Guard g(g_banLock);
    return g_bans.find(LowerId(gameId)) != g_bans.end();
}

static bool AddBanRecord(const state::BanRecord& r, bool conditional, uint64_t expectedRevision) {
    if (r.gameId.empty()) return false;
    Guard g(g_banLock);
    if (g_banLoadFailed || (conditional && g_banRevision != expectedRevision)) return false;
    state::BanRecord b = r;
    b.gameId = LowerId(b.gameId);
    if (b.createdAt.empty()) b.createdAt = IsoNowUtc();
    auto it = g_bans.find(b.gameId);
    if (it != g_bans.end() && b.name.empty()) b.name = it->second.name;
    g_bans[b.gameId] = b;
    ++g_banRevision;
    return true;
}

bool state::BanAdd(const BanRecord& r) { return AddBanRecord(r, false, 0); }
bool state::BanAddIfRevision(const BanRecord& r, uint64_t expectedRevision) {
    return AddBanRecord(r, true, expectedRevision);
}

bool state::BanRemove(const std::string& gameId) {
    if (gameId.empty()) return false;
    Guard g(g_banLock);
    if (g_banLoadFailed) return false;
    // Even a missing plugin record may still be banned in VEIN's own list.
    // Invalidate background recovery before the caller mutates that game list.
    ++g_banRevision;
    auto it = g_bans.find(LowerId(gameId));
    if (it == g_bans.end()) return false;
    g_bans.erase(it);
    return true;
}

uint64_t state::BanRevision() { Guard g(g_banLock); return g_banRevision; }
std::string state::BanPersistenceError() { Guard g(g_banLock); return g_banError; }

bool state::FlushBans() {
    Guard writer(g_banWriteLock);
    std::vector<BanRecord> records;
    std::string path;
    uint64_t revision;
    {
        Guard g(g_banLock);
        if (g_banLoadFailed) return false;
        if (g_banSavedRevision == g_banRevision) return true;
        revision = g_banRevision;
        path = g_bansPath;
        for (const auto& kv : g_bans) records.push_back(kv.second);
    }
    const bool ok = WriteFileAtomic(path, BansSerialize(records));
    {
        Guard g(g_banLock);
        if (ok) { g_banSavedRevision = revision; g_banError.clear(); }
        else g_banError = "could not durably write game-enforcement bans.json";
    }
    return ok;
}

std::vector<state::BanRecord> state::BanList() {
    return ReadBans().records;
}

state::BanSnapshot state::ReadBans() {
    Guard g(g_banLock);
    BanSnapshot out;
    out.revision = g_banRevision;
    for (auto& kv : g_bans) out.records.push_back(kv.second);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Character names seen in the server log.

namespace {
Mutex g_nameLock;
std::map<std::string, std::string> g_charNames;  // gameId -> character name
}  // namespace

void state::NoteCharacterName(const std::string& gameId, const std::string& name) {
    if (gameId.empty() || name.empty()) return;
    Guard g(g_nameLock);
    std::string key = LowerId(gameId);
    auto it = g_charNames.find(key);
    if (it != g_charNames.end() && it->second == name) return;
    if (g_charNames.size() > 256) g_charNames.clear();
    g_charNames[key] = name;
    PluginLog("state: character name for %s is %s (from the server log)", key.c_str(), name.c_str());
}

std::string state::CharacterName(const std::string& gameId) {
    Guard g(g_nameLock);
    auto it = g_charNames.find(LowerId(gameId));
    return it == g_charNames.end() ? std::string() : it->second;
}

// ---- readable creature names (lane L3c) --------------------------------------------------------

namespace {
Mutex g_entityNameLock;
std::map<std::string, std::string> g_entityNames;  // Blueprint class name -> AIName display text
}  // namespace

void state::NoteEntityName(const std::string& code, const std::string& name) {
    if (code.empty() || name.empty()) return;
    Guard g(g_entityNameLock);
    auto it = g_entityNames.find(code);
    if (it != g_entityNames.end() && it->second == name) return;
    if (g_entityNames.size() > 512) g_entityNames.clear();
    g_entityNames[code] = name;
    PluginLog("state: entity %s is named '%s' (AVeinAICharacter::AIName)", code.c_str(), name.c_str());
}

std::string state::EntityName(const std::string& code) {
    Guard g(g_entityNameLock);
    auto it = g_entityNames.find(code);
    return it == g_entityNames.end() ? std::string() : it->second;
}

std::map<std::string, std::string> state::EntityNames() {
    Guard g(g_entityNameLock);
    return g_entityNames;
}
