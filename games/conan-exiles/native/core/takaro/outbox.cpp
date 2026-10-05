#include "takaro/outbox.h"

#include "common.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <algorithm>
#include <cstdlib>

namespace takaro {

namespace {
bool U64(const JsonValue* v, uint64_t& out) {
    if (!v || v->type != JsonValue::Number || v->num < 0) return false;
    out = strtoull(v->str.c_str(), nullptr, 10);
    return true;
}
}  // namespace

std::string OutboxJson(const OutboxState& o) {
    std::string pending = "[";
    bool first = true;
    for (auto& e : o.pending) {
        pending += (first ? "" : ",") +
                   ObjBuilder().Raw("id", std::to_string(e.id)).S("type", e.type).S("frame", e.frame).Done();
        first = false;
    }
    pending += "]";
    return ObjBuilder()
        .Raw("version", "1")
        .Raw("nextId", std::to_string(o.nextId))
        .Raw("losses", std::to_string(o.losses))
        .Raw("confirmedTotal", std::to_string(o.confirmedTotal))
        .Raw("pending", pending)
        .Done();
}

bool ParseOutbox(const std::string& text, OutboxState& o, std::string& err) {
    JsonValue v;
    if (!ParseJson(text, v) || v.type != JsonValue::Object) return err = "not a JSON object", false;
    const JsonValue* ver = v.get("version");
    if (!ver || ver->type != JsonValue::Number || ver->num != 1) return err = "unsupported version", false;
    OutboxState n;
    if (!U64(v.get("nextId"), n.nextId) || n.nextId == 0) return err = "missing nextId", false;
    U64(v.get("losses"), n.losses);
    U64(v.get("confirmedTotal"), n.confirmedTotal);
    const JsonValue* pend = v.get("pending");
    if (!pend || pend->type != JsonValue::Array) return err = "missing pending[]", false;
    uint64_t last = 0;
    for (auto& e : pend->arr) {
        PendingEvent pe;
        const JsonValue* t = e.get("type");
        const JsonValue* f = e.get("frame");
        if (!U64(e.get("id"), pe.id) || !t || t->type != JsonValue::String || !f || f->type != JsonValue::String)
            return err = "malformed pending event", false;
        if (pe.id <= last || pe.id >= n.nextId) return err = "pending ids out of order", false;
        JsonValue frame;
        if (!ParseJson(f->str, frame) || frame.type != JsonValue::Object) return err = "pending frame is not JSON", false;
        last = pe.id;
        pe.type = t->str;
        pe.frame = f->str;
        n.pendingBytes += pe.frame.size();
        n.pending.push_back(std::move(pe));
    }
    o = std::move(n);
    return true;
}

Store::Store(std::string dir) : dir_(std::move(dir)), outboxPath_(JoinPath(dir_, "event-outbox.json")) {}

std::string Store::Load() {
    std::string notes, err;
    if (!EnsureDirectory(dir_, err)) {
        errors_["dir"] = err;  // not fenced: every write retries until the directory is creatable
        notes += err;
    }
    std::string text;
    bool exists = false;
    if (!ReadWholeFile(outboxPath_, text, exists, err)) {
        errors_["outbox"] = err;
        fenced_["outbox"] = true;
        notes += (notes.empty() ? "" : "; ") + std::string("outbox unreadable, left untouched: ") + err;
    } else if (exists) {
        if (!ParseOutbox(text, outbox_, err)) {
            errors_["outbox"] = outboxPath_ + ": corrupt (" + err + ")";
            fenced_["outbox"] = true;
            notes += (notes.empty() ? "" : "; ") + errors_["outbox"] + ", left untouched";
        } else {
            notes += (notes.empty() ? "" : "; ") + std::string("outbox loaded: ") +
                     std::to_string(outbox_.pending.size()) + " unconfirmed event(s) to replay";
        }
    } else {
        SaveOutbox();
        notes += (notes.empty() ? "" : "; ") + std::string("outbox created at ") + outboxPath_;
    }
    return notes;
}

bool Store::Write(const std::string& area, const std::string& path, const std::string& text) {
    if (Fenced(area)) return false;
    std::string err;
    if (!EnsureDirectory(DirName(path), err) || !AtomicWriteFile(path, text, err)) {
        if (errors_[area] != err) NativeLog("state: persisting %s failed: %s", area.c_str(), err.c_str());
        errors_[area] = err;
        return false;
    }
    errors_.erase(area);
    errors_.erase("dir");
    writes_++;
    return true;
}

uint64_t Store::Admit(const std::string& type, std::string frame) {
    PendingEvent ev;
    ev.id = outbox_.nextId++;
    ev.type = type;
    ev.frame = std::move(frame);
    outbox_.pendingBytes += ev.frame.size();
    uint64_t id = ev.id;
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
    return id;
}

size_t Store::ConfirmThrough(uint64_t outboxId) {
    size_t n = 0;
    while (!outbox_.pending.empty() && outbox_.pending.front().id <= outboxId) {
        outbox_.pendingBytes -= outbox_.pending.front().frame.size();
        outbox_.pending.pop_front();
        n++;
    }
    outbox_.confirmedTotal += n;
    return n;
}

bool Store::SaveOutbox() { return Write("outbox", outboxPath_, OutboxJson(outbox_)); }

}  // namespace takaro
