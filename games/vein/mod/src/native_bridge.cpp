#include "native_bridge.h"
#include "native_transport.h"
#include "native_behavior.h"
#include "native_persistence.h"

#include "actions.h"
#include "common.h"
#include "events.h"
#include "gamethread.h"
#include "reflect.h"
#include "state.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace NativeBridge {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Request {
    std::string id, action, args;
    uint64_t epoch = 0;
    size_t bytes = 0;
    bool full = false, shutdownCommit = false, reconcile = false, verifyBan = false,
         resolveBan = false,
         preparedReady = false;
    NativeBehavior::PreparedAction prepared;
};
struct Completion {
    std::string id, payload, error;
    uint64_t epoch = 0;
    size_t bytes = 0;
    size_t requestBytes = 0;
    bool full = false, reconcile = false, verifyBan = false, resolveBan = false;
    std::string resolvedBanId;
    NativeBehavior::PreparedAction prepared;
    NativeBehavior::ActionOutcome outcome;
};
struct Event { uint64_t seq; std::string frame; std::string locationId; uint64_t sentEpoch = 0; };
std::mutex mu;
std::condition_variable cv, actionCv, completionCv;
std::deque<NativeTransport::Notice> notices;
std::deque<NativeTransport::Notice> criticalNotices;
std::deque<Request> actions;
std::deque<Completion> completions;
std::deque<Event> outbox;
std::deque<std::string> rawLogLines;
std::deque<NativeBehavior::MappedEvent> rawMappedEvents;
// This budget covers durable events and both pre-admission raw stages. The raw
// sink runs on the background Housekeep path, never in a game hook.
std::mutex eventBudgetMu;
size_t durableBudgetBytes = 0, durableBudgetCount = 0;
size_t rawLogBytes = 0, rawMappedBytes = 0;
std::atomic<uint64_t> rawLogLosses{0};
std::unique_ptr<NativePersistence::Store> durable;
std::unique_ptr<NativeBehavior::Engine> behavior;
std::unordered_map<uint64_t, uint64_t> sentEpochByOutboxId;
std::atomic<bool> gateMode{false};
bool actionApplied = true;
struct ShutdownResponse {
    NativeTransport::Frame frame;
    std::string requestId;
    uint64_t epoch = 0;
    NativeTransport::Ticket ticket;
    Clock::time_point queuedAt, writtenAt;
    bool queued = false, written = false;
};
std::unique_ptr<ShutdownResponse> shutdownResponse;
std::unordered_set<std::string> pendingRequestIds;
std::unordered_set<std::string> completedRequestIds;
std::deque<std::string> completedRequestOrder;
std::unordered_set<std::string> expiryInFlight;
bool reconcileInFlight = false;
bool banVerifyInFlight = false;
bool banRecoveryPending = false;
std::string banMetadataError;
size_t noticeBytes = 0, actionBytes = 0, runningActions = 0, reservedActionBytes = 0, reservedActions = 0,
       eventBytes = 0, completionBytes = 0;
std::thread bridgeWorker, actionWorker;
std::atomic<bool> stopping{false}, running{false};
#ifdef TAKARO_BRIDGE_TEST
std::atomic<bool> pauseCompletions{false};
#endif
ActionHandler handler;
std::string identity, registration, serverName;
// Takaro can request a location while ingesting a join or leave. VEIN may not
// have a possessed pawn yet (or anymore), so match the sidecar's 60 s fallback.
std::mutex eventLocationMu;
std::unordered_map<std::string, Clock::time_point> eventLocationWindows;
uint64_t currentEpoch = 0, scanSeq = 0, confirmedSeq = 0, deliveryLosses = 0;
uint64_t requestCount = 0, protocolErrorCount = 0;
std::string lastRequestAction;
std::atomic<uint64_t> overloads{0};
bool identified = false;
bool liveEpoch = false;
std::string lastError;
struct Published {
    uint64_t epoch = 0, scan = 0, confirmed = 0, losses = 0;
    uint64_t requests = 0, protocolErrors = 0;
    uint64_t persistenceErrors = 0;
    uint64_t rawLogLosses = 0;
    uint32_t stateVersion = 0;
    size_t outbox = 0, outboxBytes = 0, rawMapped = 0, rawMappedBytes = 0;
    bool identified = false, recoveryPending = false, outboxDurabilityPending = false;
    std::string error, action, persistenceError, behaviorHealth = "{}", metadataError;
} published;

void Publish() {
    Published next;
    if (!gateMode && durable) {
        const auto& state = durable->Current();
        size_t durableBytes = 0;
        for (const auto& event : state.pending) if (event.frame) durableBytes += event.frame->size();
        next = {currentEpoch, state.scan.seq, state.confirmed.seq, state.deliveryLosses,
                requestCount, protocolErrorCount, durable->ErrorCount(), rawLogLosses.load(), state.version,
                state.pending.size(), durableBytes, rawMappedEvents.size(), rawMappedBytes,
                identified, banRecoveryPending, !durable->EventOutboxDurable(),
                lastError, lastRequestAction,
                durable->LastError(), behavior ? behavior->HealthJson() : "{}", banMetadataError};
    } else {
        next = {currentEpoch, scanSeq, confirmedSeq, deliveryLosses, requestCount,
                protocolErrorCount, 0, rawLogLosses.load(), 0, outbox.size(), eventBytes,
                rawMappedEvents.size(), rawMappedBytes, identified, banRecoveryPending, false, lastError,
                lastRequestAction, "", "{}", banMetadataError};
    }
    std::lock_guard<std::mutex> g(mu);
    published = std::move(next);
}

bool WithinDepth(const std::string& s) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : s) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 64) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    return depth == 0 && !quoted;
}

bool ExceedsDepth(const std::string& s) {
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char ch : s) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 64) return true; }
        else if ((ch == '}' || ch == ']') && depth) --depth;
    }
    return false;
}

