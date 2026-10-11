#include "takaro/bridge.h"

#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"

#include <algorithm>
#include <chrono>

namespace takaro {

namespace {
std::shared_ptr<const std::string> Text(std::string s) { return std::make_shared<const std::string>(std::move(s)); }
}  // namespace

Bridge::Bridge(BridgeOptions o) : o_(std::move(o)) {
    if (!o_.steadyMs)
        o_.steadyMs = [] {
            return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
}

Bridge::~Bridge() { Stop(); }

bool Bridge::Start() {
    if (started_.exchange(true)) return false;
    stopping_ = false;
    storeNotes_ = o_.store->Load();
    NativeLog("takaro: state dir %s: %s", o_.store->Dir().c_str(), storeNotes_.c_str());
    int64_t now = o_.steadyMs();
    if (o_.watcher) o_.watcher->Announce(o_.config, now);
    Publish(now);
    thread_ = std::thread(&Bridge::Loop, this);
    for (unsigned i = 0; i < std::max(1u, o_.config.actionWorkers); i++) workers_.emplace_back(&Bridge::WorkerLoop, this);
    if (o_.transport) o_.transport->Start([this](Notice&& n) { return OnNotice(std::move(n)); });
    return true;
}

void Bridge::Stop() {
    if (!started_) return;
    if (o_.transport) o_.transport->Stop();
    {
        std::lock_guard<std::mutex> g(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    jobCv_.notify_all();
    if (thread_.joinable()) thread_.join();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    workers_.clear();
    started_ = false;
}

bool Bridge::OnNotice(Notice&& n) {
    {
        std::lock_guard<std::mutex> g(mu_);
        // Only inbound messages count against the bound; lifecycle and confirmation notices are tiny.
        if (n.type == NoticeType::Frame &&
            (notices_.size() >= kMaxNotices || noticeBytes_ + n.text.size() > kMaxNoticeBytes)) {
            noticeOverflows_++;
            return false;
        }
        noticeBytes_ += n.text.size();
        notices_.push_back(std::move(n));
    }
    cv_.notify_one();
    return true;
}

std::string Bridge::HealthJson() const {
    std::lock_guard<std::mutex> g(healthMu_);
    return health_;
}

// ------------------------------------------------------------------------------------------------ loop

void Bridge::Loop() {
    while (!stopping_) {
        std::vector<Notice> batch;
        std::vector<std::shared_ptr<Job>> done;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait_for(l, std::chrono::milliseconds(50),
                         [&] { return stopping_ || !notices_.empty() || !completions_.empty(); });
            if (stopping_) break;
            while (!notices_.empty() && batch.size() < kNoticeBatch) {
                noticeBytes_ -= notices_.front().text.size();
                batch.push_back(std::move(notices_.front()));
                notices_.pop_front();
            }
            done.assign(completions_.begin(), completions_.end());
            completions_.clear();
        }
        for (auto& n : batch) HandleNotice(n);
        for (auto& j : done) Complete(j);
        int64_t now = o_.steadyMs();
        CheckDeadlines(now);
        if (now >= nextPoll_) {
            PollEvents();
            nextPoll_ = now + o_.eventPollMs;
        }
        DeliverEvents();
        FlushState(now, false);
        if (o_.watcher) {
            Config next;
            if (o_.watcher->Poll(now, next)) ApplyConfig(std::move(next), now);
            o_.watcher->Remind(o_.config, now);
        }
        if (now >= nextPublish_) {
            Publish(now);
            nextPublish_ = now + 500;
        }
    }
    int64_t now = o_.steadyMs();
    PollEvents();
    FlushState(now, true);
    Publish(now);
}

void Bridge::WorkerLoop() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> l(mu_);
            jobCv_.wait(l, [&] { return stopping_ || !jobQueue_.empty(); });
            if (stopping_) return;
            job = jobQueue_.front();
            jobQueue_.pop_front();
        }
        int expected = kQueued;
        if (!job->state.compare_exchange_strong(expected, kRunning)) continue;  // cancelled before it started
        try {
            job->outcome = o_.game->Execute(job->action, job->args);
        } catch (const std::exception& e) {
            job->outcome = ActionResult();
            job->outcome.error = std::string("internal error: ") + e.what();
        } catch (...) {
            job->outcome = ActionResult();
            job->outcome.error = "internal error";
        }
        job->state = kDone;
        {
            std::lock_guard<std::mutex> g(mu_);
            completions_.push_back(job);
        }
        cv_.notify_one();
    }
}

