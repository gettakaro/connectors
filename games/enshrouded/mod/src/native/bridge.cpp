#include "native/bridge.h"

#include "native/json_util.h"
#include "native/mapping.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace native {

namespace {

std::shared_ptr<const std::string> Text(std::string s) { return std::make_shared<const std::string>(std::move(s)); }

bool IsConnectionType(const std::string& t) { return t == "player-connected" || t == "player-disconnected"; }

std::string CursorJson(const SourceCursor& c) {
    return ObjBuilder().S("bootId", c.bootId).Raw("seq", std::to_string(c.seq)).Done();
}

const std::string* GameIdOf(const JsonValue& player) {
    const JsonValue* g = player.get("gameId");
    return g && g->type == JsonValue::String && !g->str.empty() ? &g->str : nullptr;
}

}  // namespace

Bridge::Bridge(BridgeOptions o) : o_(std::move(o)), tailer_(o_.config.logFile) {
    if (!o_.steadyMs)
        o_.steadyMs = [] {
            return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
    if (!o_.wallMs)
        o_.wallMs = [] {
            return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        };
}

Bridge::~Bridge() { Stop(); }

bool Bridge::Start() {
    if (started_.exchange(true)) return false;
    stopping_ = false;
    LoadReport rep = o_.store->Load();
    for (auto& n : rep.notes) PluginLog("native: state: %s", n.c_str());
    for (auto& kv : o_.store->Errors()) PluginLog("native: state error [%s]: %s", kv.first.c_str(), kv.second.c_str());
    RecoverBanIntents();
    reconcilePending_ = true;
    int64_t now = o_.steadyMs();
    nextPoll_ = nextHealth_ = nextBanCheck_ = now;
    nextReconcile_ = now + o_.reconcileIntervalMs;
    FlushState(now, true);
    Publish();
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
        // Only inbound messages count against the bound; lifecycle and confirmation notices are tiny and must not be lost.
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
        if (now >= nextHealth_) {
            RefreshHealth(now);
            nextHealth_ = now + o_.healthIntervalMs;
        }
        if (now >= nextPoll_) {
            PollEvents(now);
            PollTail(now);
            nextPoll_ = now + o_.config.pollIntervalMs;
        }
        if (reconcilePending_ || now >= nextReconcile_) Reconcile(reconcilePending_, now);
        if (now >= nextBanCheck_) CheckTimedBans(now);
        DeliverEvents(now);
        FlushState(now, false);
        if (now >= nextPublish_) {
            Publish();
            nextPublish_ = now + 500;
        }
    }
    FlushState(o_.steadyMs(), true);
    Publish();
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
        job->startedMs = o_.steadyMs();
        if (job->expiry)
            job->outcome = ExpireTimedBan(*o_.game, job->expiryGameId);
        else
            job->outcome = ExecuteAction(*o_.game, job->action, job->args, *job->view, o_.wallMs());
        job->finishedMs = o_.steadyMs();
        job->state = kDone;
        {
            std::lock_guard<std::mutex> g(mu_);
            completions_.push_back(job);
        }
        cv_.notify_one();
    }
}

// ------------------------------------------------------------------------------------------------ notices