Json Record(const Json& x) { return x.is_object() ? x : Json::object(); }
std::string Str(const Json& x, const char* key, const std::string& fallback = {}) {
    auto it = x.find(key);
    return it != x.end() && it->is_string() ? it->get<std::string>() : fallback;
}
Json Player(const Json& raw) {
    Json p = Record(raw);
    std::string id = Str(p, "steamId", Str(p, "gameId", Str(p, "name")));
    if (id.rfind("steam:", 0) == 0) id.erase(0, 6);
    if (id.empty()) throw std::runtime_error("player has no identifier");
    Json out = {{"gameId", id}, {"name", Str(p, "name", id)}};
    if (id.size() == 17 && id.rfind("7656", 0) == 0) {
        out["steamId"] = id; out["platformId"] = "steam:" + id;
    }
    if (p.contains("ping") && p["ping"].is_number()) out["ping"] = p["ping"];
    return out;
}
std::string PlayerId(const Json& args) {
    for (const Json& source : {Record(args), Record(Record(args).value("player", Json::object())),
                               Record(Record(args).value("playerRef", Json::object()))}) {
        std::string id = Str(source, "gameId", Str(source, "steamId", Str(source, "platformId")));
        if (!id.empty()) {
            if (id.rfind("steam:", 0) == 0) id.erase(0, 6);
            return id;
        }
    }
    throw std::runtime_error("Expected player identifier (gameId, or player.gameId)");
}
std::string ConnectionPlayerId(const Json& mapped) {
    const Json& payload = mapped.at("payload");
    const std::string type = Str(payload, "type");
    if (type != "player-connected" && type != "player-disconnected") return {};
    const Json& player = payload.at("data").at("player");
    return Str(player, "gameId");
}
void NoteLocationWindow(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> g(eventLocationMu);
    const auto now = Clock::now();
    for (auto it = eventLocationWindows.begin(); it != eventLocationWindows.end();)
        it = it->second < now ? eventLocationWindows.erase(it) : ++it;
    if (eventLocationWindows.size() >= 500) eventLocationWindows.erase(eventLocationWindows.begin());
    eventLocationWindows[id] = now + std::chrono::seconds(60);
}
bool HasLocationWindow(const std::string& id) {
    std::lock_guard<std::mutex> g(eventLocationMu);
    auto it = eventLocationWindows.find(id);
    if (it == eventLocationWindows.end()) return false;
    if (Clock::now() > it->second) { eventLocationWindows.erase(it); return false; }
    return true;
}
Json MapEvent(const Json& e) {
    std::string type = Str(e, "type");
    Json d = Record(e.value("data", Json::object()));
    Json out;
    if (type == "log") out = {{"msg", Str(d, "msg", Str(d, "message"))}};
    else if (type == "player-connected" || type == "player-disconnected")
        out = {{"player", Player(d.value("player", d))}};
    else if (type == "chat-message") {
        std::string channel = Str(d, "channel", "global");
        if (channel == "local" || channel == "radio") channel = "team";
        if (channel != "team" && channel != "friends" && channel != "whisper") channel = "global";
        out = {{"msg", Str(d, "msg", Str(d, "message"))}, {"channel", channel}};
        if (d.contains("player") && d["player"].is_object()) out["player"] = Player(d["player"]);
    } else if (type == "player-death") {
        out = {{"player", Player(d.value("player", d))}};
        if (d.contains("attacker") && d["attacker"].is_object()) {
            try { out["attacker"] = Player(d["attacker"]); } catch (...) {}
        }
        if (d.contains("position")) out["position"] = d["position"];
        if (!out.contains("attacker")) {
            std::string who = Str(out["player"], "name");
            std::string killer = Str(d, "killerEntity");
            out["msg"] = killer.empty() ? who + " died" : who + " was killed by " + killer;
        }
    } else if (type == "entity-killed") {
        Json entity = d.value("entity", Json("unknown"));
        std::string code = entity.is_string() ? entity.get<std::string>() : Str(Record(entity), "code", "unknown");
        out = {{"player", Player(d.value("player", d))},
               {"entity", code}, {"weapon", Str(d, "weapon")}};
    } else throw std::runtime_error("unsupported event type");
    if (e.contains("ts") && e["ts"].is_string()) out["timestamp"] = e["ts"];
    return {{"type", "gameEvent"}, {"payload", {{"type", type}, {"data", out}}}};
}

void SendError(const std::string& id, uint64_t epoch, const std::string& message) {
    if (id.empty()) return;
    Json frame = {{"type", "response"}, {"requestId", id}, {"error", message}};
    auto result = NativeTransport::Queue({NativeTransport::Kind::CriticalResponse,
        std::make_shared<const std::string>(frame.dump()), epoch, 0, false});
    if (!result) {
        ++overloads;
        NativeTransport::RequestClose(epoch, 1013, "overloaded");
    }
}

void PruneDroppedEvents() {
    if (gateMode || !durable || !currentEpoch) return;
    const auto& state = durable->Current();
    const uint64_t first = state.pending.empty() ? state.nextOutboxId : state.pending.front().outboxId;
    NativeTransport::PruneEventsBefore(first, currentEpoch);
    for (auto it = sentEpochByOutboxId.begin(); it != sentEpochByOutboxId.end();)
        it = it->first < first ? sentEpochByOutboxId.erase(it) : ++it;
}

void RefreshDurableBudget() {
    // Caller owns eventBudgetMu. Store itself is bridge-thread-only.
    durableBudgetCount = durable->Current().pending.size();
    durableBudgetBytes = 0;
    for (const auto& event : durable->Current().pending)
        if (event.frame) durableBudgetBytes += event.frame->size();
}

std::pair<size_t, size_t> ExternalBudget() {
    // Caller owns eventBudgetMu; rawLogLines is shared with the hook.
    std::lock_guard<std::mutex> g(mu);
    return {rawLogBytes + rawMappedBytes, rawLogLines.size() + rawMappedEvents.size()};
}

void HandleFrame(const NativeTransport::Notice& n) {
    if (n.text.size() > 1024*1024 || !WithinDepth(n.text)) { lastError = "invalid JSON size or depth"; return; }
    Json f = Json::parse(n.text, nullptr, false);
    if (f.is_discarded() || !f.is_object()) { lastError = "malformed Takaro frame"; return; }
    std::string type = Str(f, "type");
    if (type == "identifyResponse") {
        Json p = Record(f.value("payload", Json::object()));
        if (p.contains("error") && !p["error"].is_null()) {
            lastError = "Takaro rejected identity";
            NativeTransport::RequestClose(n.epoch, 1013, "identity rejected");
            return;
        }
        identified = true;
        PluginLog("native: identified with Takaro (epoch %llu)", (unsigned long long)n.epoch);
    } else if (type == "error") {
        ++protocolErrorCount;
        const Json p = Record(f.value("payload", Json::object()));
        std::string detail = Str(p, "message", Str(p, "error", Str(f, "error")));
        if (detail.empty()) detail = "unspecified protocol error";
        if (!identity.empty()) for (size_t at = 0; (at = detail.find(identity, at)) != std::string::npos;) {
            detail.replace(at, identity.size(), "[redacted]"); at += sizeof("[redacted]") - 1;
        }
        if (!registration.empty()) for (size_t at = 0; (at = detail.find(registration, at)) != std::string::npos;) {
            detail.replace(at, registration.size(), "[redacted]"); at += sizeof("[redacted]") - 1;
        }
        if (detail.size() > 512) detail.resize(512);
        lastError = "Takaro protocol error: " + detail;
    } else if (type == "ping") {
        if (!NativeTransport::Queue(NativeTransport::Kind::Control, R"({"type":"pong"})", 0, n.epoch))
            NativeTransport::RequestClose(n.epoch, 1013, "control queue full");
    } else if (type == "request") {
        std::string id = Str(f, "requestId");
        Json p = Record(f.value("payload", Json::object()));
        std::string action = Str(p, "action");
        ++requestCount;
        lastRequestAction = action.substr(0, 64);
        if (id.empty()) return;
        if (id.size() > 128) {
            lastError = "requestId exceeds 128 bytes";
            NativeTransport::RequestClose(n.epoch, 1013, "requestId too long");
            return;
        }
        if (!identified || action.empty()) { SendError(id, n.epoch, "invalid request"); return; }
        Json args = p.value("args", Json::object());
        if (args.is_string()) {
            std::string nested = args.get<std::string>();
            if (ExceedsDepth(nested)) { SendError(id, n.epoch, "argument JSON depth exceeds 64"); return; }
            args = Json::parse(nested, nullptr, false);
        }
        args = Record(args);
        std::string serialized = args.dump();
        size_t bytes = serialized.size() + id.size() + action.size();
        std::lock_guard<std::mutex> g(mu);
        if (!gateMode && (pendingRequestIds.count(id) || completedRequestIds.count(id))) {
            SendError(id, n.epoch, "duplicate requestId"); return;
        }
        if ((!gateMode && pendingRequestIds.size() >= 128) ||
            actions.size() + runningActions + reservedActions >= 128 ||
            actionBytes + reservedActionBytes + bytes > 4*1024*1024) {
            ++overloads; SendError(id, n.epoch, "native action queue overloaded"); return;
        }
        actionBytes += bytes;
        Request request;
        request.id = id; request.action = action; request.args = std::move(serialized);
        request.epoch = n.epoch; request.bytes = bytes; request.full = !gateMode;
        request.preparedReady = gateMode;
        if (!gateMode) pendingRequestIds.insert(id);
        actions.push_back(std::move(request));
        actionCv.notify_one();
    }
}