// ------------------------------------------------------------------------------------------------ config

void Bridge::ApplyConfig(Config next, int64_t now) {
    NativeLog("config: %s changed; %s (identity from %s, registration token from %s)",
              o_.watcher->FilePath().c_str(),
              next.enabled ? "reconnecting with the new settings" : next.disabledReason.c_str(),
              SourceName(next.identitySource), SourceName(next.registrationSource));
    o_.config = std::move(next);
    lastIdentifyError_.clear();
    o_.watcher->Announce(o_.config, now);
    // The old connection's Closed notice and any late frame of it carry its epoch; the identify of
    // the next connection uses the settings set here.
    if (o_.transport) o_.transport->Retarget(o_.config.url, o_.config.enabled);
}

void Bridge::OnRefused(const std::string& why, bool nameTaken) {
    identifyErrors_++;
    lastIdentifyError_ = why;
    NativeLog("takaro: identify failed: %s", why.c_str());
    if (o_.watcher) o_.watcher->Refused(o_.config, why, nameTaken);
}

// ------------------------------------------------------------------------------------------------ notices

void Bridge::HandleNotice(Notice& n) {
    switch (n.type) {
        case NoticeType::Open: {
            epoch_ = n.epoch;
            open_ = true;
            identified_ = false;
            opens_++;
            lastQueuedId_ = 0;  // everything unconfirmed goes out again on this connection
            pendingIds_.clear();
            recentIds_.clear();
            recentIdSet_.clear();
            if (!o_.config.enabled) {  // a dial that raced a hold: never identify without a token
                o_.transport->RequestClose(n.epoch, "not configured");
                break;
            }
            identifyConfig_ = o_.config;
            NativeLog("takaro: WebSocket open (epoch %llu), sending identify", (unsigned long long)n.epoch);
            auto st = o_.transport->Queue({FrameKind::Control,
                                           Text(CreateIdentify(o_.config.identityToken, o_.config.registrationToken,
                                                               o_.config.serverName)),
                                           n.epoch, 0});
            if (st != QueueStatus::Accepted) o_.transport->RequestClose(n.epoch, "identify not queued");
            break;
        }
        case NoticeType::Frame:
            if (n.epoch == epoch_ && open_) HandleFrame(n.epoch, n.text);
            break;
        case NoticeType::Confirmed:
            if (n.epoch == epoch_ && open_ && o_.store->ConfirmThrough(n.outboxId)) outboxDirty_ = true;
            break;
        case NoticeType::Closed: {
            if (n.epoch != epoch_) break;
            open_ = false;
            identified_ = false;
            closes_++;
            NativeLog("takaro: WebSocket closed (epoch %llu): %s; %zu event(s) unconfirmed, kept for replay",
                      (unsigned long long)n.epoch, o_.config.Redact(n.text).c_str(),
                      o_.store->Outbox().pending.size());
            // Unstarted requests of the dead connection can never be answered: cancel them.
            for (auto it = inflight_.begin(); it != inflight_.end();) {
                int expected = kQueued;
                if (it->second->epoch == n.epoch && it->second->state.compare_exchange_strong(expected, kCancelled))
                    it = inflight_.erase(it);
                else
                    ++it;
            }
            break;
        }
        case NoticeType::Error:
            lastError_ = o_.config.Redact(n.text);
            NativeLog("takaro: transport: %s", lastError_.c_str());
            break;
    }
}