void Bridge::HandleNotice(Notice& n) {
    switch (n.type) {
        case NoticeType::Open: {
            epoch_ = n.epoch;
            open_ = true;
            identified_ = false;
            lastQueuedId_ = 0;  // everything unconfirmed goes out again on this connection
            pendingIds_.clear();
            recentIds_.clear();
            recentIdSet_.clear();
            PluginLog("native: Takaro WebSocket open (epoch %llu), sending identify", (unsigned long long)n.epoch);
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
            if (n.epoch == epoch_ && open_) {
                size_t k = o_.store->ConfirmThrough(n.outboxId);
                if (k) outboxDirty_ = true;
            }
            break;
        case NoticeType::Closed: {
            if (n.epoch != epoch_) break;
            open_ = false;
            identified_ = false;
            gameServerId_.clear();
            PluginLog("native: Takaro WebSocket closed (epoch %llu): %s", (unsigned long long)n.epoch,
                      Redact(n.text).c_str());
            // Unstarted requests of the dead connection can never be answered: cancel them.
            for (auto it = inflight_.begin(); it != inflight_.end();) {
                auto& job = it->second;
                int expected = kQueued;
                if (!job->expiry && job->epoch == n.epoch && job->state.compare_exchange_strong(expected, kCancelled)) {
                    if (!job->intentId.empty()) {
                        auto& intents = o_.store->BanIntents();
                        intents.erase(std::remove_if(intents.begin(), intents.end(),
                                                     [&](const JsonValue& v) {
                                                         const JsonValue* id = v.get("id");
                                                         return id && id->str == job->intentId;
                                                     }),
                                      intents.end());
                        o_.store->SaveBanIntents();
                    }
                    it = inflight_.erase(it);
                } else {
                    ++it;
                }
            }
            break;
        }
        case NoticeType::Error:
            lastError_ = Redact(n.text);
            break;
    }
}

void Bridge::HandleFrame(uint64_t epoch, const std::string& text) {
    JsonValue msg;
    if (text.size() > kMaxInboundBytes || !ParseJson(text, msg) || msg.type != JsonValue::Object) {
        if (malformed_++ < 5 || malformed_ % 100 == 0)
            PluginLog("native: ignoring invalid Takaro message (%zu bytes, malformed/too deep/too large)", text.size());
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
            identifyErrors_++;
            lastIdentifyError_ = Redact(JsonDump(*payload.get("error")));
            if (lastIdentifyError_.size() > 512) lastIdentifyError_.resize(512);
            PluginLog("native: Takaro identify failed: %s", lastIdentifyError_.c_str());
            o_.transport->RequestClose(epoch, "identify rejected");
            return;
        }
        identified_ = true;
        lastIdentifyError_.clear();
        auto gs = Str(payload.get("gameServerId"));
        if (!gs) gs = Str(AsRecord(payload.get("server")).get("id"));
        gameServerId_ = gs.value_or("");
        o_.transport->MarkIdentified(epoch);
        PluginLog("native: identified with Takaro%s%s", gameServerId_.empty() ? "" : " gameServerId=",
                  gameServerId_.c_str());
    } else if (type == "connected") {
        clientId_ = Str(AsRecord(msg.get("payload")).get("clientId")).value_or("");
        PluginLog("native: Takaro confirmed WebSocket connection");
    } else if (type == "error") {
        const JsonValue* p = msg.get("payload");
        if (!p || p->type == JsonValue::Null) p = msg.get("error");
        lastError_ = "Takaro error: " + Redact(p ? JsonDump(*p) : "(none)");
        if (lastError_.size() > 600) lastError_.resize(600);
        PluginLog("native: %s", lastError_.c_str());
    }
}