void PollEvents() {
    // Reading and parsing the ring is bridge-thread work, never game-thread work.
    if (!gateMode && !durable->EventOutboxDurable()) return;
    Json ring = Json::parse(PluginState::Get().EventsJson(scanSeq, 256), nullptr, false);
    if (!ring.is_object() || !ring.contains("events") || !ring["events"].is_array()) return;
    if (!gateMode) {
        const std::string boot = Str(ring, "bootId", BootId());
        if (durable->Current().scan.bootId != boot) {
            NativePersistence::Result switched;
            {
                std::lock_guard<std::mutex> budget(eventBudgetMu);
                switched = durable->SwitchBoot(boot);
                if (switched) RefreshDurableBudget();
            }
            if (!switched) { lastError = switched.error; return; }
            scanSeq = durable->Current().scan.seq;
            ring = Json::parse(PluginState::Get().EventsJson(scanSeq, 256), nullptr, false);
            if (!ring.is_object() || !ring.contains("events") || !ring["events"].is_array()) return;
        }
        if (ring.value("truncated", false) && !ring["events"].empty()) {
            uint64_t first = ring["events"].front().value("seq", uint64_t{0});
            if (first > scanSeq + 1) {
                NativePersistence::Result lost;
                {
                    std::lock_guard<std::mutex> budget(eventBudgetMu);
                    lost = durable->RecordLoss({boot, first - 1}, "event ring truncated",
                                               first - scanSeq - 1);
                    if (lost) RefreshDurableBudget();
                }
                if (!lost) { lastError = lost.error; return; }
                PruneDroppedEvents();
                scanSeq = durable->Current().scan.seq;
            }
        }
        std::vector<NativeBehavior::MappedEvent> batch;
        std::vector<std::pair<NativePersistence::SourceCursor, NativeTransport::Frame>> frames;
        size_t batchBytes = 0;
        auto admitBatch = [&]() -> bool {
            if (frames.empty()) return true;
            NativePersistence::AdmitManyResult admitted;
            {
                std::lock_guard<std::mutex> budget(eventBudgetMu);
                auto [externalBytes, externalCount] = ExternalBudget();
                admitted = durable->AdmitMany(frames, externalBytes, externalCount);
                if (admitted) RefreshDurableBudget();
            }
            if (!admitted) { lastError = admitted.error; return false; }
            PruneDroppedEvents();
            for (const auto& mapped : batch) {
                auto observed = behavior->ObserveAdmitted(mapped);
                if (!observed) lastError = observed.error;
            }
            scanSeq = durable->Current().scan.seq;
            batch.clear(); frames.clear(); batchBytes = 0;
            return true;
        };
        for (const Json& e : ring["events"]) {
            uint64_t seq = e.value("seq", uint64_t{0});
            if (seq <= scanSeq) continue;
            if (behavior->SuppressRingConnection(Str(e, "type"))) {
                if (!admitBatch()) return;
                NativePersistence::Result skipped;
                {
                    std::lock_guard<std::mutex> budget(eventBudgetMu);
                    skipped = durable->Skip({boot, seq});
                    if (skipped) RefreshDurableBudget();
                }
                if (!skipped) { lastError = skipped.error; return; }
                scanSeq = durable->Current().scan.seq;
                continue;
            }
            auto mapped = behavior->MapEvent(e.dump());
            mapped.source = {boot, seq};
            if (!mapped.valid || !mapped.frame || mapped.frame->size() > 32*1024*1024) {
                if (!admitBatch()) return;
                if (mapped.frame && mapped.frame->size() > 32*1024*1024)
                    mapped.error = "event exceeds 32 MiB";
                NativePersistence::Result lost;
                {
                    std::lock_guard<std::mutex> budget(eventBudgetMu);
                    lost = durable->RecordLoss({boot, seq}, mapped.error.empty() ? "invalid event" : mapped.error);
                    if (lost) RefreshDurableBudget();
                }
                if (!lost) { lastError = lost.error; return; }
                PruneDroppedEvents();
                scanSeq = durable->Current().scan.seq;
            } else {
                if (batchBytes && batchBytes + mapped.frame->size() > 32*1024*1024)
                    if (!admitBatch()) return;
                batchBytes += mapped.frame->size();
                frames.push_back({mapped.source, mapped.frame});
                batch.push_back(std::move(mapped));
            }
        }
        if (!admitBatch()) return;
        return;
    }
    if (ring.value("truncated", false)) { ++deliveryLosses; PluginLog("native: event ring truncated since %llu", (unsigned long long)scanSeq); }
    for (const Json& e : ring["events"]) {
        uint64_t seq = e.value("seq", uint64_t{0});
        if (seq <= scanSeq) continue;
        scanSeq = seq;
        try {
            Json mapped = MapEvent(e);
            std::string frame = mapped.dump();
            while (outbox.size() >= 5000 || eventBytes + frame.size() > 31*1024*1024) {
                if (outbox.empty()) break;
                eventBytes -= outbox.front().frame.size(); outbox.pop_front(); ++deliveryLosses;
            }
            if (frame.size() > 1024*1024) { ++deliveryLosses; continue; }
            std::string locationId = ConnectionPlayerId(mapped);
            eventBytes += frame.size(); outbox.push_back({seq, std::move(frame), std::move(locationId), 0});
        } catch (const std::exception& ex) { ++deliveryLosses; PluginLog("native: event %llu dropped: %s", (unsigned long long)seq, ex.what()); }
    }
}

void FlushEvents() {
    if (!identified) return;
    if (!gateMode) {
        if (!durable->EventOutboxDurable()) return;
        for (const auto& e : durable->Current().pending) {
            if (sentEpochByOutboxId[e.outboxId] == currentEpoch) continue;
            auto queued = NativeTransport::Queue({NativeTransport::Kind::Event, e.frame,
                                                  currentEpoch, e.outboxId, false});
            if (!queued) break;
            sentEpochByOutboxId[e.outboxId] = currentEpoch;
            Json frame = Json::parse(*e.frame, nullptr, false);
            if (frame.is_object()) {
                Json payload = Record(frame.value("payload", Json::object()));
                std::string type = Str(payload, "type");
                if (type == "player-connected" || type == "player-disconnected") {
                    Json data = Record(payload.value("data", Json::object()));
                    behavior->NoteEventQueued(Str(Record(data.value("player", Json::object())), "gameId"));
                }
            }
        }
        return;
    }
    for (auto& e : outbox) {
        if (e.sentEpoch == currentEpoch) continue;
        if (!NativeTransport::Queue(NativeTransport::Kind::Event, e.frame, e.seq, currentEpoch)) break;
        NoteLocationWindow(e.locationId);
        e.sentEpoch = currentEpoch;
    }
}

