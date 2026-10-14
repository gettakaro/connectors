#include "bridge.h"

#include "jsonmini.h"
#include "log.h"
#include "version.h"

#include <thread>

namespace native {

Bridge& Bridge::Get() {
    static Bridge b;
    return b;
}

bool Bridge::Push(uint64_t gen, Notice&& n) {
    InboxItem it;
    switch (n.type) {
        case NoticeType::Open: it.kind = "open"; break;
        case NoticeType::Frame: it.kind = "frame"; break;
        case NoticeType::Confirmed: it.kind = "confirmed"; break;
        case NoticeType::Closed: it.kind = "closed"; break;
        case NoticeType::Error: it.kind = "error"; break;
    }
    it.epoch = (gen << 32) | (n.epoch & 0xffffffffull);
    if (n.type == NoticeType::Confirmed) it.text = std::to_string(n.outboxId);
    else it.text = std::move(n.text);
    std::lock_guard<std::mutex> g(mu_);
    if (gen != gen_) return true;  // a detached transport winding down
    if (n.type == NoticeType::Open) currentEpoch_ = it.epoch;
    if (n.type == NoticeType::Closed && currentEpoch_ == it.epoch) currentEpoch_ = 0;
    // Lifecycle notices are always accepted; only frames can overflow (then the transport drops the link).
    bool lifecycle = n.type != NoticeType::Frame;
    if (!lifecycle && (inbox_.size() >= kInboxMaxItems || inboxBytes_ + it.text.size() > kInboxMaxBytes)) {
        dropped_++;
        return false;
    }
    inboxBytes_ += it.text.size();
    inbox_.push_back(std::move(it));
    return true;
}

bool Bridge::Connect(const std::string& url, const std::string& caFile) {
    if (url.compare(0, 6, "wss://") != 0) {
        PluginLog("native: refusing Takaro URL that is not wss:// (%zu chars)", url.size());
        return false;
    }
    std::lock_guard<std::mutex> l(lifeMu_);
    if (t_) {
        if (url != url_) {
            url_ = url;
            t_->Retarget(url, true);
        }
        return true;
    }
    WinHttpTransport::Options o;
    o.url = url;
    o.caFile = caFile;
    url_ = url;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> g(mu_);
        gen = ++gen_;
        inbox_.clear();
        inboxBytes_ = 0;
        currentEpoch_ = 0;
    }
    t_ = std::make_unique<WinHttpTransport>(o);
    t_->Start([this, gen](Notice&& n) { return Push(gen, std::move(n)); });
    return true;
}

void Bridge::Disconnect() {
    std::unique_ptr<WinHttpTransport> t;
    {
        std::lock_guard<std::mutex> l(lifeMu_);
        t = std::move(t_);
    }
    if (t) t->Stop();
}

void Bridge::DisconnectAsync() {
    std::unique_ptr<WinHttpTransport> t;
    {
        std::lock_guard<std::mutex> l(lifeMu_);
        t = std::move(t_);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        gen_++;  // notices still coming from the detached transport are dropped
        inbox_.clear();
        inboxBytes_ = 0;
        currentEpoch_ = 0;
    }
    if (t) std::thread([p = t.release()] {
        p->Stop();
        delete p;
    }).detach();
}

QueueStatus Bridge::Send(const std::string& text, const std::string& kind, uint64_t epoch, uint64_t* outboxId) {
    OutFrame f;
    if (kind == "control") f.kind = FrameKind::Control;
    else if (kind == "critical") f.kind = FrameKind::CriticalResponse;
    else if (kind == "event") f.kind = FrameKind::Event;
    else f.kind = FrameKind::Response;
    f.text = std::make_shared<const std::string>(text);
    {
        std::lock_guard<std::mutex> g(mu_);
        uint64_t e = epoch ? epoch : currentEpoch_;
        if (!e) return QueueStatus::Disconnected;
        if ((e >> 32) != gen_) return QueueStatus::StaleEpoch;
        f.epoch = e & 0xffffffffull;
        if (f.kind == FrameKind::Event) f.outboxId = ++outboxSeq_;
    }
    if (outboxId) *outboxId = f.outboxId;
    std::lock_guard<std::mutex> l(lifeMu_);
    if (!t_) return QueueStatus::Disconnected;
    return t_->Queue(std::move(f));
}

bool Bridge::Poll(InboxItem& out) {
    std::lock_guard<std::mutex> g(mu_);
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front());
    inbox_.pop_front();
    inboxBytes_ -= out.text.size();
    return true;
}

void Bridge::Close(uint64_t epoch, const std::string& reason) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if ((epoch >> 32) != gen_) return;
    }
    std::lock_guard<std::mutex> l(lifeMu_);
    if (t_) t_->RequestClose(epoch & 0xffffffffull, reason);
}

void Bridge::MarkIdentified(uint64_t epoch) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if ((epoch >> 32) != gen_) return;
    }
    std::lock_guard<std::mutex> l(lifeMu_);
    if (t_) t_->MarkIdentified(epoch & 0xffffffffull);
}

std::string Bridge::StatusJson() {
    std::string transport = "null";
    {
        std::lock_guard<std::mutex> l(lifeMu_);
        if (t_) transport = t_->StatsJson();
    }
    std::lock_guard<std::mutex> g(mu_);
    return ObjBuilder()
        .S("version", TAKARO_NATIVE_VERSION_STRING)
        .Raw("epoch", std::to_string(currentEpoch_))
        .N("inboxItems", (double)inbox_.size())
        .N("inboxBytes", (double)inboxBytes_)
        .N("inboxDropped", (double)dropped_)
        .Raw("transport", transport)
        .Done();
}

}  // namespace native