void Bridge::HandleRequest(uint64_t epoch, const JsonValue& msg) {
    requests_++;
    const JsonValue* rid = msg.get("requestId");
    std::string id = rid && rid->type == JsonValue::String ? rid->str : rid && rid->type == JsonValue::Number ? JsString(rid) : "";
    if (id.empty()) {
        PluginLog("native: ignoring Takaro request without requestId");
        return;
    }
    if (id.size() > kMaxRequestIdBytes) {
        oversizeIds_++;
        PluginLog("native: ignoring Takaro request with a %zu-byte requestId (max %zu)", id.size(), kMaxRequestIdBytes);
        return;
    }
    const JsonValue& payload = AsRecord(msg.get("payload"));
    const JsonValue* a = payload.get("action");
    std::string action = a && a->type == JsonValue::String ? a->str : "";
    lastRequestAction_ = action.substr(0, 64);
    auto fail = [&](const std::string& text) {
        errorResponses_++;
        PluginLog("native: Takaro request %s failed: %s", id.c_str(), text.c_str());
        SendResponse(epoch, FrameKind::CriticalResponse, CreateErrorResponse(id, text));
    };
    if (pendingIds_.count(id) || recentIdSet_.count(id)) {
        duplicates_++;
        fail("duplicate requestId");
        return;
    }
    auto remember = [&] {
        recentIds_.push_back(id);
        recentIdSet_.insert(id);
        if (recentIds_.size() > 1024) {
            recentIdSet_.erase(recentIds_.front());
            recentIds_.pop_front();
        }
    };
    if (action.empty()) {
        remember();
        fail("Takaro request missing action");
        return;
    }
    size_t userJobs = 0;
    for (auto& kv : inflight_) userJobs += !kv.second->expiry;
    if (userJobs >= kMaxPendingActions) {
        overloads_++;
        remember();
        fail("native action queue overloaded");
        return;
    }
    const JsonValue* rawArgs = payload.get("args");
    if (rawArgs && rawArgs->type == JsonValue::String && JsonDepthExceeds(rawArgs->str, 64)) {
        remember();
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

    if (action == "banPlayer" || action == "unbanPlayer") {
        // Journal the intent durably before the game acts, so a crash between the ban and the timed-ban
        // record is recovered at the next start (RecoverBanIntents).
        std::string gameId;
        try {
            gameId = StripSteamPrefix(PlayerId(job->args));
        } catch (const std::exception&) {
        }
        auto expires = Str(job->args.get("expiresAt"));
        job->intentId = "r" + std::to_string(job->id) + ":" + id;
        JsonValue intent = JObj();
        Put(intent, "id", JStr(job->intentId));
        Put(intent, "op", JStr(action == "banPlayer" ? "ban" : "unban"));
        Put(intent, "gameId", JStr(gameId));
        if (expires) Put(intent, "expiresAt", JStr(*expires));
        if (auto r = Str(job->args.get("reason"))) Put(intent, "reason", JStr(*r));
        Put(intent, "at", JNum((double)o_.wallMs()));
        o_.store->BanIntents().push_back(intent);
        if (!o_.store->SaveBanIntents() && action == "banPlayer" && expires) {
            o_.store->BanIntents().pop_back();
            remember();
            fail("banPlayer refused: the ban-intent journal cannot be written (" +
                 (o_.store->Errors().count("banIntent") ? o_.store->Errors().at("banIntent") : std::string("unknown")) +
                 "), so the expiry could be lost");
            return;
        }
    }
    job->view = View();
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
    PluginLog("native: response not queued (%s)", QueueStatusName(st));
    if (st == QueueStatus::Full) {
        overloads_++;
        o_.transport->RequestClose(epoch, "response queue full");
    }
}

// ------------------------------------------------------------------------------------------------ actions

std::shared_ptr<const ActionView> Bridge::View() {
    if (viewDirty_ || !view_) {
        auto v = std::make_shared<ActionView>();
        for (auto& k : o_.store->Known()) {
            JsonValue p = k;
            p.obj.erase(std::remove_if(p.obj.begin(), p.obj.end(), [](const std::pair<std::string, JsonValue>& kv) {
                            return kv.first == "lastSeen";
                        }),
                        p.obj.end());
            v->knownPlayers.push_back(std::move(p));
        }
        v->timedBans = o_.store->TimedBans();
        v->banStoreError = o_.store->BanStoreError();
        view_ = v;
        viewDirty_ = false;
    }
    return view_;
}

void Bridge::Complete(const std::shared_ptr<Job>& job) {
    inflight_.erase(job->id);
    int64_t now = o_.steadyMs();
    ActionOutcome& out = job->outcome;
    int64_t wall = o_.wallMs();

    // State changes first: they happened in the game whether or not anyone still waits for the answer.
    for (auto& p : out.seenPlayers)
        if (o_.store->Remember(p, wall)) knownDirty_ = viewDirty_ = true;
    auto& bans = o_.store->TimedBans();
    if (out.ok && out.ban.op != BanChange::None && !o_.store->Fenced("timedBans")) {
        const std::string& gid = out.ban.ban.gameId;
        if (out.ban.op == BanChange::Upsert) {
            bans[gid] = out.ban.ban;
            PluginLog("native: timed ban for %s until %s", gid.c_str(), out.ban.ban.expiresAt.c_str());
        } else if (!job->expiry) {
            bans.erase(gid);
        } else {
            auto it = bans.find(gid);
            // a newer timed ban set while this unban ran keeps its own record
            if (it != bans.end() && it->second.expiresAtMs <= wall) bans.erase(it);
            expiredBans_++;
            PluginLog("native: timed ban for %s expired; unbanned", gid.c_str());
        }
        o_.store->SaveTimedBans();
        viewDirty_ = true;
    }
    if (job->expiry) {
        expiryInFlight_.erase(job->expiryGameId);
        if (!out.ok) {
            auto it = bans.find(job->expiryGameId);
            if (it != bans.end()) {
                it->second.attempts++;
                if (it->second.attempts >= 20) {
                    PluginLog("native: giving up lifting the timed ban for %s after %d attempts: %s",
                              job->expiryGameId.c_str(), it->second.attempts, out.error.c_str());
                    bans.erase(it);
                    o_.store->SaveTimedBans();
                    viewDirty_ = true;
                } else {
                    int64_t backoff = std::min<int64_t>(60000LL << std::min(it->second.attempts, 6), 3600000);
                    it->second.nextAttemptMs = now + backoff;
                    PluginLog("native: lifting the expired timed ban for %s failed (attempt %d, retry in %llds): %s",
                              job->expiryGameId.c_str(), it->second.attempts, (long long)(backoff / 1000),
                              out.error.c_str());
                }
            }
        }
        return;
    }
    if (!job->intentId.empty()) {
        auto& intents = o_.store->BanIntents();
        intents.erase(std::remove_if(intents.begin(), intents.end(),
                                     [&](const JsonValue& v) {
                                         const JsonValue* id = v.get("id");
                                         return id && id->str == job->intentId;
                                     }),
                      intents.end());
        o_.store->SaveBanIntents();
    }
    pendingIds_.erase(job->requestId);
    recentIds_.push_back(job->requestId);
    recentIdSet_.insert(job->requestId);
    if (recentIds_.size() > 1024) {
        recentIdSet_.erase(recentIds_.front());
        recentIds_.pop_front();
    }
    if (job->responded) {
        lateResults_++;
        PluginLog("native: late result for %s '%s' discarded (%s)", job->requestId.c_str(), job->action.c_str(),
                  out.ok ? "ok" : out.error.c_str());
        return;
    }
    JsonValue payload = out.payload;
    bool ok = out.ok;
    std::string error = out.error;
    if (job->action == "getPlayerLocation") {
        if (ok) {
            RememberPosition(job->args, payload);
        } else {
            JsonValue fb;
            if (EventLocationFallback(job->args, now, fb)) {
                PluginLog("native: getPlayerLocation failed during a connect/disconnect event window (%s); answering "
                          "%s so Takaro stores the event",
                          error.c_str(), JsonDump(fb).c_str());
                payload = fb;
                ok = true;
            }
        }
    }
    if (!ok) {
        errorResponses_++;
        PluginLog("native: Takaro request %s failed: %s", job->requestId.c_str(), error.c_str());
    }
    if (ok && job->action == "shutdown") FlushState(now, true);  // the game quits about 500 ms from now
    maxRequestLatencyMs_ = std::max<int64_t>(maxRequestLatencyMs_, now - job->enqueuedMs);
    SendResponse(job->epoch, FrameKind::Response,
                 ok ? CreateResponse(job->requestId, payload) : CreateErrorResponse(job->requestId, error));
}

void Bridge::CheckDeadlines(int64_t now) {
    for (auto it = inflight_.begin(); it != inflight_.end();) {
        auto job = it->second;
        if (job->expiry || job->responded || now <= job->deadlineMs) {
            ++it;
            continue;
        }
        timeouts_++;
        int expected = kQueued;
        bool neverStarted = job->state.compare_exchange_strong(expected, kCancelled);
        std::string error = neverStarted ? "Enshrouded connector did not start '" + job->action + "' within " +
                                               std::to_string(o_.config.actionTimeoutMs) + "ms (action workers busy)"
                                         : "Enshrouded plugin call '" + job->action + "' timed out after " +
                                               std::to_string(o_.config.actionTimeoutMs) + "ms";
        JsonValue fb;
        bool fallback = job->action == "getPlayerLocation" && EventLocationFallback(job->args, now, fb);
        errorResponses_ += !fallback;
        PluginLog("native: Takaro request %s: %s", job->requestId.c_str(), error.c_str());
        SendResponse(job->epoch, FrameKind::CriticalResponse,
                     fallback ? CreateResponse(job->requestId, fb) : CreateErrorResponse(job->requestId, error));
        job->responded = true;
        pendingIds_.erase(job->requestId);
        if (neverStarted) {
            if (!job->intentId.empty()) {
                auto& intents = o_.store->BanIntents();
                intents.erase(std::remove_if(intents.begin(), intents.end(),
                                             [&](const JsonValue& v) {
                                                 const JsonValue* id = v.get("id");
                                                 return id && id->str == job->intentId;
                                             }),
                              intents.end());
                o_.store->SaveBanIntents();
            }
            it = inflight_.erase(it);
        } else {
            ++it;  // keep it: its completion still records state changes
        }
    }
}

void Bridge::RememberPosition(const JsonValue& args, const JsonValue& payload) {
    const JsonValue* x = payload.get("x");
    const JsonValue* y = payload.get("y");
    const JsonValue* z = payload.get("z");
    if (!x || !y || !z || x->type != JsonValue::Number || y->type != JsonValue::Number || z->type != JsonValue::Number)
        return;
    try {
        JsonValue pos = JObj();
        Put(pos, "x", *x);
        Put(pos, "y", *y);
        Put(pos, "z", *z);
        lastPositions_[PlayerId(args)] = pos;
    } catch (const std::exception&) {
        // no identifier: nothing to remember
    }
}

void Bridge::NoteConnectionEvent(const std::string& type, const JsonValue& data, int64_t nowMs) {
    if (!IsConnectionType(type)) return;
    const JsonValue& player = AsRecord(data.get("player"));
    for (auto* key : {"gameId", "steamId"}) {
        const JsonValue* v = player.get(key);
        if (v && v->type == JsonValue::String && !v->str.empty()) windows_[v->str] = nowMs + kLocationFallbackMs;
    }
}

bool Bridge::EventLocationFallback(const JsonValue& args, int64_t nowMs, JsonValue& out) {
    std::string id;
    try {
        id = PlayerId(args);
    } catch (const std::exception&) {
        return false;
    }
    auto it = windows_.find(id);
    if (it == windows_.end()) return false;
    if (it->second < nowMs) {
        windows_.erase(it);
        return false;
    }
    auto pos = lastPositions_.find(id);
    if (pos != lastPositions_.end()) {
        out = pos->second;
    } else {
        out = JObj();
        Put(out, "x", JNum(0));
        Put(out, "y", JNum(0));
        Put(out, "z", JNum(0));
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ events

void Bridge::PollEvents(int64_t now) {
    if (!o_.store->OutboxUsable()) return;
    OutboxState& ob = o_.store->Outbox();
    for (int round = 0; round < 8; round++) {
        GameResponse r = o_.game->Call("GET", "/events?since=" + std::to_string(ob.scan.seq) + "&limit=512", "");
        JsonValue v;
        if (r.status != 200 || !ParseJson(r.body, v) || v.type != JsonValue::Object) {
            lastError_ = "plugin /events failed (HTTP " + std::to_string(r.status) + ")";
            return;
        }
        std::string boot = Str(v.get("bootId")).value_or("");
        if (!boot.empty() && boot != ob.scan.bootId) {
            if (!ob.scan.bootId.empty()) {
                PluginLog("native: plugin reports a new server process (bootId %s -> %s); reconciling online players",
                          ob.scan.bootId.c_str(), boot.c_str());
                reconcilePending_ = true;
            }
            ob.scan = {boot, 0};
            outboxDirty_ = true;
            continue;
        }
        const JsonValue* events = v.get("events");
        if (!events || events->type != JsonValue::Array || events->arr.empty()) return;
        const JsonValue* truncated = v.get("truncated");
        if (truncated && truncated->type == JsonValue::Bool && truncated->b) {
            auto first = Num(events->arr.front().get("seq"));
            if (first && *first > (double)ob.scan.seq + 1) {
                uint64_t gap = (uint64_t)*first - ob.scan.seq - 1;
                ringGaps_ += gap;
                PluginLog("native: %llu plugin events were overwritten in the ring before they could be read",
                          (unsigned long long)gap);
            }
        }
        for (auto& e : events->arr) {
            auto seqNum = Num(e.get("seq"));
            if (!seqNum) continue;
            uint64_t seq = (uint64_t)*seqNum;
            if (seq <= ob.scan.seq) continue;
            const JsonValue* t = e.get("type");
            std::string type = t && t->type == JsonValue::String ? t->str : "";
            std::optional<MappedEvent> mapped;
            try {
                mapped = MapPluginEvent(type, e.get("data"));
            } catch (const std::exception& ex) {
                eventsMalformed_++;
                PluginLog("native: dropping malformed plugin event seq=%llu: %s", (unsigned long long)seq, ex.what());
            }
            if (mapped) {
                bool suppressed = tailActive_ && IsConnectionType(mapped->type);
                bool filtered = mapped->type == "log" && !ShouldForwardLog(o_.config.logEvents, mapped->data);
                if (!suppressed && !filtered) {
                    AdmitEvent(mapped->type, mapped->data, {boot, seq}, now);
                    if (mapped->type != "log")
                        PluginLog("native: admitted plugin %s: %s", mapped->type.c_str(), JsonDump(mapped->data).c_str());
                }
            }
            ob.scan.seq = seq;
            outboxDirty_ = true;
        }
        if (events->arr.size() < 512) return;
    }
}

void Bridge::AdmitEvent(const std::string& type, const JsonValue& data, const SourceCursor& source, int64_t now) {
    OutboxState& ob = o_.store->Outbox();
    PendingEvent pe;
    pe.id = ob.nextId++;
    pe.source = source;
    pe.type = type;
    pe.frame = CreateGameEvent(type, data);
    uint64_t lossesBefore = ob.losses;
    o_.store->Admit(std::move(pe));
    if (ob.losses != lossesBefore && (ob.losses < 10 || ob.losses % 500 == 0))
        PluginLog("native: event outbox full; %llu events dropped so far", (unsigned long long)ob.losses);
    eventsAdmitted_++;
    outboxDirty_ = true;
    ObserveConnection(type, data, now);
}

void Bridge::ObserveConnection(const std::string& type, const JsonValue& data, int64_t now) {
    (void)now;
    if (!IsConnectionType(type)) return;
    const JsonValue& player = AsRecord(data.get("player"));
    const std::string* gid = GameIdOf(player);
    if (!gid) return;
    auto& online = o_.store->Online();
    online.erase(std::remove_if(online.begin(), online.end(),
                                [&](const JsonValue& p) {
                                    const std::string* g = GameIdOf(p);
                                    return g && *g == *gid;
                                }),
                 online.end());
    if (type == "player-connected") {
        online.push_back(player);
        if (o_.store->Remember(player, o_.wallMs())) knownDirty_ = viewDirty_ = true;
    }
    missingStrikes_.erase(*gid);
    onlineDirty_ = true;
}

void Bridge::DeliverEvents(int64_t now) {
    if (!open_ || !identified_ || !o_.store->OutboxUsable()) return;
    auto& pending = o_.store->Outbox().pending;
    auto it = std::upper_bound(pending.begin(), pending.end(), lastQueuedId_,
                               [](uint64_t id, const PendingEvent& e) { return id < e.id; });
    size_t queued = 0;
    for (; it != pending.end() && queued < kNoticeBatch; ++it) {
        QueueStatus st = o_.transport->Queue({FrameKind::Event, Text(it->frame), epoch_, it->id});
        if (st != QueueStatus::Accepted) {
            if (st == QueueStatus::TooLarge) {
                // cannot ever be sent: count it as lost rather than blocking the outbox forever
                PluginLog("native: dropping an event frame too large to send (%zu bytes)", it->frame.size());
                lastQueuedId_ = it->id;
                continue;
            }
            break;
        }
        lastQueuedId_ = it->id;
        queued++;
        eventsQueued_++;
        if (IsConnectionType(it->type)) {
            JsonValue frame;
            if (ParseJson(it->frame, frame))
                NoteConnectionEvent(it->type, AsRecord(AsRecord(frame.get("payload")).get("data")), now);
        }
    }
}

// ------------------------------------------------------------------------------------------------ periodic

void Bridge::Reconcile(bool immediate, int64_t now) {
    nextReconcile_ = now + o_.reconcileIntervalMs;
    if (o_.store->Fenced("online") || !o_.store->OutboxUsable()) {
        reconcilePending_ = false;
        return;
    }
    auto& online = o_.store->Online();
    if (online.empty()) {
        reconcilePending_ = false;
        missingStrikes_.clear();
        return;
    }
    GameResponse r = o_.game->Call("GET", "/players", "");
    JsonValue players;
    std::set<std::string> current;
    try {
        if (r.status != 200 || !ParseJson(r.body, players) || players.type != JsonValue::Array)
            throw NativeError(ErrorKind::Plain, "HTTP " + std::to_string(r.status));
        for (auto& p : players.arr) current.insert(MapPlayer(&p).get("gameId")->str);
    } catch (const std::exception& e) {
        PluginLog("native: online reconciliation postponed, plugin getPlayers failed: %s", e.what());
        return;
    }
    std::vector<JsonValue> snapshot = online;
    for (auto& player : snapshot) {
        const std::string* gid = GameIdOf(player);
        if (!gid) continue;
        if (current.count(*gid)) {
            missingStrikes_.erase(*gid);
            continue;
        }
        // periodic check: two misses in a row, so a disconnect already on its way through the ring wins
        if (!immediate && ++missingStrikes_[*gid] < 2) continue;
        JsonValue data = JObj();
        Put(data, "player", player);
        std::string name = Str(player.get("name")).value_or("?");
        PluginLog("native: reconcile: %s (%s) is no longer on the server; queued player-disconnected", name.c_str(),
                  gid->c_str());
        AdmitEvent("player-disconnected", data, {o_.store->Outbox().scan.bootId, 0}, now);
        reconciled_++;
    }
    reconcilePending_ = false;
}

void Bridge::RefreshHealth(int64_t now) {
    (void)now;
    GameResponse r = o_.game->Call("GET", "/health", "");
    JsonValue h;
    bool ok = r.status == 200 && ParseJson(r.body, h) && h.type == JsonValue::Object;
    healthKnown_ = ok;
    pluginStatus_ = ok ? Str(h.get("status")).value_or("") : "unreachable";
    bool want = ShouldTailLog(o_.config.logTail, ok ? &h : nullptr);
    if (want && !tailActive_) {
        PluginLog("native: enabling log-tail fallback for connect/disconnect (%s)", tailer_.File().c_str());
        tailer_.Start();
    } else if (!want && tailActive_) {
        PluginLog("native: plugin connection events healthy; disabling log-tail fallback");
        tailer_.Stop();
    }
    tailActive_ = want;
}

void Bridge::PollTail(int64_t now) {
    if (!tailActive_) return;
    std::string err;
    auto events = tailer_.Poll(&err);
    for (auto& ev : events) {
        const JsonValue& p = AsRecord(ev.data.get("player"));
        PluginLog("native: log tail %s: %s (%s)", ev.type.c_str(), Str(p.get("name")).value_or("?").c_str(),
                  Str(p.get("gameId")).value_or("?").c_str());
        if (!o_.store->OutboxUsable()) continue;
        AdmitEvent(ev.type, ev.data, {o_.store->Outbox().scan.bootId, 0}, now);
        tailEvents_++;
    }
}

void Bridge::CheckTimedBans(int64_t now) {
    nextBanCheck_ = now + o_.banCheckIntervalMs;
    if (!o_.store->BanStoreError().empty()) return;
    int64_t wall = o_.wallMs();
    for (auto& kv : o_.store->TimedBans()) {
        TimedBan& b = kv.second;
        if (b.expiresAtMs > wall || b.nextAttemptMs > now || expiryInFlight_.count(b.gameId)) continue;
        auto job = std::make_shared<Job>();
        job->id = nextJobId_++;
        job->expiry = true;
        job->expiryGameId = b.gameId;
        job->action = "expireTimedBan";
        job->deadlineMs = now + 600000;
        job->view = View();
        expiryInFlight_.insert(b.gameId);
        inflight_[job->id] = job;
        {
            std::lock_guard<std::mutex> g(mu_);
            jobQueue_.push_back(job);
        }
        jobCv_.notify_one();
    }
}

void Bridge::RecoverBanIntents() {
    auto& intents = o_.store->BanIntents();
    if (intents.empty() || !o_.store->BanStoreError().empty()) return;
    auto& bans = o_.store->TimedBans();
    int64_t wall = o_.wallMs();
    for (auto& in : intents) {
        std::string op = Str(in.get("op")).value_or("");
        std::string gid = Str(in.get("gameId")).value_or("");
        if (gid.empty()) continue;
        auto expires = Str(in.get("expiresAt"));
        int64_t ms = 0;
        if (op == "ban" && expires && ParseIsoMs(*expires, ms)) {
            // The game may or may not have applied it: keep the expiry so it can never turn permanent.
            TimedBan b;
            b.gameId = gid;
            b.expiresAt = *expires;
            b.expiresAtMs = ms;
            b.reason = Str(in.get("reason")).value_or("");
            b.createdAtMs = wall;
            bans[gid] = b;
            PluginLog("native: recovered an interrupted timed ban for %s (until %s)", gid.c_str(), expires->c_str());
        } else if (op == "ban") {
            bans.erase(gid);
            PluginLog("native: recovered an interrupted permanent ban for %s", gid.c_str());
        } else if (op == "unban") {
            // Finish what Takaro asked for: the expiry loop lifts it now.
            TimedBan b;
            b.gameId = gid;
            b.expiresAt = FormatIsoMs(wall);
            b.expiresAtMs = wall;
            b.createdAtMs = wall;
            bans[gid] = b;
            PluginLog("native: recovered an interrupted unban for %s; retrying it", gid.c_str());
        }
    }
    intents.clear();
    o_.store->SaveTimedBans();
    o_.store->SaveBanIntents();
    viewDirty_ = true;
}

void Bridge::FlushState(int64_t now, bool force) {
    // The outbox file is rewritten whole: while Takaro is away and it grows, write it less often
    // (250 ms when small, up to 5 s near the 32 MiB bound).
    int64_t outboxGap = std::min<int64_t>(5000, 250 * (1 + (int64_t)(o_.store->Outbox().pendingBytes >> 18)));
    if (outboxDirty_ && (force || now - lastOutboxSave_ >= outboxGap)) {
        lastOutboxSave_ = now;
        outboxDirty_ = !o_.store->SaveOutbox() && o_.store->OutboxUsable();
    }
    if (onlineDirty_) onlineDirty_ = !o_.store->SaveOnline() && !o_.store->Fenced("online");
    if (knownDirty_ && (force || now - lastKnownSave_ >= 5000)) {
        lastKnownSave_ = now;
        knownDirty_ = !o_.store->SaveKnown() && !o_.store->Fenced("known");
    }
    auto errors = o_.store->Errors();
    std::string detail;
    for (auto& kv : errors) detail += (detail.empty() ? "" : "; ") + kv.first + ": " + kv.second;
    std::string state = detail.empty() ? "ok" : "degraded";
    if (state + detail != lastConnectorState_) {
        lastConnectorState_ = state + detail;
        o_.game->SetCapability("connectorState", state,
                               detail.empty() ? "native connector state files healthy" : "connector state: " + detail);
    }
}

std::string Bridge::Redact(std::string text) const {
    for (const std::string* secret : {&o_.config.identityToken, &o_.config.registrationToken}) {
        if (secret->size() < 4) continue;
        for (size_t at = 0; (at = text.find(*secret, at)) != std::string::npos;) {
            text.replace(at, secret->size(), "[redacted]");
            at += 10;
        }
    }
    return text;
}

void Bridge::Publish() {
    OutboxState& ob = o_.store->Outbox();
    std::string errors = "{";
    bool first = true;
    for (auto& kv : o_.store->Errors()) {
        errors += (first ? "" : ",") + JsonStr(kv.first) + ":" + JsonStr(kv.second);
        first = false;
    }
    errors += "}";
    size_t userJobs = 0, running = 0;
    for (auto& kv : inflight_) {
        userJobs += !kv.second->expiry;
        running += kv.second->state == kRunning;
    }
    uint64_t overflows;
    {
        std::lock_guard<std::mutex> g(mu_);
        overflows = noticeOverflows_;
    }
    std::string state = !open_ ? "connecting" : identified_ ? "identified" : "open";
    std::string h = ObjBuilder()
                        .B("enabled", true)
                        .S("state", state)
                        .Raw("epoch", std::to_string(epoch_))
                        .B("identified", identified_)
                        .S("gameServerId", gameServerId_)
                        .S("lastError", lastError_)
                        .S("lastIdentifyError", lastIdentifyError_)
                        .Raw("config", ConfigSummaryJson(o_.config))
                        .Raw("transport", o_.transport ? o_.transport->StatsJson() : "null")
                        .Raw("requests", ObjBuilder()
                                             .N("received", (double)requests_)
                                             .N("responses", (double)responses_)
                                             .N("errors", (double)errorResponses_)
                                             .N("duplicates", (double)duplicates_)
                                             .N("oversizeRequestIds", (double)oversizeIds_)
                                             .N("malformedFrames", (double)malformed_)
                                             .N("overloads", (double)overloads_)
                                             .N("timeouts", (double)timeouts_)
                                             .N("lateResults", (double)lateResults_)
                                             .N("droppedResponses", (double)droppedResponses_)
                                             .N("identifyErrors", (double)identifyErrors_)
                                             .N("noticeOverflows", (double)overflows)
                                             .N("pendingActions", (double)userJobs)
                                             .N("runningActions", (double)running)
                                             .N("maxLatencyMs", (double)maxRequestLatencyMs_)
                                             .S("lastAction", lastRequestAction_)
                                             .Done())
                        .Raw("outbox", ObjBuilder()
                                           .B("usable", o_.store->OutboxUsable())
                                           .N("pending", (double)ob.pending.size())
                                           .N("pendingBytes", (double)ob.pendingBytes)
                                           .Raw("queuedThrough", std::to_string(lastQueuedId_))
                                           .Raw("scan", CursorJson(ob.scan))
                                           .Raw("confirmed", CursorJson(ob.confirmed))
                                           .N("confirmedTotal", (double)ob.confirmedTotal)
                                           .N("losses", (double)ob.losses)
                                           .N("admitted", (double)eventsAdmitted_)
                                           .N("queued", (double)eventsQueued_)
                                           .N("malformedDropped", (double)eventsMalformed_)
                                           .N("ringGaps", (double)ringGaps_)
                                           .Done())
                        .N("onlinePlayers", (double)o_.store->Online().size())
                        .N("knownPlayers", (double)o_.store->Known().size())
                        .N("reconciledDisconnects", (double)reconciled_)
                        .Raw("timedBans", ObjBuilder()
                                              .N("active", (double)o_.store->TimedBans().size())
                                              .N("expired", (double)expiredBans_)
                                              .N("intents", (double)o_.store->BanIntents().size())
                                              .S("storeError", o_.store->BanStoreError())
                                              .Done())
                        .Raw("logTail", ObjBuilder()
                                            .B("active", tailActive_)
                                            .S("file", tailer_.File())
                                            .N("events", (double)tailEvents_)
                                            .S("pluginStatus", pluginStatus_)
                                            .Done())
                        .Raw("persistence", ObjBuilder()
                                                .S("dir", o_.store->Paths().dir)
                                                .N("writes", (double)o_.store->WriteCount())
                                                .Raw("errors", errors)
                                                .Done())
                        .Done();
    std::lock_guard<std::mutex> g(healthMu_);
    health_ = std::move(h);
}

}  // namespace native
