#include "transport_lws.h"

#include "common.h"
#include "takaro/json_util.h"

#include <libwebsockets.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace takaro {

namespace {
int64_t SteadyMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

struct LwsTransport::Impl {
    LwsConfig cfg;
    NoticeSink sink;
    std::thread worker;
    std::atomic<bool> stopping{false};

    // shared (mu)
    std::mutex mu;
    lws_context* context = nullptr;
    FrameQueues queues;
    uint64_t epoch = 0;  // current connection's epoch (0 before the first)
    bool connected = false;
    uint64_t closeEpoch = 0;
    std::string closeReason;
    uint64_t identifiedEpoch = 0;
    std::string lastError;
    uint64_t connects = 0, connectFailures = 0, framesSent = 0, bytesSent = 0, framesReceived = 0;
    int64_t backoffMs = 0;
    Heartbeat hb;

    // service thread only
    struct Timer {
        lws_sorted_usec_list sul;  // first member: the callback gets &sul and casts back
        Impl* owner;
    } serviceTimer{{}, this};
    lws* wsi = nullptr;
    bool open = false;
    bool timeoutSet = false;
    std::string inbound;

    bool Notify(NoticeType type, uint64_t atEpoch, std::string text = {}, uint64_t outboxId = 0) {
        if (!sink) return true;
        Notice n;
        n.type = type;
        n.epoch = atEpoch;
        n.text = std::move(text);
        n.outboxId = outboxId;
        return sink(std::move(n));
    }

    void Error(uint64_t atEpoch, const std::string& text) {
        {
            std::lock_guard<std::mutex> g(mu);
            lastError = text;
        }
        Notify(NoticeType::Error, atEpoch, text);
    }

    int Callback(lws* w, lws_callback_reasons reason, void* in, size_t len);
    void Run();
};

namespace {
int CallbackThunk(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    (void)user;
    lws_context* ctx = lws_get_context(wsi);
    auto* impl = ctx ? static_cast<LwsTransport::Impl*>(lws_context_user(ctx)) : nullptr;
    if (!impl) return 0;
    try {
        return impl->Callback(wsi, reason, in, len);
    } catch (...) {
        // A C callback must never unwind into libwebsockets or the game process.
        return -1;
    }
}

// lws 4 ignores lws_service()'s timeout and sleeps until its next scheduled event, so a repeating
// timer is what wakes the loop for heartbeats, the watchdog and reconnects (as in VEIN).
void ServiceTimer(lws_sorted_usec_list* sul) {
    lws_context* ctx = nullptr;
    auto* impl = reinterpret_cast<LwsTransport::Impl::Timer*>(sul)->owner;
    {
        std::lock_guard<std::mutex> g(impl->mu);
        ctx = impl->context;
    }
    if (ctx) lws_sul_schedule(ctx, 0, sul, ServiceTimer, 250 * LWS_US_PER_MS);
}

lws_protocols kProtocols[] = {
    {"takaro-conan-native", CallbackThunk, 0, 64 * 1024, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0},
};
}  // namespace

int LwsTransport::Impl::Callback(lws* w, lws_callback_reasons reason, void* in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED: {
            uint64_t e;
            {
                std::lock_guard<std::mutex> g(mu);
                wsi = w;
                open = true;
                timeoutSet = false;
                inbound.clear();
                connected = true;
                e = ++epoch;
                connects++;
                queues.Clear();
                hb.Reset(SteadyMs());
                lastError.clear();
            }
            if (!Notify(NoticeType::Open, e)) return -1;
            lws_callback_on_writable(w);
            return 0;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            uint64_t e;
            {
                std::lock_guard<std::mutex> g(mu);
                hb.OnInbound(SteadyMs());
                e = epoch;
            }
            if (inbound.size() + len > kMaxInboundBytes) {
                Error(e, "incoming message exceeds 1 MiB");
                return -1;
            }
            inbound.append(static_cast<const char*>(in), len);
            if (lws_is_final_fragment(w) && lws_remaining_packet_payload(w) == 0) {
                {
                    std::lock_guard<std::mutex> g(mu);
                    framesReceived++;
                }
                std::string text;
                text.swap(inbound);
                if (!Notify(NoticeType::Frame, e, std::move(text))) return -1;
            }
            return 0;
        }
        case LWS_CALLBACK_CLIENT_RECEIVE_PONG: {
            uint64_t e, confirmed = 0;
            std::string payload(in ? static_cast<const char*>(in) : "", in ? len : 0);
            bool digits = !payload.empty() && payload.size() < 20 &&
                          std::all_of(payload.begin(), payload.end(), [](char c) { return c >= '0' && c <= '9'; });
            {
                std::lock_guard<std::mutex> g(mu);
                hb.OnInbound(SteadyMs());
                e = epoch;
                if (digits) confirmed = hb.OnPongId(std::stoull(payload));
            }
            if (confirmed && !Notify(NoticeType::Confirmed, e, {}, confirmed)) return -1;
            return 0;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            if (!open) return 0;
            const int64_t now = SteadyMs();
            OutFrame out;
            bool havePing = false, haveFrame = false;
            uint64_t pingId = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                if (closeEpoch && closeEpoch == epoch) {
                    unsigned char reasonBuf[123]{};
                    size_t n = std::min(closeReason.size(), sizeof(reasonBuf));
                    memcpy(reasonBuf, closeReason.data(), n);
                    lws_close_reason(w, LWS_CLOSE_STATUS_NORMAL, reasonBuf, n);
                    closeEpoch = 0;
                    return -1;
                }
                if (hb.PingDue(now, queues.Frames(FrameKind::Event) == 0)) {
                    havePing = true;
                    pingId = hb.NextPingId();
                } else {
                    haveFrame = queues.Pop(out);
                }
            }
            if (havePing) {
                std::string id = std::to_string(pingId);
                std::vector<unsigned char> buf(LWS_PRE + id.size());
                memcpy(buf.data() + LWS_PRE, id.data(), id.size());
                if (lws_write(w, buf.data() + LWS_PRE, id.size(), LWS_WRITE_PING) < 0) return -1;
                std::lock_guard<std::mutex> g(mu);
                hb.OnPingSent(now);
                lws_callback_on_writable(w);
                return 0;
            }
            if (!haveFrame) return 0;
            std::vector<unsigned char> buf(LWS_PRE + out.text->size());
            memcpy(buf.data() + LWS_PRE, out.text->data(), out.text->size());
            if (lws_write(w, buf.data() + LWS_PRE, out.text->size(), LWS_WRITE_TEXT) != (int)out.text->size())
                return -1;
            {
                std::lock_guard<std::mutex> g(mu);
                framesSent++;
                bytesSent += out.text->size();
                if (out.kind == FrameKind::Event) hb.OnEventWritten(out.outboxId);
            }
            lws_callback_on_writable(w);
            return 0;
        }
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
            uint64_t e;
            {
                std::lock_guard<std::mutex> g(mu);
                e = epoch;
                connectFailures++;
            }
            Error(e, std::string("connection error: ") + (in ? static_cast<const char*>(in) : "unknown"));
        }
            [[fallthrough]];
        case LWS_CALLBACK_CLIENT_CLOSED: {
            if (wsi && w != wsi) return 0;
            bool wasOpen = open;
            uint64_t e;
            {
                std::lock_guard<std::mutex> g(mu);
                e = epoch;
                connected = false;
                closeEpoch = 0;
                queues.Clear();  // every queued frame belongs to the old connection; the bridge replays
            }
            wsi = nullptr;
            open = false;
            inbound.clear();
            if (wasOpen) Notify(NoticeType::Closed, e, "connection closed");
            return 0;
        }
        default:
            return 0;
    }
}

