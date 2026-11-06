#include "takaro_client.h"

#include "version.h"

#include <algorithm>

namespace takaro {

namespace {
const char* kGameSlug = "ark-ascended";
const char* kConnectorName = "takaro-ark-ascended";

const tj::Value& Record(const tj::Value* v) {
    static const tj::Value empty = tj::Value::object();
    return v && v->is_object() ? *v : empty;
}

std::string Str(const tj::Value* v) { return v && v->is_string() ? v->as_string() : std::string(); }

std::string Clip(std::string s, size_t n) {
    if (s.size() > n) s.resize(n);
    return s;
}
}  // namespace

tj::Value NormalizeArgs(const tj::Value* raw) {
    if (raw == nullptr) return tj::Value::object();
    if (raw->is_object()) return *raw;
    if (raw->is_string()) {
        auto parsed = tj::parse(raw->as_string());
        if (parsed && parsed->is_object()) return *parsed;
    }
    return tj::Value::object();
}

std::string PlayerIdOf(const tj::Value& args) {
    std::string id = Str(args.get("gameId"));
    if (!id.empty()) return id;
    return Str(Record(args.get("player")).get("gameId"));
}

Client::Client(Config config, native::ITransport* transport, std::string streamId, LogFn log)
    : config_(std::move(config)), transport_(transport), streamId_(std::move(streamId)), log_(std::move(log)) {}

Client::~Client() { Stop(); }

bool Client::Start() {
    if (!transport_->Start([this](native::Notice&& n) { return Enqueue(std::move(n)); })) return false;
    worker_ = std::thread([this] { Run(); });
    return true;
}

bool Client::StartManual() {
    return transport_->Start([this](native::Notice&& n) { return Enqueue(std::move(n)); });
}

void Client::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    transport_->Stop();
}

bool Client::Enqueue(native::Notice&& n) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (notices_.size() >= 4096) return false;
        notices_.push_back(std::move(n));
    }
    cv_.notify_all();
    return true;
}

void Client::Run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return stopping_ || !notices_.empty() || !finished_.empty();
            });
            if (stopping_) return;
        }
        PumpOnce();
    }
}

void Client::PumpOnce() {
    std::deque<native::Notice> notices;
    std::deque<Finished> finished;
    {
        std::lock_guard<std::mutex> lock(mu_);
        notices.swap(notices_);
        finished.swap(finished_);
    }
    for (auto& n : notices) HandleNotice(n);
    for (auto& f : finished) {
        bool wanted;
        {
            std::lock_guard<std::mutex> lock(mu_);
            wanted = pending_.erase(f.call.requestId) > 0;
        }
        if (wanted) Respond(f.call.epoch, f.call.requestId, std::move(f.outcome), f.call.action);
    }
    ExpireCalls();
    PumpEvents();
}

void Client::HandleNotice(native::Notice& n) {
    switch (n.type) {
        case native::NoticeType::Open:
            epoch_ = n.epoch;
            SendIdentify(n.epoch);
            break;
        case native::NoticeType::Frame:
            HandleFrame(n.epoch, n.text);
            break;
        case native::NoticeType::Confirmed:
            if (protocol_.load() == 0) DropAcked(n.outboxId);
            break;
        case native::NoticeType::Closed:
        case native::NoticeType::Error: {
            if (n.epoch != 0 && n.epoch != epoch_) break;
            identified_ = false;
            protocol_ = 0;
            std::lock_guard<std::mutex> lock(mu_);
            eventsSent_ = 0;  // unacked events go out again; Takaro dedupes by (streamId, seq)
            break;
        }
    }
}

std::string Client::IdentifyFrame() const {
    tj::Value payload = tj::Value::object();
    payload.set("identityToken", tj::str(config_.identityToken));
    if (!config_.registrationToken.empty()) payload.set("registrationToken", tj::str(config_.registrationToken));
    if (!config_.serverName.empty()) payload.set("name", tj::str(config_.serverName));
    payload.set("game", tj::str(kGameSlug));
    payload.set("connector", tj::obj({{"name", tj::str(kConnectorName)}, {"version", tj::str(TAKARO_PLUGIN_VERSION)}}));
    tj::Value versions = tj::Value::array();
    versions.push(tj::num(0));
    versions.push(tj::num(1));
    payload.set("protocolVersions", std::move(versions));
    tj::Value capabilities = tj::Value::array();
    if (manifest_) for (auto& extra : manifest_()) capabilities.push(std::move(extra));
    payload.set("capabilities", std::move(capabilities));
    return tj::dump(tj::obj({{"type", tj::str("identify")}, {"payload", std::move(payload)}}));
}

