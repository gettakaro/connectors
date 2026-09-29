#pragma once

#include "native_persistence.h"

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NativeBehavior {
struct View;
struct PreparedAction {
    std::string requestId, action, argsJson;
    uint64_t epoch = 0;
    uint64_t revision = 0;
    bool internalExpiry = false;
    std::string expiryPlayer;
    // Resolved by the action worker before the bridge durably journals a ban.
    std::string canonicalBanId;
    uint64_t expectedBanRevision = 0;
    std::shared_ptr<const View> view;
};
struct Effect { enum class Kind { RememberPlayer, RememberLocation, TimedBanUpsert, TimedBanRemove, BanChanged } kind;
    std::string json;
};
struct ActionOutcome {
    std::string payloadJson, errorText;
    std::vector<Effect> effects;
    bool deferredShutdown = false;
    bool mutationVerified = true;
};
struct ApplyResult {
    std::string payloadJson, errorText;
    bool deferredShutdown = false;
    bool persistenceOk = true;
};
struct MappedEvent {
    NativePersistence::SourceCursor source;
    NativeTransport::Frame frame;
    std::string type, playerId;
    bool valid = false;
    std::string error;
};
struct View {
    uint64_t revision = 0;
    std::string knownPlayersJson = "[]", onlinePlayersJson = "[]", timedBansJson = "[]";
    std::string senderName, serverName;
    std::string persistenceError;
    uint64_t banRevision = 0;
    std::map<std::string, int64_t> locationWindowUntilMs;
    std::map<std::string, std::string> lastLocationsJson;
};

// Engine's mutable state belongs to the bridge worker. ExecuteAction only uses
// PreparedAction's owned arguments and immutable View, and runs on action worker.
class Engine {
public:
    explicit Engine(NativePersistence::Store& store);
    ~Engine();
    NativePersistence::Result Load(); // call after Store::Load, before transport Start
    PreparedAction PrepareAction(std::string id, std::string action,
                                 std::string normalizedArgsJson, uint64_t epoch);
    static bool NeedsBanResolution(const PreparedAction& action);
    static std::string ResolveBanTarget(const PreparedAction& action);
    static std::optional<std::string> NormalizeGamePlayers(const std::string& rawJson);
    // Called by bridge only after queue admission, immediately before dispatch.
    NativePersistence::Result BeforeExecute(const PreparedAction& action);
    static ActionOutcome ExecuteAction(const PreparedAction& action);
    ApplyResult ApplyOutcome(const PreparedAction& action, ActionOutcome outcome);
    MappedEvent MapEvent(const std::string& ringEventJson) const;
    bool SuppressRingConnection(const std::string& eventType) const;
    NativePersistence::Result ObserveAdmitted(const MappedEvent& event);
    void NoteEventQueued(const std::string& playerId); // refresh 60 s enrichment window on replay
    std::vector<PreparedAction> DueTimedBans(int64_t nowMs);
    NativePersistence::Result SyncBanMetadata(); // bridge worker; refresh HTTP ban changes before expiry
    bool NeedsBanVerification(); // bridge timer; true only with a durable intent
    static std::string VerifyBanState(); // action worker; read both game ban lists
    NativePersistence::Result ReconcileBanIntents(const std::string& verificationJson);
    std::vector<MappedEvent> ReconcileOnline(const std::string& livePlayersJson,
                                             const std::string& bootId);
    std::vector<MappedEvent> OnRawLogLine(const std::string& line);
    bool CustomLogJoin() const;
    bool CustomLogChat() const;
    std::string HealthJson() const;
    std::shared_ptr<const View> Snapshot() const { return view_; }
private:
    NativePersistence::Result Publish();
    NativePersistence::Store& store_;
    std::shared_ptr<const View> view_;
    uint64_t revision_ = 0;
    std::string lastError_;
    std::unique_ptr<class LogState> log_;
};
} // namespace NativeBehavior