void Bridge::HandleFrame(uint64_t epoch, const std::string& text) {
    JsonValue msg;
    if (text.size() > kMaxInboundBytes || !ParseJson(text, msg) || msg.type != JsonValue::Object) {
        if (malformed_++ < 5 || malformed_ % 100 == 0)
            NativeLog("takaro: ignoring an invalid Takaro message (%zu bytes)", text.size());
        return;
    }
    const JsonValue* t = msg.get("type");
    std::string type = t && t->type == JsonValue::String ? t->str : "";
    if (type == "request") {
        HandleRequest(epoch, msg);
    } else if (type == "ping") {
        auto st = o_.transport->Queue({FrameKind::Control, Text("{\"type\":\"pong\"}"), epoch, 0});
        if (st != QueueStatus::Accepted) o_.transport->RequestClose(epoch, "control queue full");
    } else if (type == "pong") {
        // heartbeat bookkeeping happens in the transport
    } else if (type == "identifyResponse") {
        const JsonValue& payload = AsRecord(msg.get("payload"));
        if (Truthy(payload.get("error"))) {
            bool nameTaken = false;
            OnRefused(DescribeTakaroError(payload.get("error"), o_.config, &nameTaken), nameTaken);
            o_.transport->RequestClose(epoch, "identify rejected");
            return;
        }
        identified_ = true;
        identifies_++;
        lastIdentifyError_.clear();
        auto gs = Str(payload.get("gameServerId"));
        if (!gs) gs = Str(AsRecord(payload.get("server")).get("id"));
        gameServerId_ = gs.value_or("");
        o_.transport->MarkIdentified(epoch);
        NativeLog("takaro: identified%s%s; %zu unconfirmed event(s) to deliver",
                  gameServerId_.empty() ? "" : " gameServerId=", gameServerId_.c_str(),
                  o_.store->Outbox().pending.size());
        if (o_.watcher) {
            o_.watcher->Identified(identifyConfig_);
            o_.watcher->Connected(o_.config);
        }
    } else if (type == "connected") {
        NativeLog("takaro: Takaro confirmed the WebSocket connection");
    } else if (type == "error") {
        const JsonValue* p = msg.get("payload");
        if (!p || p->type == JsonValue::Null) p = msg.get("error");
        if (p && p->type == JsonValue::Object && p->get("error")) p = p->get("error");
        bool nameTaken = false;
        std::string why = DescribeTakaroError(p, o_.config, &nameTaken);
        lastError_ = "Takaro error: " + why;
        NativeLog("takaro: %s", lastError_.c_str());
        if (!identified_) OnRefused(why, nameTaken);
    }
}

void Bridge::RememberId(const std::string& id) {
    recentIds_.push_back(id);
    recentIdSet_.insert(id);
    if (recentIds_.size() > 1024) {
        recentIdSet_.erase(recentIds_.front());
        recentIds_.pop_front();
    }
}

