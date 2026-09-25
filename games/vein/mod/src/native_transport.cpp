#include "native_transport.h"

#include <libwebsockets.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace NativeTransport {
namespace {
using Clock = std::chrono::steady_clock;
struct Outbound { Kind kind; Frame frame; uint64_t outboxId; uint64_t writeId; bool pingAfterWrite; };
struct PendingPing { uint64_t id; uint64_t upToOutbox; uint64_t upToWrite; };
std::mutex mu;
Config cfg;
NoticeSink sink;
std::thread worker;
std::atomic<bool> stopping{false};
lws_context* context = nullptr; // read by Queue only while mu is held
std::deque<Outbound> controls, criticalResponses, responses, events;
size_t controlBytes = 0, criticalBytes = 0, responseBytes = 0, eventBytes = 0;
uint64_t rejected = 0, epoch = 0, nextWriteId = 0;
uint64_t prunedEvents = 0, minDeliverableOutboxId = 0;
std::atomic<size_t> transientBytes{0};
struct CloseRequest { uint64_t epoch; uint16_t code; std::string reason; };
std::unique_ptr<CloseRequest> closeRequest;
bool connected = false;
std::string lastError;
struct HttpRequest {
    std::mutex mutex;
    std::condition_variable cv;
    Clock::time_point deadline;
    std::string body;
    bool done = false;
    bool canceled = false;
    bool ok = false;
};
std::shared_ptr<HttpRequest> httpMailbox; // protected by mu
std::shared_ptr<HttpRequest> httpActive;  // LWS service thread only
lws* httpWsi = nullptr;                   // LWS service thread only
std::string httpHost, httpPath;
int httpPort = 0;
bool httpTls = false;
std::atomic<bool> httpEnabled{false};
bool httpTimeoutSet = false;

void FinishHttp(bool ok) {
    auto request = std::move(httpActive);
    httpWsi = nullptr;
    if (!request) return;
    {
        std::lock_guard<std::mutex> g(request->mutex);
        request->ok = ok && !request->canceled;
        request->done = true;
        if (!request->ok) request->body.clear();
    }
    request->cv.notify_one();
    std::lock_guard<std::mutex> g(mu);
    if (httpMailbox == request) httpMailbox.reset();
}

int HttpCallback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    try {
        // A completed HTTP client may close after a new request is queued.
        // Never let that older socket finish the new request.
        if (!httpActive || user != httpActive.get() || (httpWsi && wsi != httpWsi)) return 0;
        switch (reason) {
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
            if (httpActive && lws_http_client_http_response(wsi) != 200) return -1;
            return 0;
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP: {
            char buffer[LWS_PRE + 4096];
            char* body = buffer + LWS_PRE;
            int size = sizeof(buffer) - LWS_PRE;
            return lws_http_client_read(wsi, &body, &size) < 0 ? -1 : 0;
        }
        case LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ:
            if (!httpActive || !in || len > 1024*1024 - httpActive->body.size()) return -1;
            httpActive->body.append(static_cast<const char*>(in), len);
            return 0;
        case LWS_CALLBACK_COMPLETED_CLIENT_HTTP:
            FinishHttp(true);
            return 0;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
            FinishHttp(false);
            return 0;
        default: return 0;
        }
    } catch (...) {
        FinishHttp(false);
        return -1;
    }
}

bool Notify(NoticeType type, uint64_t atEpoch, uint64_t outboxId = 0, std::string text = {},
            uint64_t writeId = 0, bool pingAfterWrite = false) {
    // The bridge sink is a bounded, nonblocking queue insertion.
    return !sink || sink({type, atEpoch, outboxId, std::move(text), {atEpoch, writeId}, outboxId,
                          pingAfterWrite});
}