bool PollRawLogs() {
    if (gateMode || !behavior || !durable) return false;
    if (!durable->EventOutboxDurable()) return false;
    std::lock_guard<std::mutex> budget(eventBudgetMu);
    // Retry events whose parsing already advanced the log parser before reading
    // another line. Count the retried frame only once when moving it to Store.
    while (!rawMappedEvents.empty()) {
        auto& event = rawMappedEvents.front();
        auto [externalBytes, externalCount] = ExternalBudget();
        auto admitted = durable->AdmitSynthetic(event.frame,
            externalBytes - event.frame->size(), externalCount - 1);
        if (!admitted) { lastError = admitted.error; return false; }
        rawMappedBytes -= event.frame->size();
        auto mapped = std::move(event);
        rawMappedEvents.pop_front();
        RefreshDurableBudget(); PruneDroppedEvents();
        auto observed = behavior->ObserveAdmitted(mapped);
        if (!observed) lastError = observed.error;
    }
    std::string line;
    {
        std::lock_guard<std::mutex> g(mu);
        if (!rawLogLines.empty()) {
            line = std::move(rawLogLines.front());
            rawLogBytes -= line.size();
            rawLogLines.pop_front();
        }
    }
    if (!line.empty()) {
        bool blocked = false;
        std::vector<NativeBehavior::MappedEvent> mapped;
        try { mapped = behavior->OnRawLogLine(line); }
        catch (const std::exception& ex) {
            lastError = std::string("raw log parsing failed: ") + ex.what();
            ++rawLogLosses;
            return false;
        }
        catch (...) { lastError = "raw log parsing failed"; ++rawLogLosses; return false; }
        for (auto& event : mapped) {
            if (!event.valid || !event.frame) { ++rawLogLosses; continue; }
            if (event.frame->size() > 32*1024*1024) { ++rawLogLosses; continue; }
            auto [externalBytes, externalCount] = ExternalBudget();
            if (!blocked) {
                auto admitted = durable->AdmitSynthetic(event.frame, externalBytes, externalCount);
                if (admitted) {
                    RefreshDurableBudget(); PruneDroppedEvents();
                    auto observed = behavior->ObserveAdmitted(event);
                    if (!observed) lastError = observed.error;
                    continue;
                }
                lastError = admitted.error;
                blocked = true;
            }
            if (durableBudgetBytes + externalBytes + event.frame->size() <= 32*1024*1024 &&
                durableBudgetCount + externalCount < 5000) {
                rawMappedBytes += event.frame->size();
                rawMappedEvents.push_back(std::move(event));
            } else ++rawLogLosses;
        }
        if (blocked) return false;
    }
    return true;
}

void ScheduleTimedBanExpiry() {
    if (gateMode || !behavior) return;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    for (auto& prepared : behavior->DueTimedBans(nowMs)) {
        if (prepared.expiryPlayer.empty() || expiryInFlight.count(prepared.expiryPlayer)) continue;
        const size_t bytes = prepared.argsJson.size() + prepared.action.size();
        {
            std::lock_guard<std::mutex> g(mu);
            if (!actionApplied || !actions.empty() ||
                actions.size() + runningActions + reservedActions >= 128 ||
                actionBytes + reservedActionBytes + bytes > 4*1024*1024) break;
        }
        auto intent = behavior->BeforeExecute(prepared);
        if (!intent) { lastError = intent.error; break; }
        Request r;
        r.action = prepared.action; r.args = prepared.argsJson;
        r.epoch = currentEpoch; r.bytes = bytes; r.full = true; r.preparedReady = true;
        r.prepared = std::move(prepared);
        expiryInFlight.insert(r.prepared.expiryPlayer);
        { std::lock_guard<std::mutex> g(mu); actionBytes += bytes; actions.push_back(std::move(r)); }
        actionCv.notify_one();
    }
}

void ApplyReconciliation(const std::string& livePlayersJson) {
    if (gateMode || !behavior || !durable) return;
    for (auto& event : behavior->ReconcileOnline(livePlayersJson, BootId())) {
        if (!event.valid || !event.frame) { ++rawLogLosses; continue; }
        if (event.frame->size() > 32*1024*1024) { ++rawLogLosses; continue; }
        std::lock_guard<std::mutex> budget(eventBudgetMu);
        auto [externalBytes, externalCount] = ExternalBudget();
        auto admitted = durable->AdmitSynthetic(event.frame, externalBytes, externalCount);
        if (!admitted) { lastError = admitted.error; return; }
        RefreshDurableBudget();
        PruneDroppedEvents();
        auto observed = behavior->ObserveAdmitted(event);
        if (!observed) lastError = observed.error;
    }
}

void ScheduleReconciliation() {
    if (gateMode || !identified || !behavior || reconcileInFlight) return;
    std::lock_guard<std::mutex> g(mu);
    if (actions.size() + runningActions + reservedActions >= 128) return;
    Request r;
    r.reconcile = true; r.preparedReady = true; r.epoch = currentEpoch;
    actions.push_back(std::move(r));
    reconcileInFlight = true;
    actionCv.notify_one();
}

void ScheduleBanVerification() {
    if (gateMode || !behavior || banVerifyInFlight) return;
    bool needed = false;
    try { needed = behavior->NeedsBanVerification(); banRecoveryPending = needed; }
    catch (const std::exception& ex) { lastError = ex.what(); return; }
    catch (...) { lastError = "ban verification state failed"; return; }
    if (!needed) return;
    std::lock_guard<std::mutex> g(mu);
    if (!actionApplied || !actions.empty() || runningActions || reservedActions ||
        !completions.empty()) return;
    Request r;
    r.verifyBan = true; r.preparedReady = true; r.epoch = currentEpoch;
    actions.push_back(std::move(r));
    banVerifyInFlight = true;
    actionCv.notify_one();
}

void PrepareNextAction() {
    if (gateMode || !behavior) return;
    for (;;) {
        std::string id, action, args;
        uint64_t atEpoch = 0;
        {
            std::lock_guard<std::mutex> g(mu);
            if (!actionApplied || actions.empty() || actions.front().preparedReady) return;
            id = actions.front().id; action = actions.front().action;
            args = actions.front().args; atEpoch = actions.front().epoch;
        }
        std::string error;
        NativeBehavior::PreparedAction prepared;
        bool resolveBan = false;
        if (atEpoch != currentEpoch || !liveEpoch) error = "request connection expired";
        else {
            try {
                prepared = behavior->PrepareAction(id, action, args, atEpoch);
                resolveBan = NativeBehavior::Engine::NeedsBanResolution(prepared);
                if (!resolveBan) {
                    auto ready = behavior->BeforeExecute(prepared);
                    if (!ready) error = ready.error;
                }
            } catch (const std::exception& ex) { error = ex.what(); }
              catch (...) { error = "native preparation failed"; }
        }
        {
            std::lock_guard<std::mutex> g(mu);
            if (error.empty()) {
                actions.front().prepared = std::move(prepared);
                actions.front().resolveBan = resolveBan;
                actions.front().preparedReady = true;
                actionCv.notify_one();
                return;
            }
            actionBytes -= actions.front().bytes;
            actions.pop_front();
            pendingRequestIds.erase(id);
        }
        if (atEpoch == currentEpoch && liveEpoch) SendError(id, atEpoch, error);
    }
}

void CommitShutdown() {
    Request job;
    job.shutdownCommit = true;
    job.preparedReady = true;
    { std::lock_guard<std::mutex> g(mu); actions.push_front(std::move(job)); }
    actionCv.notify_one();
    shutdownResponse.reset();
}

