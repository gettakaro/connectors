// Durable event outbox, ported from the Enshrouded native connector's Store
// (games/enshrouded/mod/src/native/persistence.h; the online-player, known-player and timed-ban
// areas follow with the lanes that need them).
//
// Every event the game produces is admitted here first, written to disk, and only removed when
// Takaro confirmed it (the pong of a ping sent after the event was written). A Takaro outage or a
// server restart therefore replays the unconfirmed tail instead of losing it.
//
// Single owner: only the bridge thread calls a Store. Every write is tmp -> fsync -> atomic rename.
// A file that exists but does not parse is fenced (never overwritten, never read as empty) and
// reported in health until an operator fixes it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace takaro {

struct PendingEvent {
    uint64_t id = 0;     // strictly increasing
    std::string type;    // Takaro gameEvent type
    std::string frame;   // the full {"type":"gameEvent",...} text
};

struct OutboxState {
    uint64_t nextId = 1;
    uint64_t losses = 0;  // events dropped because the outbox was full
    uint64_t confirmedTotal = 0;
    std::deque<PendingEvent> pending;
    size_t pendingBytes = 0;
};

class Store {
public:
    static constexpr size_t kMaxPendingEvents = 5000;
    static constexpr size_t kMaxPendingBytes = 32u << 20;

    explicit Store(std::string dir);
    // Loads the outbox. Returns notes for the log.
    std::string Load();
    const std::string& Dir() const { return dir_; }
    const std::string& OutboxPath() const { return outboxPath_; }

    bool OutboxUsable() const { return !Fenced("outbox"); }
    OutboxState& Outbox() { return outbox_; }
    // Appends an event with the next id, dropping the oldest `log` event (else the oldest) when full.
    uint64_t Admit(const std::string& type, std::string frame);
    // Removes every pending event with id <= outboxId; returns how many were confirmed.
    size_t ConfirmThrough(uint64_t outboxId);
    bool SaveOutbox();

    std::map<std::string, std::string> Errors() const { return errors_; }
    bool Fenced(const std::string& area) const { return fenced_.count(area) > 0; }
    uint64_t WriteCount() const { return writes_; }

private:
    bool Write(const std::string& area, const std::string& path, const std::string& text);

    std::string dir_, outboxPath_;
    OutboxState outbox_;
    std::map<std::string, std::string> errors_;
    std::map<std::string, bool> fenced_;
    uint64_t writes_ = 0;
};

std::string OutboxJson(const OutboxState& o);
bool ParseOutbox(const std::string& text, OutboxState& o, std::string& err);

}  // namespace takaro
