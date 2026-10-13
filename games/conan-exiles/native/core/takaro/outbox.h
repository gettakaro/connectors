// Durable event outbox, ported from the Enshrouded native connector's Store
// (games/enshrouded/mod/src/native/persistence.h; the online-player, known-player and timed-ban
// areas follow with the lanes that need them).
//
// Every event the game produces is admitted here first, written to disk, and only removed when
// Takaro confirmed it: on protocol 0 the pong of a ping sent after the event was written, on
// protocol 1 an eventAck that covers its (streamId, seq). A Takaro outage or a server restart
// therefore replays the unconfirmed tail instead of losing it.
//
// Protocol 1 streams: each process admits its events into a fresh random stream, seq 1, 2, ...
// Events replayed from an earlier process keep the stream and seq they were first sent with, so
// Takaro's high-water deduplication drops any it already handled.
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
    std::string frame;   // the full protocol 0 {"type":"gameEvent",...} text
    std::string stream;  // protocol 1 streamId
    uint64_t seq = 0;    // protocol 1 seq within `stream` (>= 1)
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

    // `streamId` empty: a random one (the production case); tests pass a fixed one.
    explicit Store(std::string dir, std::string streamId = "");
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
    // Protocol 1 eventAck: removes every pending event of `stream` with seq <= upTo.
    size_t Acknowledge(const std::string& stream, uint64_t upTo);
    const std::string& StreamId() const { return streamId_; }
    bool SaveOutbox();

    std::map<std::string, std::string> Errors() const { return errors_; }
    bool Fenced(const std::string& area) const { return fenced_.count(area) > 0; }
    uint64_t WriteCount() const { return writes_; }

private:
    bool Write(const std::string& area, const std::string& path, const std::string& text);

    std::string dir_, outboxPath_, streamId_;
    uint64_t nextSeq_ = 1;
    OutboxState outbox_;
    std::map<std::string, std::string> errors_;
    std::map<std::string, bool> fenced_;
    uint64_t writes_ = 0;
};

// A random protocol 1 streamId (32 hex characters).
std::string NewStreamId();
std::string OutboxJson(const OutboxState& o);
bool ParseOutbox(const std::string& text, OutboxState& o, std::string& err);

}  // namespace takaro
