#pragma once

#include "native_transport.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Store is single-owner: only the native bridge worker calls its methods.
// Normal successful writes have reached fsync(file), rename and fsync(directory).
// A post-rename outbox fsync failure admits a visible snapshot but fences event
// delivery until RetryDurability proves the directory entry durable.
namespace NativePersistence {
struct Paths {
    std::string directory, cursor, online, timedBans, knownPlayers, outbox, banIntent;
};
Paths ResolvePaths();

struct SourceCursor { std::string bootId; uint64_t seq = 0; };
struct DurableEvent {
    uint64_t outboxId = 0;
    SourceCursor source;
    NativeTransport::Frame frame;
};
struct Snapshot {
    uint32_t version = 1;
    uint64_t nextOutboxId = 1;
    SourceCursor scan, confirmed;
    uint64_t deliveryLosses = 0;
    std::vector<DurableEvent> pending;
    std::string derivedOnlineJson = "[]", derivedKnownJson = "[]";
    bool hasDerivedState = false;
    bool legacyBanMigrationDone = false;
};
struct Result { bool ok = false; std::string error; explicit operator bool() const { return ok; } };
struct AdmitResult : Result { uint64_t outboxId = 0; };
struct AdmitManyResult : Result { std::vector<uint64_t> outboxIds; };

class Store {
public:
    explicit Store(Paths paths = ResolvePaths());
    Result Load();
    AdmitResult Admit(SourceCursor source, NativeTransport::Frame frame,
                      size_t externalBytes=0, size_t externalCount=0);
    AdmitManyResult AdmitMany(const std::vector<std::pair<SourceCursor,NativeTransport::Frame>>& events,
                              size_t externalBytes=0, size_t externalCount=0);
    AdmitResult AdmitSynthetic(NativeTransport::Frame frame,
                               size_t externalBytes=0, size_t externalCount=0);
    Result ConfirmThrough(uint64_t outboxId);
    Result RecordLoss(SourceCursor source, const std::string& reason, uint64_t lostCount=1);
    Result Skip(SourceCursor source); // intentionally handled by log tail; no delivery loss
    Result SwitchBoot(const std::string& newBootId);
    Result MarkLegacyBanMigrationDone();
    const Snapshot& Current() const { return snapshot_; }
    const Paths& Files() const { return paths_; }
    uint64_t ErrorCount() const { return errors_; }
    const std::string& LastError() const { return lastError_; }
    bool OutboxDurabilityPending() const { return outboxDurabilityPending_; }
    bool EventOutboxDurable() const { return !outboxDurabilityPending_; }
    bool NeedsDurabilityRetry() const { return outboxDurabilityPending_ || cursorDirty_; }
    Result RetryDurability(); // retry directory fsync after a visible but unproven rename

    // Legacy files retain their exact JSON shapes. A missing file is empty;
    // existing corrupt timed-bans.json is an explicit error, never empty bans.
    Result ReadLegacy(const std::string& key, std::string& json);
    Result SaveLegacy(const std::string& key, const std::string& json);
    Result SaveKnownPlayers(const std::string& json); // authoritative snapshot + legacy mirror
    // Intent is durable before a two-file ban transition. Recovered on Load.
    Result BeginBanIntent(const std::string& json);
    Result FinishBanIntent(const std::string& requestId);
    // A later verified mutation supersedes every older intent for this ID.
    Result FinishBanIntentsForPlayer(const std::string& gameId);
    Result ReadBanIntent(std::string& json) const;
private:
    Result Commit(Snapshot next);
    Result Failure(std::string error, const std::string& category = "state");
    void Recover(const std::string& category);
    Paths paths_;
    Snapshot snapshot_;
    uint64_t errors_ = 0;
    std::string lastError_;
    std::map<std::string,std::string> activeErrors_;
    bool outboxDurabilityPending_ = false;
    bool cursorDirty_ = false;
};
#ifdef TAKARO_BRIDGE_TEST
void TestBeforeRename(std::function<void(const std::string& path)> hook);
void TestDirectorySyncFailure(std::function<bool(const std::string& path)> fail);
#endif
} // namespace NativePersistence
