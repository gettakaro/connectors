// Durable connector state: the event outbox (scan/confirmed cursors per plugin bootId plus the
// events Takaro has not yet confirmed), online players, known players, timed bans and the
// ban-intent journal.
//
// Single owner: only the bridge thread calls a Store. Every write is tmp -> flush -> atomic
// replace (fileio.h). A file that exists but does not parse is an error: that area is fenced
// (never overwritten, never read as empty) and reported in health until an operator fixes it.
#pragma once
#include "common.h"
#include "native/adapter.h"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace native {

using EnvFn = std::function<std::string(const char*)>;

struct StatePaths {
    std::string dir, outbox, online, known, timedBans, banIntent, legacyCursor;
};
// Default dir <baseDir>\takaro\connector-state; TAKARO_STATE_DIR moves it. TAKARO_ONLINE_FILE
// points the online-players store (same format as the sidecar's) elsewhere; TAKARO_CURSOR_FILE
// names the sidecar's event cursor to import once (default <dir>\event-cursor.json).
StatePaths ResolveStatePaths(const std::string& baseDir, const EnvFn& env);

struct SourceCursor {
    std::string bootId;
    uint64_t seq = 0;
};
struct PendingEvent {
    uint64_t id = 0;       // outbox id, strictly increasing
    SourceCursor source;   // plugin ring position it came from (seq 0 for synthetic events)
    std::string type;      // Takaro gameEvent type
    std::string frame;     // the full {"type":"gameEvent",...} text
};
struct OutboxState {
    SourceCursor scan;       // next ring scan starts after this
    SourceCursor confirmed;  // newest event Takaro confirmed (a later ping's pong)
    uint64_t nextId = 1;
    uint64_t losses = 0;          // events dropped because the outbox was full
    uint64_t confirmedTotal = 0;
    bool legacyImported = false;  // sidecar cursor/online state adopted (or checked) once
    std::deque<PendingEvent> pending;
    size_t pendingBytes = 0;
};

struct LoadReport {
    bool outboxCreated = false;
    bool legacyCursorImported = false;
    std::vector<std::string> notes;
};

class Store {
public:
    static constexpr size_t kMaxPendingEvents = 5000;
    static constexpr size_t kMaxPendingBytes = 32u << 20;
    static constexpr size_t kMaxKnownPlayers = 5000;

    explicit Store(StatePaths paths) : paths_(std::move(paths)) {}
    LoadReport Load();

    const StatePaths& Paths() const { return paths_; }

    // ---- event outbox ----
    bool OutboxUsable() const { return !Fenced("outbox"); }
    OutboxState& Outbox() { return outbox_; }
    // Appends a pending event, dropping the oldest `log` event (else the oldest event) when full.
    void Admit(PendingEvent ev);
    // Removes every pending event with id <= outboxId; returns how many were confirmed.
    size_t ConfirmThrough(uint64_t outboxId);
    bool SaveOutbox();

    // ---- online / known players (Takaro-shaped player objects) ----
    std::vector<JsonValue>& Online() { return online_; }
    bool SaveOnline();
    std::vector<JsonValue>& Known() { return known_; }  // each has an extra "lastSeen" (ms)
    void Remember(const JsonValue& player, int64_t nowMs);
    bool SaveKnown();

    // ---- timed bans + ban-intent journal ----
    std::map<std::string, TimedBan>& TimedBans() { return timedBans_; }
    bool SaveTimedBans();
    std::vector<JsonValue>& BanIntents() { return banIntents_; }
    bool SaveBanIntents();
    // Non-empty when timed bans cannot be trusted (store or journal unreadable).
    std::string BanStoreError() const;

    // area -> error text; load errors fence the area, write errors clear on the next good write.
    std::map<std::string, std::string> Errors() const { return errors_; }
    bool Fenced(const std::string& area) const { return fenced_.count(area) > 0; }
    uint64_t WriteCount() const { return writes_; }

private:
    bool Write(const std::string& area, const std::string& path, const std::string& text);
    void LoadError(const std::string& area, const std::string& message);

    StatePaths paths_;
    OutboxState outbox_;
    std::vector<JsonValue> online_, known_, banIntents_;
    std::map<std::string, TimedBan> timedBans_;
    std::map<std::string, std::string> errors_;
    std::map<std::string, bool> fenced_;
    uint64_t writes_ = 0;
};

std::string OutboxJson(const OutboxState& o);
bool ParseOutbox(const std::string& text, OutboxState& o, std::string& err);

}  // namespace native