void Bridge::HandleRequest(uint64_t epoch, const JsonValue& msg) {
    requests_++;
    const JsonValue* rid = msg.get("requestId");
    std::string id = rid && rid->type == JsonValue::String   ? rid->str
                     : rid && rid->type == JsonValue::Number ? JsString(rid)
                                                             : "";
    if (id.empty()) {
        NativeLog("takaro: ignoring a request without requestId");
        return;
    }
    if (id.size() > kMaxRequestIdBytes) {
        NativeLog("takaro: ignoring a request with a %zu-byte requestId", id.size());
        return;
    }
    const JsonValue& payload = AsRecord(msg.get("payload"));
    const JsonValue* a = payload.get("action");
    std::string action = a && a->type == JsonValue::String ? a->str : "";
    lastAction_ = action.substr(0, 64);
    auto fail = [&](const std::string& text) {
        errorResponses_++;
        NativeLog("takaro: request %s (%s) failed: %s", id.c_str(), lastAction_.c_str(), text.c_str());
        SendResponse(epoch, FrameKind::CriticalResponse, CreateErrorResponse(id, text));
    };
    if (pendingIds_.count(id) || recentIdSet_.count(id)) {
        duplicates_++;
        fail("duplicate requestId");
        return;
    }
    if (action.empty()) {
        RememberId(id);
        fail("Takaro request missing action");
        return;
    }
    if (inflight_.size() >= kMaxPendingActions) {
        overloads_++;
        RememberId(id);
        fail("native action queue overloaded");
        return;
    }
    const JsonValue* rawArgs = payload.get("args");
    if (rawArgs && rawArgs->type == JsonValue::String && JsonDepthExceeds(rawArgs->str, 64)) {
        RememberId(id);
        fail("request args exceed JSON nesting depth 64");
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = nextJobId_++;
    job->requestId = id;
    job->action = action;
    job->args = NormalizeArgs(rawArgs);
    job->epoch = epoch;
    job->enqueuedMs = o_.steadyMs();
    job->deadlineMs = job->enqueuedMs + o_.config.actionTimeoutMs;
    pendingIds_.insert(id);
    inflight_[job->id] = job;
    {
        std::lock_guard<std::mutex> g(mu_);
        jobQueue_.push_back(job);
    }
    jobCv_.notify_one();
}

void Bridge::SendResponse(uint64_t epoch, FrameKind kind, const std::string& frame) {
    if (!open_ || epoch != epoch_) {
        droppedResponses_++;
        return;
    }
    QueueStatus st = o_.transport->Queue({kind, Text(frame), epoch, 0});
    if (st == QueueStatus::Accepted) {
        responses_++;
        return;
    }
    droppedResponses_++;
    NativeLog("takaro: response not queued (%s)", QueueStatusName(st));
    if (st == QueueStatus::Full) {
        overloads_++;
        o_.transport->RequestClose(epoch, "response queue full");
    }
}

void Bridge::Complete(const std::shared_ptr<Job>& job) {
    inflight_.erase(job->id);
    int64_t now = o_.steadyMs();
    pendingIds_.erase(job->requestId);
    RememberId(job->requestId);
    const ActionResult& out = job->outcome;
    if (job->responded) {
        lateResults_++;
        NativeLog("takaro: late result for %s '%s' discarded (%s)", job->requestId.c_str(), job->action.c_str(),
                  out.ok ? "ok" : out.error.c_str());
        return;
    }
    if (!out.ok) {
        errorResponses_++;
        NativeLog("takaro: request %s '%s' failed: %s", job->requestId.c_str(), job->action.c_str(),
                  out.error.c_str());
    }
    maxRequestLatencyMs_ = std::max<int64_t>(maxRequestLatencyMs_, now - job->enqueuedMs);
    SendResponse(job->epoch, FrameKind::Response,
                 out.ok ? CreateResponse(job->requestId, out.payload) : CreateErrorResponse(job->requestId, out.error));
}

void Bridge::CheckDeadlines(int64_t now) {
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        auto job = it->second;
        if (job->responded || now <= job->deadlineMs) {
            ++it;
            continue;
        }
        timeouts_++;
        int expected = kQueued;
        bool neverStarted = job->state.compare_exchange_strong(expected, kCancelled);
        std::string error = neverStarted ? "Conan connector did not start '" + job->action + "' within " +
                                               std::to_string(o_.config.actionTimeoutMs) + "ms (action workers busy)"
                                         : "Conan action '" + job->action + "' timed out after " +
                                               std::to_string(o_.config.actionTimeoutMs) + "ms";
        errorResponses_++;
        NativeLog("takaro: request %s: %s", job->requestId.c_str(), error.c_str());
        SendResponse(job->epoch, FrameKind::CriticalResponse, CreateErrorResponse(job->requestId, error));
        job->responded = true;
        pendingIds_.erase(job->requestId);
        if (neverStarted) {
            RememberId(job->requestId);
            it = inflight_.erase(it);
        } else {
            ++it;  // keep it until the worker finishes, then drop the late result
        }
    }
}

// ------------------------------------------------------------------------------------------------ events

void Bridge::PollEvents() {
    std::vector<GameEvent> events;
    o_.game->DrainEvents(events);
    if (events.empty()) return;
    if (!o_.store->OutboxUsable()) {
        eventsRejected_ += events.size();
        return;
    }
    for (auto& e : events) {
        if (!IsEventType(e.type) || e.data.type != JsonValue::Object) {
            eventsRejected_++;
            NativeLog("takaro: dropping an event with an unknown type '%s'", e.type.c_str());
            continue;
        }
        uint64_t lossesBefore = o_.store->Outbox().losses;
        o_.store->Admit(e.type, CreateGameEvent(e.type, e.data));
        uint64_t losses = o_.store->Outbox().losses;
        if (losses != lossesBefore && (losses < 10 || losses % 500 == 0))
            NativeLog("takaro: event outbox full; %llu events dropped so far", (unsigned long long)losses);
        eventsAdmitted_++;
        outboxDirty_ = true;
    }
}

void Bridge::DeliverEvents() {
    if (!open_ || !identified_ || !o_.store->OutboxUsable()) return;
    auto& pending = o_.store->Outbox().pending;
    auto it = std::upper_bound(pending.begin(), pending.end(), lastQueuedId_,
                               [](uint64_t id, const PendingEvent& e) { return id < e.id; });
    size_t queued = 0;
    for (; it != pending.end() && queued < kNoticeBatch; ++it) {
        QueueStatus st = o_.transport->Queue({FrameKind::Event, Text(it->frame), epoch_, it->id});
        if (st != QueueStatus::Accepted) {
            if (st == QueueStatus::TooLarge) {
                NativeLog("takaro: dropping an event frame too large to send (%zu bytes)", it->frame.size());
                lastQueuedId_ = it->id;
                continue;
            }
            break;
        }
        lastQueuedId_ = it->id;
        queued++;
        eventsQueued_++;
    }
}