struct Session {
    lws* wsi = nullptr; // service thread only
    std::string inbound;
    Clock::time_point lastActivity = Clock::now();
    Clock::time_point lastPing = Clock::now();
    std::deque<PendingPing> pings;
    uint64_t nextPing = 0;
    uint64_t written = 0;
    uint64_t writtenWriteId = 0;
    uint64_t id = 0;
    bool open = false;
    bool timeoutSet = false;
    bool immediatePing = false;
};
Session session;
lws_sorted_usec_list serviceTimer;
void ServiceTimer(lws_sorted_usec_list* sul) {
    lws_sul_schedule(context, 0, sul, ServiceTimer, LWS_US_PER_SEC);
}

bool Pop(Outbound& out) {
    std::lock_guard<std::mutex> g(mu);
    std::deque<Outbound>* q = nullptr;
    size_t* bytes = nullptr;
    if (!controls.empty()) { q = &controls; bytes = &controlBytes; }
    else if (!criticalResponses.empty()) { q = &criticalResponses; bytes = &criticalBytes; }
    else if (!responses.empty()) { q = &responses; bytes = &responseBytes; }
    else if (!events.empty()) { q = &events; bytes = &eventBytes; }
    if (!q) return false;
    out = std::move(q->front());
    *bytes -= out.frame->size();
    q->pop_front();
    return true;
}