void LwsTransport::Impl::Run() {
    const std::string caPath = cfg.caFile.empty() ? "/etc/ssl/certs/ca-certificates.crt" : cfg.caFile;
    X509_STORE* trust = X509_STORE_new();
    if (!trust || X509_STORE_load_file(trust, caPath.c_str()) != 1) {
        if (trust) X509_STORE_free(trust);
        Error(0, "could not load the trusted CA file " + caPath);
        return;
    }
    X509_STORE_free(trust);

    std::string url = cfg.url;
    const char *protocol = nullptr, *address = nullptr, *path = nullptr;
    int port = 0;
    if (lws_parse_uri(&url[0], &protocol, &address, &port, &path) || !protocol || strcmp(protocol, "wss") != 0 ||
        !address || !*address || !path) {
        Error(0, "the Takaro URL must be a valid wss:// URL");
        return;
    }
    const std::string host = address;
    std::string uriPath = path;
    if (uriPath.empty() || uriPath[0] != '/') uriPath.insert(uriPath.begin(), '/');

    lws_set_log_level(LLL_ERR, nullptr);
    lws_context_creation_info info{};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = kProtocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.fd_limit_per_thread = 16;
    info.client_ssl_ca_filepath = caPath.c_str();
    info.user = this;
    lws_context* local = lws_create_context(&info);
    if (!local) {
        Error(0, "libwebsockets context creation failed");
        return;
    }
    {
        std::lock_guard<std::mutex> g(mu);
        context = local;
    }
    lws_sul_schedule(local, 0, &serviceTimer.sul, ServiceTimer, 250 * LWS_US_PER_MS);
    int64_t delay = cfg.reconnectBaseMs, nextConnect = 0;
    bool awaiting = false;
    while (!stopping) {
        int64_t now = SteadyMs();
        if (!wsi && now >= nextConnect) {
            lws_client_connect_info ci{};
            ci.context = local;
            ci.address = host.c_str();
            ci.port = port;
            ci.path = uriPath.c_str();
            ci.host = host.c_str();
            ci.origin = host.c_str();
            // Takaro's endpoint advertises no WS subprotocol: bind our callback without sending one.
            ci.local_protocol_name = kProtocols[0].name;
            ci.ssl_connection = LCCSCF_USE_SSL;  // SNI and hostname verification are lws defaults
            ci.pwsi = &wsi;
            lws* started = lws_client_connect_via_info(&ci);
            if (!started) {
                wsi = nullptr;
                Error(epoch, "Takaro connection initiation failed");
            }
            awaiting = true;
        }
        lws_service(local, 100);
        now = SteadyMs();
        if (awaiting && !wsi) {  // the attempt (or the connection) ended: back off
            {
                std::lock_guard<std::mutex> g(mu);
                if (identifiedEpoch && identifiedEpoch == epoch) delay = cfg.reconnectBaseMs;
                backoffMs = delay;
            }
            nextConnect = now + delay;
            delay = std::min<int64_t>(cfg.reconnectMaxMs, delay * 2);
            awaiting = false;
        }
        if (open && wsi) {
            bool wantWrite, dead;
            {
                std::lock_guard<std::mutex> g(mu);
                if (identifiedEpoch == epoch) delay = cfg.reconnectBaseMs;
                dead = hb.Dead(now) || hb.Stalled(now);
                wantWrite = closeEpoch == epoch || queues.TotalFrames() > 0 ||
                            hb.PingDue(now, queues.Frames(FrameKind::Event) == 0);
            }
            if (dead && !timeoutSet) {
                // Set on the service thread: it closes even when the socket never becomes writable again.
                Error(epoch, "heartbeat timeout: no frame or pong from Takaro");
                lws_set_timeout(wsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
                timeoutSet = true;
            }
            if (wantWrite) lws_callback_on_writable(wsi);
        }
    }
    lws_sul_cancel(&serviceTimer.sul);
    {
        std::lock_guard<std::mutex> g(mu);
        context = nullptr;
        connected = false;
        queues.Clear();
    }
    lws_context_destroy(local);
    wsi = nullptr;
    open = false;
}

LwsTransport::LwsTransport(LwsConfig cfg) : impl_(new Impl) {
    impl_->cfg = std::move(cfg);
    impl_->hb = Heartbeat(impl_->cfg.heartbeat);
}

LwsTransport::~LwsTransport() { Stop(); }

bool LwsTransport::Start(NoticeSink sink) {
    if (impl_->worker.joinable()) return false;
    impl_->sink = std::move(sink);
    impl_->stopping = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
    return true;
}

void LwsTransport::Stop() {
    impl_->stopping = true;
    {
        std::lock_guard<std::mutex> g(impl_->mu);
        if (impl_->context) lws_cancel_service(impl_->context);
    }
    if (impl_->worker.joinable()) impl_->worker.join();
}

QueueStatus LwsTransport::Queue(OutFrame frame) {
    std::lock_guard<std::mutex> g(impl_->mu);
    if (impl_->stopping || !impl_->connected) return QueueStatus::Disconnected;
    if (frame.epoch != impl_->epoch) return QueueStatus::StaleEpoch;
    QueueStatus st = impl_->queues.Push(std::move(frame));
    if (st == QueueStatus::Accepted && impl_->context) lws_cancel_service(impl_->context);
    return st;
}

void LwsTransport::RequestClose(uint64_t epoch, const std::string& reason) {
    std::lock_guard<std::mutex> g(impl_->mu);
    if (!impl_->connected || epoch != impl_->epoch) return;
    impl_->closeEpoch = epoch;
    impl_->closeReason = reason.substr(0, 123);
    if (impl_->context) lws_cancel_service(impl_->context);
}

void LwsTransport::MarkIdentified(uint64_t epoch) {
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->identifiedEpoch = epoch;
}

std::string LwsTransport::StatsJson() {
    std::lock_guard<std::mutex> g(impl_->mu);
    Impl& i = *impl_;
    return ObjBuilder()
        .S("kind", "libwebsockets")
        .B("connected", i.connected)
        .Raw("epoch", std::to_string(i.epoch))
        .N("connects", (double)i.connects)
        .N("connectFailures", (double)i.connectFailures)
        .N("framesSent", (double)i.framesSent)
        .N("bytesSent", (double)i.bytesSent)
        .N("framesReceived", (double)i.framesReceived)
        .N("queuedFrames", (double)i.queues.TotalFrames())
        .N("queuedBytes", (double)i.queues.TotalBytes())
        .N("rejected", (double)i.queues.Rejected())
        .N("pingsSent", (double)i.hb.PingsSent())
        .N("pongs", (double)i.hb.PongsReceived())
        .N("outstandingPings", (double)i.hb.Outstanding())
        .Raw("eventsWritten", std::to_string(i.hb.Written()))
        .Raw("eventsConfirmed", std::to_string(i.hb.Confirmed()))
        .N("backoffMs", (double)i.backoffMs)
        .S("lastError", i.lastError)
        .Done();
}

}  // namespace takaro