// ------------------------------------------------------------------------------------------------ state

void Bridge::FlushState(int64_t now, bool force) {
    // The outbox file is rewritten whole: while Takaro is away and it grows, write it less often
    // (100 ms when small, up to 5 s near the 32 MiB bound).
    int64_t gap = std::min<int64_t>(5000, 100 * (1 + (int64_t)(o_.store->Outbox().pendingBytes >> 18)));
    if (outboxDirty_ && (force || now - lastOutboxSave_ >= gap)) {
        lastOutboxSave_ = now;
        outboxDirty_ = !o_.store->SaveOutbox() && o_.store->OutboxUsable();
    }
}

void Bridge::Publish(int64_t now) {
    OutboxState& ob = o_.store->Outbox();
    std::string errors = "{";
    bool first = true;
    for (auto& kv : o_.store->Errors()) {
        errors += (first ? "" : ",") + JsonStr(kv.first) + ":" + JsonStr(kv.second);
        first = false;
    }
    errors += "}";
    size_t running = 0;
    for (auto& kv : inflight_) running += kv.second->state == kRunning;
    uint64_t overflows;
    {
        std::lock_guard<std::mutex> g(mu_);
        overflows = noticeOverflows_;
    }
    std::string state = !open_ ? "connecting" : identified_ ? "identified" : "open";
    std::string h = ObjBuilder()
                        .S("state", state)
                        .Raw("epoch", std::to_string(epoch_))
                        .B("identified", identified_)
                        .S("gameServerId", gameServerId_)
                        .S("lastError", lastError_)
                        .S("lastIdentifyError", lastIdentifyError_)
                        .Raw("config", ConfigSummaryJson(o_.config))
                        .Raw("configFile", o_.watcher ? o_.watcher->HealthJson() : "null")
                        .Raw("transport", o_.transport ? o_.transport->StatsJson() : "null")
                        .Raw("game", o_.game->HealthJson())
                        .Raw("connection", ObjBuilder()
                                               .N("opens", (double)opens_)
                                               .N("closes", (double)closes_)
                                               .N("identifies", (double)identifies_)
                                               .N("identifyErrors", (double)identifyErrors_)
                                               .Done())
                        .Raw("requests", ObjBuilder()
                                             .N("received", (double)requests_)
                                             .N("responses", (double)responses_)
                                             .N("errors", (double)errorResponses_)
                                             .N("duplicates", (double)duplicates_)
                                             .N("malformedFrames", (double)malformed_)
                                             .N("overloads", (double)overloads_)
                                             .N("timeouts", (double)timeouts_)
                                             .N("lateResults", (double)lateResults_)
                                             .N("droppedResponses", (double)droppedResponses_)
                                             .N("noticeOverflows", (double)overflows)
                                             .N("inflight", (double)inflight_.size())
                                             .N("running", (double)running)
                                             .N("maxLatencyMs", (double)maxRequestLatencyMs_)
                                             .S("lastAction", lastAction_)
                                             .Done())
                        .Raw("outbox", ObjBuilder()
                                           .B("usable", o_.store->OutboxUsable())
                                           .N("pending", (double)ob.pending.size())
                                           .N("pendingBytes", (double)ob.pendingBytes)
                                           .Raw("nextId", std::to_string(ob.nextId))
                                           .Raw("queuedThrough", std::to_string(lastQueuedId_))
                                           .N("confirmedTotal", (double)ob.confirmedTotal)
                                           .N("losses", (double)ob.losses)
                                           .N("admitted", (double)eventsAdmitted_)
                                           .N("queued", (double)eventsQueued_)
                                           .N("rejected", (double)eventsRejected_)
                                           .S("dir", o_.store->Dir())
                                           .N("writes", (double)o_.store->WriteCount())
                                           .Raw("errors", errors)
                                           .Done())
                        .Done();
    {
        std::lock_guard<std::mutex> g(healthMu_);
        health_ = h;
    }
    if (!o_.healthFile.empty() && now >= nextHealthFile_) {
        nextHealthFile_ = now + 5000;
        std::string err;
        AtomicWriteFile(o_.healthFile, h + "\n", err);
    }
}

}  // namespace takaro