int Callback(lws* wsi, lws_callback_reasons reason, void* user, void* in, size_t len) {
    try {
    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED: {
        session.wsi = wsi;
        session.open = true;
        session.inbound.clear();
        session.pings.clear();
        session.nextPing = session.written = session.writtenWriteId = 0;
        session.lastActivity = session.lastPing = Clock::now();
        {
            std::lock_guard<std::mutex> g(mu);
            connected = true;
            session.id = ++epoch;
            minDeliverableOutboxId = 0;
            lastError.clear();
        }
        if (!Notify(NoticeType::Open, session.id)) return -1;
        lws_callback_on_writable(wsi);
        return 0;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE: {
        session.lastActivity = Clock::now();
        if (session.inbound.size() + len > 1024 * 1024) {
            Notify(NoticeType::Error, session.id, 0, "incoming message exceeds 1 MiB");
            return -1;
        }
        session.inbound.append(static_cast<const char*>(in), len);
        if (lws_is_final_fragment(wsi) && lws_remaining_packet_payload(wsi) == 0) {
            if (!Notify(NoticeType::Frame, session.id, 0, std::move(session.inbound))) return -1;
            session.inbound.clear();
        }
        return 0;
    }
    case LWS_CALLBACK_CLIENT_RECEIVE_PONG: {
        session.lastActivity = Clock::now();
        if (session.pings.empty()) return 0;
        if (!in || !len) return 0;
        std::string payload(static_cast<const char*>(in), len);
        if (!std::all_of(payload.begin(), payload.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
        uint64_t received = 0;
        try { received = std::stoull(payload); } catch (...) { return 0; }
        auto it = std::find_if(session.pings.begin(), session.pings.end(),
                               [received](const PendingPing& p) { return p.id == received; });
        if (it == session.pings.end()) return 0;
        uint64_t confirmed = it->upToOutbox;
        uint64_t confirmedWrite = it->upToWrite;
        session.pings.erase(session.pings.begin(), std::next(it));
        if (!Notify(NoticeType::Confirmed, session.id, confirmed, {}, confirmedWrite)) return -1;
        return 0;
    }
    case LWS_CALLBACK_CLIENT_WRITEABLE: {
        if (!session.open) return 0;
        {
            std::lock_guard<std::mutex> g(mu);
            if (closeRequest && closeRequest->epoch == session.id) {
                unsigned char reason[124]{};
                const size_t n = std::min(closeRequest->reason.size(), sizeof(reason));
                memcpy(reason, closeRequest->reason.data(), n);
                lws_close_reason(wsi, static_cast<lws_close_status>(closeRequest->code), reason, n);
                closeRequest.reset();
                return -1;
            }
        }
        const auto now = Clock::now();
        if (session.immediatePing || now - session.lastPing >= std::chrono::seconds(5)) {
            if (session.pings.size() >= 2 || now - session.lastActivity > std::chrono::seconds(20)) {
                Notify(NoticeType::Error, session.id, 0, "heartbeat timeout");
                return -1;
            }
            std::string id = std::to_string(++session.nextPing);
            std::vector<unsigned char> buf(LWS_PRE + id.size());
            memcpy(buf.data() + LWS_PRE, id.data(), id.size());
            // LWS reports payload length for TEXT, but PING may report the
            // full encoded control frame length (header and mask included).
            if (lws_write(wsi, buf.data() + LWS_PRE, id.size(), LWS_WRITE_PING) < 0) return -1;
            session.pings.push_back({session.nextPing, session.written, session.writtenWriteId});
            session.lastPing = now;
            session.immediatePing = false;
            return 0;
        }
        Outbound out;
        if (!Pop(out)) return 0;
        if (out.kind == Kind::Event) {
            std::lock_guard<std::mutex> g(mu);
            if (out.outboxId < minDeliverableOutboxId) {
                ++prunedEvents;
                lws_callback_on_writable(wsi);
                return 0;
            }
        }
        transientBytes = out.frame->size();
        std::vector<unsigned char> buf(LWS_PRE + out.frame->size());
        memcpy(buf.data() + LWS_PRE, out.frame->data(), out.frame->size());
        if (lws_write(wsi, buf.data() + LWS_PRE, out.frame->size(), LWS_WRITE_TEXT) != static_cast<int>(out.frame->size())) {
            transientBytes = 0;
            return -1;
        }
        transientBytes = 0;
        if (out.kind == Kind::Event) session.written = std::max(session.written, out.outboxId);
        session.writtenWriteId = out.writeId;
        if (!Notify(NoticeType::Written, session.id, session.written, {}, out.writeId,
                    out.pingAfterWrite)) return -1;
        if (out.pingAfterWrite) session.immediatePing = true;
        lws_callback_on_writable(wsi);
        return 0;
    }
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        // Pre-protocol HTTP connection failures can arrive on protocol[0].
        // They must not reset the independent Takaro WebSocket session.
        if (session.wsi && wsi != session.wsi) {
            if (httpActive && (user == httpActive.get() || wsi == httpWsi)) FinishHttp(false);
            return 0;
        }
        if (in) Notify(NoticeType::Error, session.id, 0, static_cast<const char*>(in));
        [[fallthrough]];
    case LWS_CALLBACK_CLIENT_CLOSED: {
        if (session.wsi && wsi != session.wsi) return 0;
        if (session.open) Notify(NoticeType::Closed, session.id);
        session = Session{};
        std::lock_guard<std::mutex> g(mu);
        connected = false;
        // Every queued outbound frame belongs to the old connection. The bridge
        // retains event ownership and replays after the next identify.
        controls.clear(); criticalResponses.clear(); responses.clear(); events.clear();
        controlBytes = criticalBytes = responseBytes = eventBytes = 0;
        closeRequest.reset();
        return 0;
    }
    default: return 0;
    }
    } catch (...) {
        // A C callback must never unwind into libwebsockets or the game process.
        transientBytes = 0;
        return -1;
    }
}

lws_protocols protocols[] = {
    {"takaro-native", Callback, 0, 1024 * 1024, 0, nullptr, 0},
    {"vein-game-http", HttpCallback, 0, 4096, 0, nullptr, 0},
    {}
};

void ServiceGameHttp(lws_context* local) {
    if (!httpActive) {
        std::shared_ptr<HttpRequest> request;
        { std::lock_guard<std::mutex> g(mu); request = httpMailbox; }
        if (!request) return;
        httpActive = std::move(request);
        httpTimeoutSet = false;
        bool canceled;
        { std::lock_guard<std::mutex> g(httpActive->mutex); canceled = httpActive->canceled; }
        if (canceled || Clock::now() >= httpActive->deadline) {
            FinishHttp(false);
            return;
        }
        lws_client_connect_info ci{};
        ci.context = local;
        ci.address = httpHost.c_str(); ci.port = httpPort;
        ci.path = httpPath.c_str(); ci.host = httpHost.c_str();
        ci.method = "GET";
        ci.local_protocol_name = protocols[1].name;
        ci.userdata = httpActive.get();
        ci.ssl_connection = httpTls ? LCCSCF_USE_SSL : 0;
        ci.pwsi = &httpWsi;
        lws* started = lws_client_connect_via_info(&ci);
        if (httpActive) {
            httpWsi = started;
            if (!httpWsi) FinishHttp(false);
        }
    }
    if (httpActive && httpWsi && !httpTimeoutSet) {
        bool canceled;
        { std::lock_guard<std::mutex> g(httpActive->mutex); canceled = httpActive->canceled; }
        if (canceled || Clock::now() >= httpActive->deadline) {
            lws_set_timeout(httpWsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
            httpTimeoutSet = true;
        }
    }
}

void Run() {
    const std::string caPath = cfg.caFile.empty() ? "/etc/ssl/certs/ca-certificates.crt" : cfg.caFile;
    X509_STORE* trust = X509_STORE_new();
    if (!trust || X509_STORE_load_file(trust, caPath.c_str()) != 1) {
        if (trust) X509_STORE_free(trust);
        Notify(NoticeType::Error, 0, 0, "could not load trusted CA file: " + caPath);
        return;
    }
    X509_STORE_free(trust);
    lws_context_creation_info info{};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.fd_limit_per_thread = 16;
    info.client_ssl_ca_filepath = caPath.c_str();
    lws_context* local = lws_create_context(&info);
    if (!local) { Notify(NoticeType::Error, 0, 0, "libwebsockets context creation failed"); return; }
    { std::lock_guard<std::mutex> g(mu); context = local; }
    lws_sul_schedule(local, 0, &serviceTimer, ServiceTimer, LWS_US_PER_SEC);
    std::string url = cfg.url;
    const char *protocol = nullptr, *address = nullptr, *path = nullptr;
    int port = 0;
    if (lws_parse_uri(url.data(), &protocol, &address, &port, &path) || !protocol ||
        strcmp(protocol, "wss") != 0 || !address || !path) {
        Notify(NoticeType::Error, 0, 0, "TAKARO_WS_URL must be a valid wss:// URL");
        stopping = true;
    }
    std::string host = address ? address : "", uriPath = path ? path : "";
    if (uriPath.empty() || uriPath[0] != '/') uriPath.insert(uriPath.begin(), '/');
    httpEnabled = false;
    if (!cfg.gameHttpUrl.empty() && cfg.gameHttpUrl.find('@') == std::string::npos &&
        cfg.gameHttpUrl.find('?') == std::string::npos && cfg.gameHttpUrl.find('#') == std::string::npos) {
        std::string gameUrl = cfg.gameHttpUrl;
        const char *scheme = nullptr, *gameHost = nullptr, *gamePath = nullptr;
        int gamePort = 0;
        if (!lws_parse_uri(gameUrl.data(), &scheme, &gameHost, &gamePort, &gamePath) &&
            scheme && (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0) &&
            gameHost && *gameHost && gamePort > 0 && gamePath) {
            httpTls = strcmp(scheme, "https") == 0;
            httpHost = gameHost;
            httpPort = gamePort;
            httpPath = gamePath;
            if (httpPath.empty() || httpPath[0] != '/') httpPath.insert(httpPath.begin(), '/');
            while (httpPath.size() > 1 && httpPath.back() == '/') httpPath.pop_back();
            if (httpPath == "/") httpPath.clear();
            httpPath += "/players";
            httpEnabled = true;
        }
    }
    unsigned delay = cfg.reconnectBaseMs;
    auto nextConnect = Clock::now();
    bool awaiting = false;
    while (!stopping) {
        ServiceGameHttp(local);
        if (!session.wsi && Clock::now() >= nextConnect) {
            lws_client_connect_info ci{};
            ci.context = local;
            ci.address = host.c_str(); ci.port = port;
            ci.path = uriPath.c_str(); ci.host = host.c_str(); ci.origin = host.c_str();
            // Takaro's endpoint does not advertise a WS subprotocol. Bind our
            // local callback without adding Sec-WebSocket-Protocol to the wire.
            ci.local_protocol_name = protocols[0].name;
            ci.ssl_connection = LCCSCF_USE_SSL; // SNI and hostname verification are LWS defaults.
            session.wsi = lws_client_connect_via_info(&ci);
            if (!session.wsi) Notify(NoticeType::Error, 0, 0, "Takaro connection initiation failed");
            awaiting = true;
        }
        lws_service(local, 100);
        if (awaiting && !session.wsi) {
            nextConnect = Clock::now() + std::chrono::milliseconds(delay);
            delay = std::min(cfg.reconnectMaxMs, delay * 2);
            awaiting = false;
        }
        if (session.open) {
            delay = cfg.reconnectBaseMs;
            const auto now = Clock::now();
            if (!session.timeoutSet &&
                ((session.pings.size() >= 2 && now - session.lastPing >= std::chrono::seconds(5)) ||
                 now - session.lastActivity > std::chrono::seconds(20))) {
                // Timeout is set on the LWS service thread. It closes even when
                // the socket never becomes writable again.
                lws_set_timeout(session.wsi, PENDING_TIMEOUT_CLOSE_SEND, 1);
                session.timeoutSet = true;
            }
            if (session.immediatePing || Clock::now() - session.lastPing >= std::chrono::seconds(5))
                lws_callback_on_writable(session.wsi);
            std::lock_guard<std::mutex> g(mu);
            if (closeRequest || !controls.empty() || !criticalResponses.empty() || !responses.empty() || !events.empty())
                lws_callback_on_writable(session.wsi);
        }
    }
    if (httpActive) FinishHttp(false);
    else {
        std::shared_ptr<HttpRequest> pending;
        { std::lock_guard<std::mutex> g(mu); pending = std::move(httpMailbox); }
        if (pending) {
            { std::lock_guard<std::mutex> g(pending->mutex); pending->done = true; }
            pending->cv.notify_one();
        }
    }
    httpEnabled = false;
    httpHost.clear(); httpPath.clear(); httpPort = 0; httpWsi = nullptr;
    { std::lock_guard<std::mutex> g(mu); context = nullptr; connected = false; }
    lws_sul_cancel(&serviceTimer);
    lws_context_destroy(local);
    session = Session{};
    transientBytes = 0;
}
} // namespace

bool Start(Config config, NoticeSink noticeSink) {
    if (worker.joinable()) return false;
    static bool exitStopRegistered = false;
    if (!exitStopRegistered) { std::atexit([] { Stop(); }); exitStopRegistered = true; }
    cfg = std::move(config); sink = std::move(noticeSink); stopping = false;
    worker = std::thread(Run);
    return true;
}
void Stop() {
    stopping = true;
    { std::lock_guard<std::mutex> g(mu); if (context) lws_cancel_service(context); }
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> g(mu);
    controls.clear(); criticalResponses.clear(); responses.clear(); events.clear();
    controlBytes = criticalBytes = responseBytes = eventBytes = 0;
    closeRequest.reset();
    connected = false;
    transientBytes = 0;
}
std::optional<std::string> FetchGamePlayers() {
    auto request = std::make_shared<HttpRequest>();
    request->deadline = Clock::now() + std::chrono::seconds(3);
    {
        std::lock_guard<std::mutex> g(mu);
        if (stopping || !context || !httpEnabled || httpMailbox) return std::nullopt;
        httpMailbox = request;
        lws_cancel_service(context);
    }
    std::unique_lock<std::mutex> lock(request->mutex);
    if (!request->cv.wait_until(lock, request->deadline, [&] { return request->done; })) {
        request->canceled = true;
        lock.unlock();
        std::lock_guard<std::mutex> g(mu);
        if (context) lws_cancel_service(context);
        return std::nullopt;
    }
    if (!request->ok) return std::nullopt;
    return std::move(request->body);
}
size_t PruneEventsBefore(uint64_t minOutboxId, uint64_t expectedEpoch) {
    std::lock_guard<std::mutex> g(mu);
    if (expectedEpoch != epoch) return 0;
    minDeliverableOutboxId = std::max(minDeliverableOutboxId, minOutboxId);
    size_t removed = 0;
    for (auto it = events.begin(); it != events.end();) {
        if (it->outboxId < minDeliverableOutboxId) {
            eventBytes -= it->frame->size(); it = events.erase(it); ++removed;
        } else ++it;
    }
    prunedEvents += removed;
    return removed;
}
QueueResult Queue(Send send) {
    std::lock_guard<std::mutex> g(mu);
    if (stopping || !connected) return {QueueStatus::Disconnected, {}};
    if (send.expectedEpoch && send.expectedEpoch != epoch) return {QueueStatus::StaleEpoch, {}};
    if (!send.frame) return {QueueStatus::TooLarge, {}};
    if (send.kind == Kind::Event && send.outboxId < minDeliverableOutboxId)
        return {QueueStatus::StaleEpoch, {}};
    const size_t n = send.frame->size();
    std::deque<Outbound>* q = nullptr;
    size_t* bytes = nullptr;
    size_t maxCount = 0, maxBytes = 0;
    switch (send.kind) {
    case Kind::Control: q=&controls; bytes=&controlBytes; maxCount=128; maxBytes=1024*1024; break;
    case Kind::CriticalResponse: q=&criticalResponses; bytes=&criticalBytes; maxCount=128; maxBytes=1024*1024; break;
    // The bridge reserves 16 MiB of the aggregate 32 MiB response budget.
    // Normal and critical responses share this transport half.
    case Kind::Response: q=&responses; bytes=&responseBytes; maxCount=128; maxBytes=16*1024*1024; break;
    // The bridge's durable outbox owns the same immutable frame reference.
    // This logical byte tally limits staging, but the payload is not copied.
    case Kind::Event: q=&events; bytes=&eventBytes; maxCount=5000; maxBytes=32*1024*1024; break;
    }
    if ((send.kind == Kind::Response && n > 8*1024*1024) || n > maxBytes) {
        ++rejected; return {QueueStatus::TooLarge, {}};
    }
    if (q->size() >= maxCount || *bytes + n > maxBytes ||
        ((send.kind == Kind::Response || send.kind == Kind::CriticalResponse) &&
         responseBytes + criticalBytes + n > 16*1024*1024)) {
        ++rejected; return {QueueStatus::Full, {}};
    }
    Ticket ticket{epoch, ++nextWriteId};
    *bytes += n;
    q->push_back({send.kind, std::move(send.frame), send.outboxId, ticket.writeId, send.pingAfterWrite});
    if (context) lws_cancel_service(context);
    return {QueueStatus::Accepted, ticket};
}
bool Queue(Kind kind, std::string value, uint64_t eventSeq, uint64_t expectedEpoch) {
    return static_cast<bool>(Queue({kind, std::make_shared<const std::string>(std::move(value)),
                                    expectedEpoch, eventSeq, false}));
}
bool RequestClose(uint64_t expectedEpoch, uint16_t code, std::string reason) {
    std::lock_guard<std::mutex> g(mu);
    if (stopping || !connected || expectedEpoch != epoch) return false;
    if (reason.size() > 123) reason.resize(123);
    closeRequest = std::make_unique<CloseRequest>(CloseRequest{expectedEpoch, code, std::move(reason)});
    if (context) lws_cancel_service(context);
    return true;
}
Stats Snapshot() {
    std::lock_guard<std::mutex> g(mu);
    return {connected, epoch, controls.size()+criticalResponses.size()+responses.size()+events.size(),
            controlBytes+criticalBytes+responseBytes+eventBytes, transientBytes.load(),
            rejected, prunedEvents, lastError};
}
} // namespace NativeTransport
