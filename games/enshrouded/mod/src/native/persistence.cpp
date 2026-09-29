#include "native/persistence.h"

#include "native/fileio.h"
#include "native/json_util.h"
#include "native/mapping.h"

#include <algorithm>
#include <cstdlib>

namespace native {

StatePaths ResolveStatePaths(const std::string& baseDir, const EnvFn& env) {
    StatePaths p;
    std::string dir = env("TAKARO_STATE_DIR");
    p.dir = dir.empty() ? JoinPath(JoinPath(baseDir, "takaro"), "connector-state") : ResolvePath(baseDir, dir);
    p.outbox = JoinPath(p.dir, "event-outbox.json");
    std::string online = env("TAKARO_ONLINE_FILE");
    p.online = online.empty() ? JoinPath(p.dir, "online-players.json") : ResolvePath(baseDir, online);
    p.known = JoinPath(p.dir, "known-players.json");
    p.timedBans = JoinPath(p.dir, "timed-bans.json");
    p.banIntent = JoinPath(p.dir, "ban-intent.json");
    std::string cursor = env("TAKARO_CURSOR_FILE");
    p.legacyCursor = cursor.empty() ? JoinPath(p.dir, "event-cursor.json") : ResolvePath(baseDir, cursor);
    return p;
}

namespace {

std::string CursorJson(const SourceCursor& c) {
    return ObjBuilder().S("bootId", c.bootId).Raw("seq", std::to_string(c.seq)).Done();
}

bool U64(const JsonValue* v, uint64_t& out) {
    if (!v || v->type != JsonValue::Number || v->num < 0) return false;
    out = strtoull(v->str.c_str(), nullptr, 10);
    return true;
}

bool ParseCursor(const JsonValue* v, SourceCursor& c) {
    if (!v || v->type != JsonValue::Object) return false;
    const JsonValue* b = v->get("bootId");
    if (!b || b->type != JsonValue::String) return false;
    c.bootId = b->str;
    return U64(v->get("seq"), c.seq);
}

std::string PlayersJson(const std::vector<JsonValue>& players) {
    std::string o = "[";
    for (size_t i = 0; i < players.size(); i++) o += (i ? "," : "") + JsonDump(players[i]);
    return o + "]";
}

}  // namespace

std::string OutboxJson(const OutboxState& o) {
    std::string pending = "[";
    bool first = true;
    for (auto& e : o.pending) {
        pending += (first ? "" : ",") + ObjBuilder()
                                            .Raw("id", std::to_string(e.id))
                                            .S("bootId", e.source.bootId)
                                            .Raw("seq", std::to_string(e.source.seq))
                                            .S("type", e.type)
                                            .S("frame", e.frame)
                                            .Done();
        first = false;
    }
    pending += "]";
    return ObjBuilder()
        .Raw("version", "1")
        .Raw("scan", CursorJson(o.scan))
        .Raw("confirmed", CursorJson(o.confirmed))
        .Raw("nextId", std::to_string(o.nextId))
        .Raw("losses", std::to_string(o.losses))
        .Raw("confirmedTotal", std::to_string(o.confirmedTotal))
        .B("legacyImported", o.legacyImported)
        .Raw("pending", pending)
        .Done();
}

bool ParseOutbox(const std::string& text, OutboxState& o, std::string& err) {
    JsonValue v;
    if (!ParseJson(text, v) || v.type != JsonValue::Object) return err = "not a JSON object", false;
    const JsonValue* ver = v.get("version");
    if (!ver || ver->type != JsonValue::Number || ver->num != 1) return err = "unsupported version", false;
    OutboxState n;
    if (!ParseCursor(v.get("scan"), n.scan) || !ParseCursor(v.get("confirmed"), n.confirmed))
        return err = "missing scan/confirmed cursor", false;
    if (!U64(v.get("nextId"), n.nextId) || n.nextId == 0) return err = "missing nextId", false;
    U64(v.get("losses"), n.losses);
    U64(v.get("confirmedTotal"), n.confirmedTotal);
    const JsonValue* li = v.get("legacyImported");
    n.legacyImported = li && li->type == JsonValue::Bool && li->b;
    const JsonValue* pend = v.get("pending");
    if (!pend || pend->type != JsonValue::Array) return err = "missing pending[]", false;
    uint64_t last = 0;
    for (auto& e : pend->arr) {
        PendingEvent pe;
        const JsonValue* b = e.get("bootId");
        const JsonValue* t = e.get("type");
        const JsonValue* f = e.get("frame");
        if (!U64(e.get("id"), pe.id) || !U64(e.get("seq"), pe.source.seq) || !b || b->type != JsonValue::String ||
            !t || t->type != JsonValue::String || !f || f->type != JsonValue::String)
            return err = "malformed pending event", false;
        if (pe.id <= last || pe.id >= n.nextId) return err = "pending ids out of order", false;
        JsonValue frame;
        if (!ParseJson(f->str, frame) || frame.type != JsonValue::Object) return err = "pending frame is not JSON", false;
        last = pe.id;
        pe.source.bootId = b->str;
        pe.type = t->str;
        pe.frame = f->str;
        n.pendingBytes += pe.frame.size();
        n.pending.push_back(std::move(pe));
    }
    o = std::move(n);
    return true;
}

void Store::LoadError(const std::string& area, const std::string& message) {
    errors_[area] = message;
    fenced_[area] = true;
    PluginLog("native: state %s unusable, left untouched: %s", area.c_str(), message.c_str());
}

LoadReport Store::Load() {
    LoadReport rep;
    std::string err;
    if (!EnsureDirectory(paths_.dir, err)) {
        // Not fenced: every write retries and reports until the directory is creatable.
        errors_["dir"] = err;
        rep.notes.push_back(err);
    }
    auto read = [&](const std::string& area, const std::string& path, std::string& text) -> bool {
        bool exists = false;
        std::string e;
        if (!ReadWholeFile(path, text, exists, e)) {
            LoadError(area, e);
            return false;
        }
        return exists;
    };

    // outbox
    std::string text;
    if (read("outbox", paths_.outbox, text)) {
        if (!ParseOutbox(text, outbox_, err)) LoadError("outbox", paths_.outbox + ": corrupt (" + err + ")");
    } else if (!Fenced("outbox")) {
        rep.outboxCreated = true;
        // First native start: adopt the sidecar's cursor once (its bootId marks the process it belonged to).
        std::string legacy;
        bool exists = false;
        if (ReadWholeFile(paths_.legacyCursor, legacy, exists, err) && exists) {
            JsonValue v;
            const JsonValue* seq = nullptr;
            const JsonValue* boot = nullptr;
            if (ParseJson(legacy, v) && v.type == JsonValue::Object) {
                seq = v.get("seq");
                boot = v.get("bootId");
            }
            if (seq && seq->type == JsonValue::Number && seq->num >= 0) {
                outbox_.scan.seq = outbox_.confirmed.seq = (uint64_t)seq->num;
                outbox_.scan.bootId = outbox_.confirmed.bootId =
                    boot && boot->type == JsonValue::String ? boot->str : "legacy-sidecar";
                rep.legacyCursorImported = true;
                rep.notes.push_back("imported sidecar event cursor " + paths_.legacyCursor);
            } else {
                rep.notes.push_back("ignored unreadable sidecar event cursor " + paths_.legacyCursor);
            }
        }
        outbox_.legacyImported = true;
        SaveOutbox();
    }

    // online players (the sidecar's file format: an array of Takaro players)
    if (read("online", paths_.online, text)) {
        JsonValue v;
        if (!ParseJson(text, v) || v.type != JsonValue::Array) {
            LoadError("online", paths_.online + ": corrupt (not a JSON array)");
        } else {
            for (auto& p : v.arr) {
                const JsonValue* g = p.get("gameId");
                const JsonValue* n = p.get("name");
                if (g && g->type == JsonValue::String && n && n->type == JsonValue::String) online_.push_back(p);
            }
        }
    }

    if (read("known", paths_.known, text)) {
        JsonValue v;
        if (!ParseJson(text, v) || v.type != JsonValue::Array)
            LoadError("known", paths_.known + ": corrupt (not a JSON array)");
        else
            for (auto& p : v.arr)
                if (p.get("gameId") && p.get("gameId")->type == JsonValue::String) known_.push_back(p);
    }

    if (read("timedBans", paths_.timedBans, text)) {
        JsonValue v;
        bool ok = ParseJson(text, v) && v.type == JsonValue::Array;
        std::map<std::string, TimedBan> bans;
        for (size_t i = 0; ok && i < v.arr.size(); i++) {
            const JsonValue& b = v.arr[i];
            const JsonValue* g = b.get("gameId");
            const JsonValue* e = b.get("expiresAt");
            TimedBan t;
            if (!g || g->type != JsonValue::String || g->str.empty() || !e || e->type != JsonValue::String ||
                !ParseIsoMs(e->str, t.expiresAtMs)) {
                ok = false;
                break;
            }
            t.gameId = g->str;
            t.expiresAt = e->str;
            if (auto r = Str(b.get("reason"))) t.reason = *r;
            if (auto c = Num(b.get("createdAtMs"))) t.createdAtMs = (int64_t)*c;
            bans[t.gameId] = t;
        }
        if (!ok) LoadError("timedBans", paths_.timedBans + ": corrupt (expected [{gameId, expiresAt}])");
        else timedBans_ = std::move(bans);
    }

    if (read("banIntent", paths_.banIntent, text)) {
        JsonValue v;
        if (!ParseJson(text, v) || v.type != JsonValue::Array) LoadError("banIntent", paths_.banIntent + ": corrupt");
        else banIntents_ = v.arr;
    }
    return rep;
}

bool Store::Write(const std::string& area, const std::string& path, const std::string& text) {
    if (Fenced(area)) return false;
    std::string err;
    if (!EnsureDirectory(DirName(path), err) || !AtomicWriteFile(path, text, err)) {
        if (errors_[area] != err) PluginLog("native: persisting %s failed: %s", area.c_str(), err.c_str());
        errors_[area] = err;
        return false;
    }
    errors_.erase(area);
    if (errors_.count("dir") && area == "outbox") errors_.erase("dir");
    writes_++;
    return true;
}

void Store::Admit(PendingEvent ev) {
    outbox_.pendingBytes += ev.frame.size();
    outbox_.pending.push_back(std::move(ev));
    while (outbox_.pending.size() > kMaxPendingEvents || outbox_.pendingBytes > kMaxPendingBytes) {
        // Log lines are the least valuable and the most numerous: they go first.
        auto victim = std::find_if(outbox_.pending.begin(), outbox_.pending.end(),
                                   [](const PendingEvent& e) { return e.type == "log"; });
        if (victim == outbox_.pending.end()) victim = outbox_.pending.begin();
        outbox_.pendingBytes -= victim->frame.size();
        outbox_.pending.erase(victim);
        outbox_.losses++;
    }
}

size_t Store::ConfirmThrough(uint64_t outboxId) {
    size_t n = 0;
    while (!outbox_.pending.empty() && outbox_.pending.front().id <= outboxId) {
        auto& e = outbox_.pending.front();
        if (e.source.seq) outbox_.confirmed = e.source;
        outbox_.pendingBytes -= e.frame.size();
        outbox_.pending.pop_front();
        n++;
    }
    outbox_.confirmedTotal += n;
    return n;
}

bool Store::SaveOutbox() { return Write("outbox", paths_.outbox, OutboxJson(outbox_)); }
bool Store::SaveOnline() { return Write("online", paths_.online, PlayersJson(online_)); }
bool Store::SaveKnown() { return Write("known", paths_.known, PlayersJson(known_)); }

void Store::Remember(const JsonValue& player, int64_t nowMs) {
    const JsonValue* g = player.get("gameId");
    if (!g || g->type != JsonValue::String || g->str.empty()) return;
    JsonValue row = player;
    Put(row, "lastSeen", JNum((double)nowMs));
    for (auto& k : known_)
        if (k.get("gameId") && k.get("gameId")->str == g->str) {
            k = row;
            return;
        }
    known_.push_back(row);
    if (known_.size() > kMaxKnownPlayers) {
        auto oldest = std::min_element(known_.begin(), known_.end(), [](const JsonValue& a, const JsonValue& b) {
            return Num(a.get("lastSeen")).value_or(0) < Num(b.get("lastSeen")).value_or(0);
        });
        known_.erase(oldest);
    }
}

bool Store::SaveTimedBans() {
    std::string o = "[";
    bool first = true;
    for (auto& kv : timedBans_) {
        auto& t = kv.second;
        o += (first ? "" : ",") + ObjBuilder()
                                      .S("gameId", t.gameId)
                                      .S("expiresAt", t.expiresAt)
                                      .S("reason", t.reason)
                                      .Raw("createdAtMs", std::to_string(t.createdAtMs))
                                      .Done();
        first = false;
    }
    return Write("timedBans", paths_.timedBans, o + "]");
}

bool Store::SaveBanIntents() { return Write("banIntent", paths_.banIntent, PlayersJson(banIntents_)); }

std::string Store::BanStoreError() const {
    for (auto* area : {"timedBans", "banIntent"})
        if (Fenced(area)) return errors_.at(area);
    return "";
}

}  // namespace native