void Client::SendIdentify(uint64_t epoch) {
    native::OutFrame f;
    f.kind = native::FrameKind::Control;
    f.text = std::make_shared<const std::string>(IdentifyFrame());
    f.epoch = epoch;
    transport_->Queue(std::move(f));
    log_("takaro: identify sent");  // the frame holds tokens, never log it
}

void Client::HandleFrame(uint64_t epoch, const std::string& raw) {
    if (config_.wireDebug) log_("takaro-wire RECV " + Clip(raw, 2000));
    auto message = tj::parse(raw);
    if (!message || !message->is_object()) {
        log_("takaro: invalid message ignored");
        return;
    }
    const std::string kind = Str(message->get("type"));
    if (kind == "identifyResponse") {
        const tj::Value& payload = Record(message->get("payload"));
        const tj::Value* failure = message->get("error") ? message->get("error") : payload.get("error");
        if (failure && !failure->is_null()) {
            log_("takaro: identify rejected: " + Clip(tj::dump(*failure), 500));
            transport_->RequestClose(epoch, "identify rejected");
            return;
        }
        std::string id = Str(payload.get("gameServerId"));
        if (id.empty()) id = Str(Record(payload.get("server")).get("id"));
        {
            std::lock_guard<std::mutex> lock(idMu_);
            gameServerId_ = id;
        }
        const tj::Value* v = payload.get("protocolVersion");
        protocol_ = (v && v->is_number() && v->as_number() == 1) ? 1 : 0;
        identified_ = true;
        transport_->MarkIdentified(epoch);
        log_("takaro: identified gameServerId=" + id + " protocol=" + std::to_string(protocol_.load()));
        return;
    }
    if (kind == "eventAck") {
        const tj::Value& payload = Record(message->get("payload"));
        const tj::Value* upTo = payload.get("upTo");
        if (Str(payload.get("streamId")) == streamId_ && upTo && upTo->is_number() && upTo->as_number() >= 0)
            DropAcked(static_cast<uint64_t>(upTo->as_number()));
        return;
    }
    if (kind == "eventError") {
        log_("takaro: event rejected: " + Clip(raw, 800));
        return;
    }
    if (kind == "request") {
        HandleRequest(epoch, *message);
        return;
    }
    if (kind == "error") log_("takaro: error frame: " + Clip(raw, 800));
}

void Client::HandleRequest(uint64_t epoch, const tj::Value& message) {
    const std::string requestId = Str(message.get("requestId"));
    if (requestId.empty()) return;
    const tj::Value& payload = Record(message.get("payload"));
    GameCall call;
    call.requestId = requestId;
    call.action = Str(payload.get("action"));
    call.args = NormalizeArgs(payload.get("args"));
    call.epoch = epoch;
    if (call.action.empty()) {
        Respond(epoch, requestId, Outcome::Fail("invalid_args", "request has no action"), call.action);
        return;
    }
    if (call.action == "testReachability" && reachability_) {
        Respond(epoch, requestId, Outcome::Ok(reachability_()), call.action);
        return;
    }
    Outcome local;
    if (localAction_ && localAction_(call.action, local)) {
        Respond(epoch, requestId, std::move(local), call.action);
        return;
    }
    bool full = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        full = calls_.size() >= config_.maxPendingCalls;
        if (!full) {
            pending_[requestId] = std::chrono::steady_clock::now() + config_.callDeadline;
            calls_.push_back(std::move(call));
        }
    }
    if (full) Respond(epoch, requestId, Outcome::Fail("game_error", "connector is busy"), "");
}