void PumpShutdown() {
    if (!shutdownResponse) return;
    auto& pending = *shutdownResponse;
    const auto now = Clock::now();
    if (!pending.queued) {
        if (pending.epoch != currentEpoch || now - pending.queuedAt >= std::chrono::seconds(2)) {
            lastError = "shutdown response could not be queued within 2 seconds; shutdown cancelled";
            shutdownResponse.reset();
            return;
        }
        auto queued = NativeTransport::Queue({NativeTransport::Kind::CriticalResponse, pending.frame,
                                               pending.epoch, 0, true});
        if (queued) { pending.queued = true; pending.ticket = queued.ticket; }
        else if (queued.status != NativeTransport::QueueStatus::Full) {
            lastError = "shutdown response could not be queued; shutdown cancelled";
            shutdownResponse.reset();
        }
        return;
    }
    if (!pending.written && now - pending.queuedAt >= std::chrono::seconds(2)) {
        lastError = "shutdown response was not written within 2 seconds; shutdown cancelled";
        NativeTransport::RequestClose(pending.epoch, 1013, "shutdown response timeout");
        shutdownResponse.reset();
    } else if (pending.written && now - pending.writtenAt >= std::chrono::seconds(2)) {
        lastError = "shutdown response written but pong not received within 2 seconds";
        CommitShutdown();
    }
}

void BridgeLoop() {
    auto nextPoll = Clock::now();
    auto nextExpiry = Clock::now(), nextReconcile = Clock::now(), nextBanIntent = Clock::now(),
         nextBanMetadata = Clock::now(),
         nextDurability = Clock::now();
    while (!stopping) {
      try {
        NativeTransport::Notice n;
        bool have = false;
        {
            std::unique_lock<std::mutex> g(mu);
            cv.wait_for(g, std::chrono::milliseconds(100), [] {
#ifdef TAKARO_BRIDGE_TEST
                if (pauseCompletions) return stopping || !criticalNotices.empty() || !notices.empty();
#endif
                return stopping || !criticalNotices.empty() || !notices.empty() || !completions.empty();
            });
            if (!criticalNotices.empty()) {
                n = std::move(criticalNotices.front()); criticalNotices.pop_front(); have = true;
            } else if (!notices.empty()) {
                n = std::move(notices.front()); noticeBytes -= n.text.size(); notices.pop_front(); have = true;
            }
        }
        if (have) {
            if (n.type == NativeTransport::NoticeType::Open) {
                currentEpoch = n.epoch; identified = false; liveEpoch = true;
                sentEpochByOutboxId.clear();
                Json p = {{"identityToken", identity}};
                if (!registration.empty()) p["registrationToken"] = registration;
                if (!serverName.empty()) p["name"] = serverName;
                if (!NativeTransport::Queue(NativeTransport::Kind::Control,
                                            Json({{"type", "identify"}, {"payload", p}}).dump(), 0, n.epoch))
                    NativeTransport::RequestClose(n.epoch, 1013, "identify queue full");
            } else if (n.type == NativeTransport::NoticeType::Closed && n.epoch == currentEpoch) {
                identified = false; liveEpoch = false;
                if (gateMode) for (auto& e : outbox) e.sentEpoch = 0;
                else sentEpochByOutboxId.clear();
            } else if (n.type == NativeTransport::NoticeType::Frame && n.epoch == currentEpoch && liveEpoch) {
                try { HandleFrame(n); } catch (const std::exception& ex) { lastError = ex.what(); }
            } else if (n.type == NativeTransport::NoticeType::Written && shutdownResponse &&
                       n.pingAfterWrite && n.ticket.epoch == shutdownResponse->ticket.epoch &&
                       n.ticket.writeId == shutdownResponse->ticket.writeId) {
                shutdownResponse->written = true;
                shutdownResponse->writtenAt = Clock::now();
            } else if (n.type == NativeTransport::NoticeType::Confirmed && n.epoch == currentEpoch) {
                if (gateMode) {
                    while (!outbox.empty() && outbox.front().sentEpoch == n.epoch && outbox.front().seq <= n.eventSeq) {
                        confirmedSeq = outbox.front().seq;
                        eventBytes -= outbox.front().frame.size(); outbox.pop_front();
                    }
                } else if (!durable->Current().pending.empty() &&
                           durable->Current().pending.front().outboxId <= n.outboxId) {
                    NativePersistence::Result result;
                    {
                        std::lock_guard<std::mutex> budget(eventBudgetMu);
                        result = durable->ConfirmThrough(n.outboxId);
                        if (result) RefreshDurableBudget();
                    }
                    if (!result) lastError = result.error;
                    else for (auto it = sentEpochByOutboxId.begin(); it != sentEpochByOutboxId.end();)
                        it = it->first <= n.outboxId ? sentEpochByOutboxId.erase(it) : ++it;
                    if (result) PruneDroppedEvents();
                }
                if (shutdownResponse && shutdownResponse->written &&
                    n.ticket.epoch == shutdownResponse->ticket.epoch &&
                    n.ticket.writeId == shutdownResponse->ticket.writeId) CommitShutdown();
            } else if (n.type == NativeTransport::NoticeType::Error) lastError = n.text;
        }
        Completion completed;
        bool hasCompletion = false;
        {
            std::lock_guard<std::mutex> g(mu);
            if (!completions.empty()
#ifdef TAKARO_BRIDGE_TEST
                && !pauseCompletions
#endif
            ) {
                completed = std::move(completions.front()); completions.pop_front();
                completionBytes -= completed.bytes; hasCompletion = true; completionCv.notify_one();
            }
        }
        if (hasCompletion) {
            const bool fresh = completed.epoch == currentEpoch && liveEpoch;
            if (completed.verifyBan) {
                banVerifyInFlight = false;
                if (completed.error.empty()) {
                    try {
                        auto reconciled = behavior->ReconcileBanIntents(completed.payload);
                        if (!reconciled) lastError = reconciled.error;
                        banRecoveryPending = behavior->NeedsBanVerification();
                    } catch (const std::exception& ex) { lastError = ex.what(); }
                      catch (...) { lastError = "ban verification failed"; }
                } else lastError = completed.error;
                { std::lock_guard<std::mutex> g(mu);
                  if (runningActions) --runningActions;
                  actionApplied = true; }
                actionCv.notify_one();
                Publish();
                continue;
            }
            if (completed.resolveBan) {
                std::string error = completed.error;
                if (!fresh && error.empty()) error = "request connection expired";
                if (error.empty()) {
                    try {
                        completed.prepared.canonicalBanId = std::move(completed.resolvedBanId);
                        auto ready = behavior->BeforeExecute(completed.prepared);
                        if (!ready) error = ready.error;
                    } catch (const std::exception& ex) { error = ex.what(); }
                      catch (...) { error = "ban intent persistence failed"; }
                }
                {
                    std::lock_guard<std::mutex> g(mu);
                    if (runningActions) --runningActions;
                    if (error.empty()) {
                        try {
                            Request next;
                            next.id = completed.id; next.action = completed.prepared.action;
                            next.args = completed.prepared.argsJson; next.epoch = completed.epoch;
                            next.bytes = completed.requestBytes; next.full = true;
                            next.preparedReady = true;
                            next.prepared = std::move(completed.prepared);
                            actions.push_front(std::move(next));
                        } catch (...) { error = "ban mutation could not be queued"; }
                    }
                    if (!error.empty()) {
                        actionBytes -= completed.requestBytes;
                        pendingRequestIds.erase(completed.id);
                        if (!completed.id.empty() && completedRequestIds.insert(completed.id).second) {
                            completedRequestOrder.push_back(completed.id);
                            if (completedRequestOrder.size() > 128) {
                                completedRequestIds.erase(completedRequestOrder.front());
                                completedRequestOrder.pop_front();
                            }
                        }
                    }
                    actionApplied = true;
                }
                actionCv.notify_one();
                if (fresh && !error.empty()) SendError(completed.id, currentEpoch, error);
                if (!error.empty()) lastError = error;
                Publish();
                continue;
            }
            NativeBehavior::ApplyResult applied;
            if (completed.reconcile) {
                reconcileInFlight = false;
                if (fresh && completed.error.empty()) {
                    try { ApplyReconciliation(completed.payload); }
                    catch (const std::exception& ex) { lastError = ex.what(); }
                }
            }
            if (completed.full) {
                try { applied = behavior->ApplyOutcome(completed.prepared, std::move(completed.outcome)); }
                catch (const std::exception& ex) { applied.errorText = ex.what(); lastError = ex.what(); }
                catch (...) { applied.errorText = "native apply failed"; lastError = applied.errorText; }
                if (!completed.error.empty()) applied.errorText = completed.error;
                if (completed.prepared.internalExpiry) expiryInFlight.erase(completed.prepared.expiryPlayer);
            }
            { std::lock_guard<std::mutex> g(mu);
              actionBytes -= completed.requestBytes;
              if (runningActions) --runningActions;
              if (completed.full) {
                  pendingRequestIds.erase(completed.id);
                  if (!completed.id.empty() && completedRequestIds.insert(completed.id).second) {
                      completedRequestOrder.push_back(completed.id);
                      if (completedRequestOrder.size() > 128) {
                          completedRequestIds.erase(completedRequestOrder.front());
                          completedRequestOrder.pop_front();
                      }
                  }
              }
              actionApplied = true; }
            actionCv.notify_one();
            if (fresh && !completed.id.empty()) {
                Json response = {{"type", "response"}, {"requestId", completed.id}};
                const std::string& error = completed.full ? applied.errorText : completed.error;
                const std::string& payload = completed.full ? applied.payloadJson : completed.payload;
                if (error.empty()) response["payload"] = Json::parse(payload, nullptr, false);
                else response["error"] = error;
                std::string encoded = response.dump();
                if (encoded.size() > 8*1024*1024) SendError(completed.id, currentEpoch, "response exceeds 8 MiB");
                else if (completed.full && applied.deferredShutdown && error.empty()) {
                    if (shutdownResponse) SendError(completed.id, currentEpoch, "shutdown already pending");
                    else {
                        shutdownResponse = std::make_unique<ShutdownResponse>();
                        shutdownResponse->frame = std::make_shared<const std::string>(std::move(encoded));
                        shutdownResponse->requestId = completed.id;
                        shutdownResponse->epoch = currentEpoch;
                        shutdownResponse->queuedAt = Clock::now();
                        PumpShutdown();
                    }
                } else {
                    auto queued = NativeTransport::Queue({NativeTransport::Kind::Response,
                        std::make_shared<const std::string>(std::move(encoded)), currentEpoch, 0, false});
                    if (!queued) SendError(completed.id, currentEpoch, "native response queue overloaded");
                }
            }
        }
        if (Clock::now() >= nextPoll) { PollEvents(); nextPoll = Clock::now() + std::chrono::seconds(1); }
        for (int i = 0; i < 32; ++i) {
            bool hasRaw;
            { std::lock_guard<std::mutex> g(mu); hasRaw = !rawLogLines.empty(); }
            if (!hasRaw && rawMappedEvents.empty()) break;
            if (!PollRawLogs()) break;
        }
        if (!gateMode && Clock::now() >= nextBanMetadata) {
            try {
                auto synced = behavior->SyncBanMetadata();
                banMetadataError = synced ? "" : synced.error;
            } catch (const std::exception& ex) { banMetadataError = ex.what(); }
              catch (...) { banMetadataError = "ban metadata synchronization failed"; }
            if (!banMetadataError.empty()) lastError = banMetadataError;
            nextBanMetadata = Clock::now() + std::chrono::seconds(2);
        }
        if (!gateMode && Clock::now() >= nextExpiry) {
            if (banMetadataError.empty()) ScheduleTimedBanExpiry();
            nextExpiry = Clock::now() + std::chrono::seconds(1);
        }
        if (!gateMode && Clock::now() >= nextBanIntent) {
            ScheduleBanVerification();
            nextBanIntent = Clock::now() + std::chrono::seconds(2);
        }
        if (!gateMode && Clock::now() >= nextDurability) {
            if (durable->NeedsDurabilityRetry()) {
                auto retried = durable->RetryDurability();
                if (!retried) lastError = retried.error;
            }
            nextDurability = Clock::now() + std::chrono::seconds(2);
        }
        if (!gateMode && identified && Clock::now() >= nextReconcile) {
            ScheduleReconciliation(); nextReconcile = Clock::now() + std::chrono::seconds(30);
        }
        PrepareNextAction();
        FlushEvents();
        PumpShutdown();
        FlushPluginLogs();
        Publish();
      } catch (const std::exception& ex) {
          lastError = ex.what(); Publish();
      } catch (...) {
          lastError = "native bridge worker failed"; Publish();
      }
    }
}