void Client::Respond(uint64_t epoch, const std::string& requestId, Outcome outcome, const std::string& action) {
    tj::Value frame = tj::Value::object();
    frame.set("type", tj::str("response"));
    frame.set("requestId", tj::str(requestId));
    if (outcome.errorCode.empty()) {
        frame.set("payload", std::move(outcome.payload));
    } else {
        log_("takaro: action " + Clip(action, 64) + " failed: " + outcome.errorCode + " " + Clip(outcome.errorMessage, 300));
        if (protocol_.load() < 1) return;  // v0 has no failure shape: stay silent, Takaro times out
        frame.set("payload", tj::Value::null());
        frame.set("error", tj::obj({{"code", tj::str(outcome.errorCode)}, {"message", tj::str(Clip(outcome.errorMessage, 512))}}));
    }
    native::OutFrame f;
    f.kind = native::FrameKind::Response;
    f.text = std::make_shared<const std::string>(tj::dump(frame));
    f.epoch = epoch;
    if (config_.wireDebug) log_("takaro-wire SEND " + Clip(*f.text, 2000));
    const auto status = transport_->Queue(std::move(f));
    if (status != native::QueueStatus::Accepted)
        log_(std::string("takaro: response not queued: ") + native::QueueStatusName(status));
}

void Client::ExpireCalls() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired;
    uint64_t epoch = epoch_;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second <= now) {
                expired.push_back(it->first);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& id : expired)
        Respond(epoch, id, Outcome::Fail("game_error", "the game thread did not run this action in time"), "timeout");
}

bool Client::NextCall(GameCall& out) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!calls_.empty()) {
        GameCall call = std::move(calls_.front());
        calls_.pop_front();
        if (pending_.count(call.requestId) == 0) continue;  // already answered as timed out
        out = std::move(call);
        return true;
    }
    return false;
}

void Client::Complete(const GameCall& call, Outcome outcome) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        finished_.push_back({call, std::move(outcome)});
    }
    cv_.notify_all();
}

void Client::Emit(const std::string& type, tj::Value data) {
    QueuedEvent e;
    {
        std::lock_guard<std::mutex> lock(mu_);
        e.seq = nextSeq_++;
    }
    // Built separately: tj::Value copies share their object, so set() on a copy would change both.
    tj::Value v0 = tj::obj({{"type", tj::str(type)}, {"data", data}});
    tj::Value v1 = tj::obj({{"type", tj::str(type)}, {"data", data}});
    v1.set("streamId", tj::str(streamId_));
    v1.set("seq", tj::num(static_cast<double>(e.seq)));
    e.v0 = std::make_shared<const std::string>(tj::dump(tj::obj({{"type", tj::str("gameEvent")}, {"payload", std::move(v0)}})));
    e.v1 = std::make_shared<const std::string>(tj::dump(tj::obj({{"type", tj::str("gameEvent")}, {"payload", std::move(v1)}})));
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (events_.size() >= config_.maxQueuedEvents) {
            events_.pop_front();
            if (eventsSent_ > 0) --eventsSent_;
            ++droppedEvents_;
        }
        events_.push_back(std::move(e));
    }
    cv_.notify_all();
}

void Client::PumpEvents() {
    if (!identified_.load()) return;
    const bool v1 = protocol_.load() >= 1;
    for (;;) {
        native::OutFrame f;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (eventsSent_ >= events_.size() || eventsSent_ >= config_.eventWindow) return;
            const QueuedEvent& e = events_[eventsSent_];
            f.kind = native::FrameKind::Event;
            f.text = v1 ? e.v1 : e.v0;
            f.epoch = epoch_;
            f.outboxId = e.seq;
        }
        if (config_.wireDebug) log_("takaro-wire SEND " + Clip(*f.text, 2000));
        if (transport_->Queue(std::move(f)) != native::QueueStatus::Accepted) return;  // retried on the next pass
        std::lock_guard<std::mutex> lock(mu_);
        ++eventsSent_;
    }
}

void Client::DropAcked(uint64_t upTo) {
    std::lock_guard<std::mutex> lock(mu_);
    while (!events_.empty() && events_.front().seq <= upTo) {
        events_.pop_front();
        if (eventsSent_ > 0) --eventsSent_;
    }
}

std::string Client::GameServerId() {
    std::lock_guard<std::mutex> lock(idMu_);
    return gameServerId_;
}

}  // namespace takaro