std::string GateAction(const std::string& action, const std::string& encodedArgs) {
    if (action == "testReachability") {
        bool ready = GameThread::Alive() && Reflect::Validated() &&
                     PluginState::Get().Capability("gameThread") == "ok";
        return Json({{"connectable", ready}, {"reason", ready ? Json("native gate experimental; persistence unavailable")
                                                            : Json("game thread unavailable")}}).dump();
    }
    if (action == "getPlayers") {
        Actions::Result r = Actions::Players();
        if (r.status != 200) throw std::runtime_error("getPlayers: " + r.body);
        Json rows = Json::parse(r.body);
        Json mapped = Json::array();
        for (const Json& row : rows) if (!row.contains("online") || row["online"] != false) mapped.push_back(Player(row));
        return mapped.dump();
    }
    if (action == "getPlayerLocation") {
        const std::string id = PlayerId(Json::parse(encodedArgs));
        Actions::Result r = Actions::PlayerLocation(id);
        if (r.status == 200) {
            Json raw = Json::parse(r.body);
            if (raw.contains("x") && raw.contains("y") && raw.contains("z") &&
                raw["x"].is_number() && raw["y"].is_number() && raw["z"].is_number())
                return Json({{"x", raw["x"]}, {"y", raw["y"]}, {"z", raw["z"]}}).dump();
        }
        if (HasLocationWindow(id)) return R"({"x":0,"y":0,"z":0})";
        throw std::runtime_error("getPlayerLocation: " + r.body);
    }
    throw std::runtime_error("native gate action unavailable: " + action);
}

void ActionLoop() {
    while (!stopping) {
      Request r;
      bool started = false;
      try {
        {
            std::unique_lock<std::mutex> g(mu);
            actionCv.wait(g, [] { return stopping || (!actions.empty() && actionApplied &&
                                              actions.front().preparedReady); });
            if (stopping) break;
            r = std::move(actions.front()); actions.pop_front();
            actionApplied = false;
            ++runningActions;
            started = true;
        }
        if (r.shutdownCommit) {
            Actions::Shutdown();
            { std::lock_guard<std::mutex> g(mu); if (runningActions) --runningActions; actionApplied = true; }
            actionCv.notify_one();
            continue;
        }
        Completion done;
        done.id = r.id;
        done.epoch = r.epoch;
        done.requestBytes = r.bytes;
        done.full = r.full;
        done.reconcile = r.reconcile;
        done.verifyBan = r.verifyBan;
        done.resolveBan = r.resolveBan;
        if (r.full) done.prepared = std::move(r.prepared);
        try {
            if (r.verifyBan) {
                done.payload = NativeBehavior::Engine::VerifyBanState();
                if (done.payload.size() > 8*1024*1024)
                    throw std::runtime_error("ban verification exceeds 8 MiB");
            } else if (r.reconcile) {
                Actions::Result players = Actions::Players();
                bool nativeRows = false;
                if (players.status == 200 && players.body.size() <= 8*1024*1024) {
                    try { nativeRows = Json::parse(players.body).is_array(); } catch (...) {}
                }
                if (nativeRows) done.payload = std::move(players.body);
                else {
                    auto raw = NativeTransport::FetchGamePlayers();
                    auto normalized = raw ? NativeBehavior::Engine::NormalizeGamePlayers(*raw) : std::nullopt;
                    if (!normalized) throw std::runtime_error("online reconciliation failed");
                    done.payload = std::move(*normalized);
                }
                if (done.payload.size() > 8*1024*1024) throw std::runtime_error("reconciliation exceeds 8 MiB");
            } else if (r.resolveBan) {
                done.resolvedBanId = NativeBehavior::Engine::ResolveBanTarget(done.prepared);
                if (done.resolvedBanId.empty() || done.resolvedBanId.size() > 1024*1024)
                    throw std::runtime_error("ban target resolution failed");
            } else if (r.full) {
                done.outcome = NativeBehavior::Engine::ExecuteAction(done.prepared);
                if (done.outcome.payloadJson.size() > 8*1024*1024)
                    throw std::runtime_error("response exceeds 8 MiB");
            } else {
                std::string payload = handler ? handler(r.action, r.args) : GateAction(r.action, r.args);
                if (payload.size() > 8*1024*1024) throw std::runtime_error("response exceeds 8 MiB");
                done.payload = Json::parse(payload).dump();
                if (done.payload.size() > 8*1024*1024) throw std::runtime_error("response exceeds 8 MiB");
            }
        } catch (const std::exception& ex) { done.payload.clear(); done.error = ex.what(); }
        catch (...) { done.payload.clear(); done.error = "native action failed"; }
        if (r.full && !done.error.empty()) done.outcome.errorText = done.error;
        if (done.error.size() > 1024) done.error.resize(1024);
        {
            std::unique_lock<std::mutex> g(mu);
            size_t bytes = done.id.size() + done.payload.size() + done.error.size() +
                           done.outcome.payloadJson.size() + done.outcome.errorText.size() +
                           done.resolvedBanId.size();
            for (const auto& effect : done.outcome.effects) bytes += effect.json.size();
            if (bytes > 15*1024*1024 || completionBytes + bytes > 15*1024*1024) {
                done.payload.clear(); done.outcome.payloadJson.clear();
                done.error = "native completion queue overloaded";
                done.outcome.errorText = done.error;
                // Cache effects are optional after an explicit error. BanChanged
                // is required to finish the durable ban intent after mutation.
                done.outcome.effects.erase(std::remove_if(done.outcome.effects.begin(),
                    done.outcome.effects.end(), [](const NativeBehavior::Effect& e) {
                        return e.kind == NativeBehavior::Effect::Kind::RememberPlayer ||
                               e.kind == NativeBehavior::Effect::Kind::RememberLocation;
                    }), done.outcome.effects.end());
                bytes = done.id.size() + done.error.size() + done.outcome.errorText.size() +
                        done.resolvedBanId.size();
                for (const auto& effect : done.outcome.effects) bytes += effect.json.size();
                ++overloads;
            }
            if (bytes > 16*1024*1024) throw std::runtime_error("critical completion exceeds 16 MiB");
            completionCv.wait(g, [bytes] {
                return stopping || (completions.size() < 128 && completionBytes + bytes <= 16*1024*1024);
            });
            if (stopping) break;
            done.bytes = bytes;
            completionBytes += bytes;
            completions.push_back(std::move(done));
        }
        cv.notify_one();
      } catch (...) {
          ++overloads;
          if (started) {
              { std::lock_guard<std::mutex> g(mu);
                actionBytes -= r.bytes;
                if (runningActions) --runningActions;
                pendingRequestIds.erase(r.id);
                actionApplied = true; }
              actionCv.notify_one();
              NativeTransport::RequestClose(r.epoch, 1013, "action worker failed");
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
}
} // namespace

void SetActionHandler(ActionHandler h) { if (!running) handler = std::move(h); }
bool Start() {
    const char* disable = getenv("TAKARO_NATIVE_DISABLE");
    if (disable && strcmp(disable, "1") == 0) return false;
    const char* gate = getenv("TAKARO_NATIVE_GATE");
    const bool experimentalGate = gate && strcmp(gate, "1") == 0;
    if (running.exchange(true)) return false;
    gateMode = experimentalGate;
    try {
    static bool exitStopRegistered = false;
    if (!exitStopRegistered) { std::atexit([] { Stop(); }); exitStopRegistered = true; }
    stopping = false;
    {
        std::lock_guard<std::mutex> g(mu);
        notices.clear(); criticalNotices.clear(); actions.clear(); completions.clear();
        rawLogLines.clear(); rawLogBytes = 0;
        pendingRequestIds.clear(); completedRequestIds.clear(); completedRequestOrder.clear();
        noticeBytes = actionBytes = runningActions = reservedActionBytes = reservedActions = 0;
        completionBytes = 0; actionApplied = true;
    }
    outbox.clear();
    { std::lock_guard<std::mutex> budget(eventBudgetMu);
      rawMappedEvents.clear(); rawMappedBytes = 0;
      durableBudgetBytes = durableBudgetCount = 0; }
    eventBytes = 0;
    sentEpochByOutboxId.clear(); shutdownResponse.reset(); rawLogLosses = 0;
    reconcileInFlight = banVerifyInFlight = banRecoveryPending = false;
    currentEpoch = scanSeq = confirmedSeq = deliveryLosses = requestCount = protocolErrorCount = 0;
    identified = liveEpoch = false; lastError.clear(); banMetadataError.clear(); lastRequestAction.clear();
    if (!gateMode) {
        durable = std::make_unique<NativePersistence::Store>();
        auto loaded = durable->Load();
        if (!loaded) { lastError = loaded.error; Publish(); running = false; return false; }
        behavior = std::make_unique<NativeBehavior::Engine>(*durable);
        auto ready = behavior->Load();
        if (!ready) {
            lastError = ready.error;
            try { banRecoveryPending = behavior->NeedsBanVerification(); } catch (...) {}
            Publish(); behavior.reset(); durable.reset(); running = false; return false;
        }
        banRecoveryPending = behavior->NeedsBanVerification();
        { std::lock_guard<std::mutex> budget(eventBudgetMu); RefreshDurableBudget(); }
        scanSeq = durable->Current().scan.seq;
        Events::SetRawLogSink([](std::string line) {
            if (stopping) return;
            std::lock_guard<std::mutex> budget(eventBudgetMu);
            if (stopping) return;
            std::lock_guard<std::mutex> g(mu);
            if (line.size() > 1024*1024) { ++rawLogLosses; return; }
            if (durableBudgetCount + rawLogLines.size() + rawMappedEvents.size() >= 5000 ||
                durableBudgetBytes + rawLogBytes + rawMappedBytes + line.size() > 32*1024*1024) {
                ++rawLogLosses; return;
            }
            rawLogBytes += line.size(); rawLogLines.push_back(std::move(line)); cv.notify_one();
        }, behavior->CustomLogJoin(), behavior->CustomLogChat());
    }
    const char* id = getenv("TAKARO_IDENTITY_TOKEN"); identity = id && *id ? id : "vein";
    const char* reg = getenv("TAKARO_REGISTRATION_TOKEN"); registration = reg ? reg : "";
    while (!registration.empty() && std::isspace(static_cast<unsigned char>(registration.front())))
        registration.erase(registration.begin());
    while (!registration.empty() && std::isspace(static_cast<unsigned char>(registration.back())))
        registration.pop_back();
    const char* name = getenv("TAKARO_SERVER_NAME"); serverName = name && *name ? name : "Takaro Dev Vein";
    NativeTransport::Config c;
    const char* url = getenv("TAKARO_WS_URL"); c.url = url && *url ? url : "wss://connect.takaro.io/";
    const char* ca = getenv("TAKARO_CA_FILE"); c.caFile = ca ? ca : "";
    // An explicitly empty VEIN_HTTP_API retains the legacy disable switch.
    const char* gameApi = getenv("VEIN_HTTP_API");
    c.gameHttpUrl = gameApi ? gameApi : "http://127.0.0.1:8080";
    while (!c.gameHttpUrl.empty() && std::isspace(static_cast<unsigned char>(c.gameHttpUrl.front())))
        c.gameHttpUrl.erase(c.gameHttpUrl.begin());
    while (!c.gameHttpUrl.empty() && std::isspace(static_cast<unsigned char>(c.gameHttpUrl.back())))
        c.gameHttpUrl.pop_back();
    while (!c.gameHttpUrl.empty() && c.gameHttpUrl.back() == '/') c.gameHttpUrl.pop_back();
    auto backoff = [](const char* key, unsigned fallback) {
        const char* value = getenv(key);
        if (!value || !*value) return fallback;
        char* end = nullptr;
        unsigned long parsed = std::strtoul(value, &end, 10);
        return *end == '\0' && parsed >= 2000 && parsed <= 60000
            ? static_cast<unsigned>(parsed) : fallback;
    };
    c.reconnectBaseMs = backoff("TAKARO_RECONNECT_BASE_MS", 2000);
    c.reconnectMaxMs = backoff("TAKARO_RECONNECT_MAX_MS", 60000);
    if (c.reconnectMaxMs < c.reconnectBaseMs) c.reconnectMaxMs = c.reconnectBaseMs;
    auto sink = [](NativeTransport::Notice n) -> bool {
        std::lock_guard<std::mutex> g(mu);
        if (n.type == NativeTransport::NoticeType::Written && !n.pingAfterWrite) {
            for (auto& queued : criticalNotices) {
                if (queued.type == n.type && queued.epoch == n.epoch && !queued.pingAfterWrite) {
                    queued.eventSeq = std::max(queued.eventSeq, n.eventSeq);
                    queued.outboxId = std::max(queued.outboxId, n.outboxId);
                    return true;
                }
            }
        }
        if (n.type != NativeTransport::NoticeType::Frame) {
            if (criticalNotices.size() >= 128) { ++overloads; return false; }
            criticalNotices.push_back(std::move(n)); cv.notify_one(); return true;
        }
        if (notices.size() >= 512 || noticeBytes + n.text.size() > 8*1024*1024) {
            ++overloads;
            return false;
        }
        noticeBytes += n.text.size(); notices.push_back(std::move(n)); cv.notify_one();
        return true;
    };
    bridgeWorker = std::thread(BridgeLoop);
    actionWorker = std::thread(ActionLoop);
    if (!NativeTransport::Start(std::move(c), sink)) { Stop(); return false; }
    if (gateMode) PluginLog("native: experimental direct Takaro gate enabled; event outbox is not yet durable");
    else PluginLog("native: direct Takaro connector enabled with durable event outbox");
    return true;
    } catch (const std::exception& ex) {
        lastError = ex.what(); Stop(); Publish(); return false;
    } catch (...) {
        lastError = "native startup failed"; Stop(); Publish(); return false;
    }
}
void Stop() {
    if (!running.exchange(false)) return;
    if (!gateMode) Events::SetRawLogSink({}, false, false);
    stopping = true; cv.notify_all(); actionCv.notify_all(); completionCv.notify_all();
    NativeTransport::Stop();
    if (bridgeWorker.joinable()) bridgeWorker.join();
    if (actionWorker.joinable()) actionWorker.join();
    FlushPluginLogs();
    behavior.reset(); durable.reset();
    sentEpochByOutboxId.clear(); shutdownResponse.reset();
    reconcileInFlight = banVerifyInFlight = banRecoveryPending = false;
    banMetadataError.clear();
    { std::lock_guard<std::mutex> budget(eventBudgetMu);
      rawMappedEvents.clear(); rawMappedBytes = 0;
      durableBudgetBytes = durableBudgetCount = 0; }
    { std::lock_guard<std::mutex> g(mu);
      notices.clear(); criticalNotices.clear(); actions.clear(); completions.clear(); rawLogLines.clear();
      pendingRequestIds.clear(); completedRequestIds.clear(); completedRequestOrder.clear();
      noticeBytes = actionBytes = runningActions = reservedActionBytes = reservedActions = 0;
      completionBytes = 0; rawLogBytes = 0; actionApplied = true; }
    { std::lock_guard<std::mutex> g(eventLocationMu); eventLocationWindows.clear(); }
}
std::string HealthJson() {
    auto transport = NativeTransport::Snapshot();
    std::lock_guard<std::mutex> g(mu);
    Json j = { {"connection", {{"state", !running ? "disabled" : transport.connected ? "connected" : "disconnected"},
                               {"identified", published.identified}, {"epoch", published.epoch}}},
               {"queues", {{"inbound", notices.size()}, {"actions", actions.size()},
                            {"runningActions", runningActions},
                            {"outbound", transport.outboundMessages}, {"outbox", published.outbox},
                            {"inboundBytes", noticeBytes}, {"actionBytes", actionBytes},
                            {"outboundBytes", transport.outboundBytes}, {"outboxBytes", published.outboxBytes},
                            {"transientWriteBytes", transport.transientBytes},
                            {"completionBytes", completionBytes}, {"rawLogs", rawLogLines.size()},
                            {"rawLogBytes", rawLogBytes}, {"rawMapped", published.rawMapped},
                            {"rawMappedBytes", published.rawMappedBytes}}},
               {"deliveryLosses", published.losses}, {"persistenceErrors", published.persistenceErrors},
               {"banRecoveryPending", published.recoveryPending},
               {"banMetadataError", published.metadataError},
               {"outboxDurabilityPending", published.outboxDurabilityPending},
               {"rawLogLosses", published.rawLogLosses},
               {"overloads", overloads.load() + transport.rejected},
               {"prunedEvents", transport.prunedEvents},
               {"confirmedSeq", published.confirmed}, {"scanSeq", published.scan},
               {"lastError", published.error},
               {"requestCount", published.requests}, {"lastRequestAction", published.action},
               {"protocolErrorCount", published.protocolErrors},
               {"stateVersion", published.stateVersion},
               {"persistenceLastError", published.persistenceError},
               {"behavior", Json::parse(published.behaviorHealth, nullptr, false)},
               {"gate", {{"experimental", gateMode.load()}, {"durableOutbox", !gateMode.load()}}} };
    return j.dump();
}
#ifdef TAKARO_BRIDGE_TEST
void TestPauseCompletions(bool pause) {
    pauseCompletions = pause;
    cv.notify_one();
}
void TestNoteLocationWindow(const std::string& id) { NoteLocationWindow(id); }
std::string TestGateAction(const std::string& action, const std::string& args) {
    return GateAction(action, args);
}
#endif
} // namespace NativeBridge
