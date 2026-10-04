#include "transport_winhttp.h"

#include "common.h"
#include "takaro/json_util.h"

#define WIN32_LEAN_AND_MEAN
#include "abortive_close.h"  // winsock2.h, which must come before windows.h
#include <windows.h>

#include <winhttp.h>
#include <wincrypt.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace takaro {

namespace {

int64_t Now() { return (int64_t)GetTickCount64(); }

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Utf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string ErrName(DWORD e) {
    switch (e) {
        case 12002: return "timeout";
        case 12007: return "name not resolved";
        case 12017: return "operation cancelled";
        case 12029: return "cannot connect";
        case 12030: return "connection error";
        case 12038: return "certificate name mismatch";
        case 12045: return "certificate not trusted by caFile";
        case 12152: return "invalid server response";
        case 12157: return "TLS/certificate rejected";
        case 12175: return "TLS failure";
        case 4317: return "invalid operation";
        default: return "";
    }
}

struct Url {
    std::wstring host, path = L"/";
    INTERNET_PORT port = 443;
};

bool ParseUrl(const std::string& u, Url& out) {
    if (u.compare(0, 6, "wss://") != 0) return false;
    std::string rest = u.substr(6);
    size_t sl = rest.find('/');
    std::string hp = sl == std::string::npos ? rest : rest.substr(0, sl);
    if (sl != std::string::npos) out.path = Wide(rest.substr(sl));
    size_t co = hp.rfind(':');
    if (co != std::string::npos) {
        out.port = (INTERNET_PORT)atoi(hp.c_str() + co + 1);
        hp = hp.substr(0, co);
    }
    out.host = Wide(hp);
    return !hp.empty() && out.port != 0;
}

// PEM bundle -> in-memory store. Empty store on any failure (the caller fails closed).
HCERTSTORE LoadCaStore(const std::string& path, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open caFile '" + path + "'";
        return nullptr;
    }
    std::string pem;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) pem.append(buf, n);
    fclose(f);
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (!store) {
        err = "CertOpenStore failed";
        return nullptr;
    }
    int count = 0;
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    for (size_t pos = 0; (pos = pem.find(begin, pos)) != std::string::npos;) {
        size_t e = pem.find(end, pos);
        if (e == std::string::npos) break;
        std::string block = pem.substr(pos, e + end.size() - pos);
        pos = e + end.size();
        DWORD der = 0;
        if (!CryptStringToBinaryA(block.c_str(), (DWORD)block.size(), CRYPT_STRING_BASE64HEADER, nullptr, &der, nullptr,
                                  nullptr))
            continue;
        std::vector<BYTE> bin(der);
        if (!CryptStringToBinaryA(block.c_str(), (DWORD)block.size(), CRYPT_STRING_BASE64HEADER, bin.data(), &der,
                                  nullptr, nullptr))
            continue;
        if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, bin.data(), der, CERT_STORE_ADD_ALWAYS, nullptr))
            count++;
    }
    if (!count) {
        CertCloseStore(store, 0);
        err = "caFile '" + path + "' holds no PEM certificate";
        return nullptr;
    }
    return store;
}

std::string CertName(PCCERT_CONTEXT c, DWORD flags) {
    wchar_t b[256] = {0};
    CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, b, 256);
    return Utf8(b);
}

// The chain of the server certificate must end in a CA from caFile and pass the SSL policy for `host`.
bool VerifyAgainstCa(HINTERNET req, HCERTSTORE ca, const std::wstring& host, std::string& detail, std::string& subject,
                     std::string& issuer) {
    PCCERT_CONTEXT srv = nullptr;
    DWORD len = sizeof srv;
    if (!WinHttpQueryOption(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &srv, &len) || !srv) {
        detail = "no server certificate";
        return false;
    }
    subject = CertName(srv, 0);
    issuer = CertName(srv, CERT_NAME_ISSUER_FLAG);
    CERT_CHAIN_PARA cp = {};
    cp.cbSize = sizeof cp;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    bool ok = false;
    if (!CertGetCertificateChain(nullptr, srv, nullptr, ca, &cp, 0, nullptr, &chain) || !chain) {
        detail = "chain build failed (" + std::to_string(GetLastError()) + ")";
    } else {
        const CERT_SIMPLE_CHAIN* sc = chain->rgpChain[0];
        PCCERT_CONTEXT root = sc->rgpElement[sc->cElement - 1]->pCertContext;
        bool pinned = false;
        PCCERT_CONTEXT it = nullptr;
        while ((it = CertEnumCertificatesInStore(ca, it)) != nullptr)
            if (CertCompareCertificate(X509_ASN_ENCODING, it->pCertInfo, root->pCertInfo)) pinned = true;
        DWORD errs = chain->TrustStatus.dwErrorStatus;
        const DWORD tolerated =
            CERT_TRUST_IS_UNTRUSTED_ROOT | CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION;
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl = {};
        ssl.cbSize = sizeof ssl;
        ssl.dwAuthType = AUTHTYPE_SERVER;
        ssl.pwszServerName = (wchar_t*)host.c_str();
        CERT_CHAIN_POLICY_PARA pp = {};
        pp.cbSize = sizeof pp;
        pp.dwFlags = CERT_CHAIN_POLICY_ALLOW_UNKNOWN_CA_FLAG;
        pp.pvExtraPolicyPara = &ssl;
        CERT_CHAIN_POLICY_STATUS ps = {};
        ps.cbSize = sizeof ps;
        BOOL called = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps);
        ok = pinned && (errs & ~tolerated) == 0 && called && ps.dwError == 0;
        char b[160];
        snprintf(b, sizeof b, "rootPinned=%d trustErrors=0x%08lx policyError=0x%08lx", pinned ? 1 : 0,
                 (unsigned long)errs, (unsigned long)ps.dwError);
        detail = b;
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(srv);
    return ok;
}

const char kPing[] = "{\"type\":\"ping\"}";

}  // namespace

// Per-connection context. It is the WinHTTP callback context for the request and WebSocket handles, so
// it must outlive their HANDLE_CLOSING callbacks; when that cannot be proven it is leaked, never freed.
struct WinHttpTransport::Epoch {
    WinHttpTransport* owner = nullptr;
    uint64_t id = 0;
    HINTERNET req = nullptr, ws = nullptr;
    HANDLE evReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE evSend = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE evRecv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE evClose = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE evReqClosing = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE evWsClosing = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    volatile DWORD reqErr = 0, sendErr = 0, recvErr = 0, recvBytes = 0, closeErr = 0, secureFailure = 0;
    volatile WINHTTP_WEB_SOCKET_BUFFER_TYPE recvType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
    bool dead = false;  // under owner->mu_
    bool serverClosed = false;
    std::string why;
    TcpEndpoints tcp;  // the connection's addresses, to find its socket for an abortive close
    ~Epoch() {
        for (HANDLE h : {evReq, evSend, evRecv, evClose, evReqClosing, evWsClosing})
            if (h) CloseHandle(h);
    }
};

static void CALLBACK StatusCallback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD) {
    auto* e = (WinHttpTransport::Epoch*)ctx;
    if (!e) return;
    bool onWs = e->ws && h == e->ws;
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            e->reqErr = 0;
            SetEvent(e->evReq);
            break;
        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
            e->secureFailure = info ? *(DWORD*)info : 1;
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (onWs) {
                auto* st = (WINHTTP_WEB_SOCKET_STATUS*)info;
                e->recvBytes = st->dwBytesTransferred;
                e->recvType = st->eBufferType;
                e->recvErr = 0;
                SetEvent(e->evRecv);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            if (onWs) {
                e->sendErr = 0;
                SetEvent(e->evSend);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
            e->closeErr = 0;
            SetEvent(e->evClose);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (onWs) {
                auto* r = (WINHTTP_WEB_SOCKET_ASYNC_RESULT*)info;
                DWORD err = r->AsyncResult.dwError ? r->AsyncResult.dwError : 1;
                if (r->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) {
                    e->recvErr = err;
                    SetEvent(e->evRecv);
                } else if (r->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) {
                    e->sendErr = err;
                    SetEvent(e->evSend);
                } else {
                    e->closeErr = err;
                    SetEvent(e->evClose);
                }
            } else {
                auto* r = (WINHTTP_ASYNC_RESULT*)info;
                e->reqErr = r->dwError ? r->dwError : 1;
                SetEvent(e->evReq);
            }
            break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            if (onWs) SetEvent(e->evWsClosing);
            else if (h == e->req) SetEvent(e->evReqClosing);
            break;
    }
}

WinHttpTransport::WinHttpTransport(WinHttpConfig o) : o_(std::move(o)), hb_(o_.heartbeat) {}

WinHttpTransport::~WinHttpTransport() { Stop(); }

bool WinHttpTransport::Start(NoticeSink sink) {
    if (supervisor_.joinable()) return false;
    sink_ = std::move(sink);
    stopping_ = false;
    supervisor_ = std::thread(&WinHttpTransport::Supervisor, this);
    return true;
}

void WinHttpTransport::Stop() {
    if (!supervisor_.joinable()) return;
    {
        std::lock_guard<std::mutex> g(mu_);
        stopping_ = true;
        if (cur_ && !cur_->dead) {
            cur_->dead = true;
            cur_->why = "stopping";
        }
    }
    supCv_.notify_all();
    sendCv_.notify_all();
    supervisor_.join();
}

QueueStatus WinHttpTransport::Queue(OutFrame frame) {
    QueueStatus st;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!open_ || !cur_ || cur_->dead) return QueueStatus::Disconnected;
        if (frame.epoch != cur_->id) return QueueStatus::StaleEpoch;
        st = queues_.Push(std::move(frame));
    }
    if (st == QueueStatus::Accepted) sendCv_.notify_one();
    return st;
}

void WinHttpTransport::RequestClose(uint64_t epoch, const std::string& reason) {
    Epoch* e = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        if (cur_ && cur_->id == epoch) e = cur_;
    }
    if (e) MarkDead(e, "closed by connector: " + reason);
}

void WinHttpTransport::MarkIdentified(uint64_t epoch) {
    std::lock_guard<std::mutex> g(mu_);
    if (cur_ && cur_->id == epoch) attempts_ = 0;
}

void WinHttpTransport::MarkDead(Epoch* e, const std::string& why) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!e->dead) {
            e->dead = true;
            e->why = why;
        }
    }
    sendCv_.notify_all();
    supCv_.notify_all();
}

std::string WinHttpTransport::StatsJson() {
    std::lock_guard<std::mutex> g(mu_);
    int64_t now = Now();
    return ObjBuilder()
        .B("connected", open_)
        .Raw("epoch", std::to_string(epoch_))
        .N("connects", (double)connects_)
        .N("connectFailures", (double)connectFailures_)
        .N("backoffAttempts", attempts_)
        .N("reconnectInMs", !open_ && nextAttemptAtMs_ > now ? (double)(nextAttemptAtMs_ - now) : 0)
        .N("connectedForMs", open_ ? (double)(now - openedAtMs_) : 0)
        .Raw("queued", ObjBuilder()
                           .N("control", (double)queues_.Frames(FrameKind::Control))
                           .N("critical", (double)queues_.Frames(FrameKind::CriticalResponse))
                           .N("responses", (double)queues_.Frames(FrameKind::Response))
                           .N("events", (double)queues_.Frames(FrameKind::Event))
                           .N("bytes", (double)queues_.TotalBytes())
                           .N("rejected", (double)queues_.Rejected())
                           .Done())
        .Raw("heartbeat", ObjBuilder()
                              .N("pingsSent", (double)hb_.PingsSent())
                              .N("pongs", (double)hb_.PongsReceived())
                              .N("outstanding", (double)hb_.Outstanding())
                              .Raw("writtenThrough", std::to_string(hb_.Written()))
                              .Raw("confirmedThrough", std::to_string(hb_.Confirmed()))
                              .Done())
        .N("framesSent", (double)framesSent_)
        .N("bytesSent", (double)bytesSent_)
        .N("framesReceived", (double)framesReceived_)
        .N("bytesReceived", (double)bytesReceived_)
        .N("sendErrors", (double)sendErrors_)
        .N("maxSendMs", (double)maxSendMs_)
        .N("receive12152", (double)desyncs_)
        .S("tls", tlsMode_)
        .S("tlsSubject", tlsSubject_)
        .S("tlsIssuer", tlsIssuer_)
        .S("lastError", lastError_)
        .S("lastClose", lastClose_)
        .Done();
}

void WinHttpTransport::Fail(const std::string& stage, unsigned long err) {
    std::string name = ErrName((DWORD)err);
    std::string msg = "connect failed at " + stage + " (WinHTTP " + std::to_string(err) + (name.empty() ? "" : " " + name) + ")";
    {
        std::lock_guard<std::mutex> g(mu_);
        lastError_ = msg;
        connectFailures_++;
    }
    NativeLog("native: %s", msg.c_str());
    if (sink_) sink_({NoticeType::Error, 0, msg, 0});
}

void WinHttpTransport::Supervisor() {
    NativeLog("native: Takaro transport starting (%s%s)", o_.url.c_str(), o_.caFile.empty() ? "" : ", pinned CA file");
    while (!stopping_) {
        RunEpoch();
        if (stopping_) break;
        unsigned delay;
        {
            std::lock_guard<std::mutex> g(mu_);
            uint64_t d = (uint64_t)o_.reconnectBaseMs << std::min(attempts_, 20u);
            delay = (unsigned)std::min<uint64_t>(d, o_.reconnectMaxMs);
            attempts_++;
            nextAttemptAtMs_ = Now() + delay;
        }
        NativeLog("native: reconnecting to Takaro in %ums", delay);
        std::unique_lock<std::mutex> l(mu_);
        supCv_.wait_for(l, std::chrono::milliseconds(delay), [&] { return stopping_.load(); });
    }
    NativeLog("native: Takaro transport stopped");
}

void WinHttpTransport::RunEpoch() {
    Url url;
    if (!ParseUrl(o_.url, url)) {
        Fail("url (must be wss://host[:port]/path)", 0);
        return;
    }
    HCERTSTORE ca = nullptr;
    if (!o_.caFile.empty()) {
        std::string err;
        ca = LoadCaStore(o_.caFile, err);
        if (!ca) {
            // Fail closed: a configured CA that cannot be used never falls back to system trust.
            {
                std::lock_guard<std::mutex> g(mu_);
                lastError_ = err + "; refusing to connect (no fallback to system trust)";
                connectFailures_++;
            }
            NativeLog("native: %s; refusing to connect (no fallback to system trust)", err.c_str());
            if (sink_) sink_({NoticeType::Error, 0, err, 0});
            return;
        }
    }
    Epoch* e = new Epoch();
    e->owner = this;
    static const std::wstring agent = Wide("Takaro-Conan-Exiles-Native/" TAKARO_CONAN_NATIVE_VERSION);
    HINTERNET sess = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    HINTERNET conn = nullptr;
    auto waitReq = [&](BOOL started) -> BOOL {
        if (!started) return FALSE;
        if (WaitForSingleObject(e->evReq, 20000) != WAIT_OBJECT_0) {
            SetLastError(ERROR_WINHTTP_TIMEOUT);
            return FALSE;
        }
        if (e->reqErr) {
            SetLastError(e->reqErr);
            return FALSE;
        }
        return TRUE;
    };
    auto cleanup = [&](bool reqClosingNeeded) {
        bool safe = true;
        if (e->req) {
            WinHttpCloseHandle(e->req);
            safe = WaitForSingleObject(e->evReqClosing, 5000) == WAIT_OBJECT_0;
            e->req = nullptr;
        } else if (reqClosingNeeded) {
            safe = WaitForSingleObject(e->evReqClosing, 5000) == WAIT_OBJECT_0;
        }
        if (conn) WinHttpCloseHandle(conn);
        if (sess) WinHttpCloseHandle(sess);
        if (ca) CertCloseStore(ca, 0);
        Sleep(50);  // callbacks for the connect/session handles carry no context but let them drain
        if (safe) delete e;
        else NativeLog("native: WinHTTP request handle did not report closing; leaking its context");
    };
    auto fail = [&](const char* stage) {
        DWORD err = GetLastError();
        if (e->secureFailure) NativeLog("native: TLS secure failure flags 0x%lx", (unsigned long)e->secureFailure);
        Fail(stage, err);
        cleanup(false);
    };
    if (!sess) return fail("open");
    if (WinHttpSetStatusCallback(sess, StatusCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0) ==
        WINHTTP_INVALID_STATUS_CALLBACK)
        return fail("set-callback");
    WinHttpSetTimeouts(sess, 10000, 10000, 10000, 30000);
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos)) {
        protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(sess, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
    }
    conn = WinHttpConnect(sess, url.host.c_str(), url.port, 0);
    if (!conn) return fail("connect");
    e->req = WinHttpOpenRequest(conn, L"GET", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!e->req) return fail("open-request");
    if (ca) {
        // Only the unknown-CA check is relaxed in WinHTTP (name and dates stay on); the chain is then pinned
        // to caFile below, before any application data (identify) is sent.
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
        if (!WinHttpSetOption(e->req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags)) return fail("security-flags");
    }
    if (!WinHttpSetOption(e->req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return fail("upgrade-option");
    if (!waitReq(WinHttpSendRequest(e->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                                    (DWORD_PTR)e)))
        return fail("send-request");
    std::string subject, issuer, detail;
    if (ca) {
        if (!VerifyAgainstCa(e->req, ca, url.host, detail, subject, issuer)) {
            NativeLog("native: TLS rejected: server certificate '%s' (issuer '%s') does not chain to caFile for "
                      "this host: %s",
                      subject.c_str(), issuer.c_str(), detail.c_str());
            SetLastError(12045);
            return fail("ca-pin");
        }
    } else {
        PCCERT_CONTEXT srv = nullptr;
        DWORD len = sizeof srv;
        if (WinHttpQueryOption(e->req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &srv, &len) && srv) {
            subject = CertName(srv, 0);
            issuer = CertName(srv, CERT_NAME_ISSUER_FLAG);
            CertFreeCertificateContext(srv);
        }
    }
    if (!waitReq(WinHttpReceiveResponse(e->req, nullptr))) return fail("receive-response");
    DWORD status = 0, sl = sizeof status;
    WinHttpQueryHeaders(e->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        SetLastError(status);
        return fail("upgrade-status (HTTP status instead of 101)");
    }
    if (!EndpointsFromRequest(e->req, e->tcp))
        NativeLog("native: WARNING no connection addresses from WinHTTP (%lu); a dead link will close gracefully",
                  (unsigned long)GetLastError());
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(e->req, (DWORD_PTR)e);
    if (!ws) return fail("complete-upgrade");
    e->ws = ws;
    WinHttpCloseHandle(e->req);
    bool reqClosed = WaitForSingleObject(e->evReqClosing, 5000) == WAIT_OBJECT_0;
    e->req = nullptr;

    {
        std::lock_guard<std::mutex> g(mu_);
        e->id = ++epoch_;
        cur_ = e;
        open_ = true;
        queues_.Clear();
        hb_.Reset(Now());
        connects_++;
        openedAtMs_ = Now();
        nextAttemptAtMs_ = 0;
        lastError_.clear();
        tlsMode_ = ca ? "pinned-ca" : "system-trust";
        tlsSubject_ = subject;
        tlsIssuer_ = issuer;
    }
    NativeLog("native: Takaro WebSocket upgraded (epoch %llu, tls %s, subject '%s', issuer '%s')",
              (unsigned long long)e->id, ca ? "pinned-ca" : "system-trust", subject.c_str(), issuer.c_str());
    if (sink_) sink_({NoticeType::Open, e->id, "", 0});

    HANDLE hs = CreateThread(nullptr, 0, SendThunk, e, 0, nullptr);
    HANDLE hr = CreateThread(nullptr, 0, RecvThunk, e, 0, nullptr);
    if (!hs || !hr) MarkDead(e, "thread creation failed");

    {
        std::unique_lock<std::mutex> l(mu_);
        while (!e->dead && !stopping_) {
            supCv_.wait_for(l, std::chrono::milliseconds(1000));
            if (!e->dead && hb_.Dead(Now())) {
                e->dead = true;
                e->why = "no message from Takaro for " + std::to_string(o_.heartbeat.idleMs / 1000) + "s (dead link)";
            }
        }
        if (!e->dead) {
            e->dead = true;
            e->why = "stopping";
        }
        open_ = false;  // no new frames for this epoch
    }
    sendCv_.notify_all();
    std::string why;
    {
        std::lock_guard<std::mutex> g(mu_);
        why = e->why;
    }
    // The send thread must be quiet before the handle goes away; a send stuck on a full TCP window ends
    // with its own 30 s wait.
    bool sendJoined = !hs || WaitForSingleObject(hs, 35000) == WAIT_OBJECT_0;
    if (sendJoined && !e->serverClosed && why.rfind("receive", 0) != 0 && why.find("dead link") == std::string::npos) {
        // polite close frame on a live link; async shutdown returns at once (G0 c7)
        const char reason[] = "connector closing";
        if (WinHttpWebSocketShutdown(ws, 1000, (PVOID)reason, sizeof reason - 1) == NO_ERROR)
            WaitForSingleObject(e->evClose, 2000);
    } else if (!e->serverClosed) {
        // Abortive close: the kernel drops what is still unsent instead of delivering it once the route
        // is back. Those frames are replayed on the next connection from the outbox; delivering the old
        // copies too stores every outage-time event twice (Linux transport, live 2026-10-03).
        int64_t t0 = Now();
        AbortResult ar = MakeCloseAbortive(e->tcp);
        if (ar.matched)
            NativeLog("native: dead connection %s -> %s closes abortively (%d handles scanned in %lldms)",
                      EndpointText(e->tcp.local).c_str(), EndpointText(e->tcp.remote).c_str(), ar.handlesScanned,
                      (long long)(Now() - t0));
        else
            NativeLog("native: WARNING dead connection closes gracefully: %s", ar.error.c_str());
    }
    WinHttpCloseHandle(ws);  // async: cancels the pending receive with 12017
    bool recvJoined = !hr || WaitForSingleObject(hr, 10000) == WAIT_OBJECT_0;
    bool wsClosed = WaitForSingleObject(e->evWsClosing, 5000) == WAIT_OBJECT_0;
    if (hs) CloseHandle(hs);
    if (hr) CloseHandle(hr);
    if (conn) WinHttpCloseHandle(conn);
    if (sess) WinHttpCloseHandle(sess);
    if (ca) CertCloseStore(ca, 0);
    uint64_t id = e->id;
    {
        std::lock_guard<std::mutex> g(mu_);
        cur_ = nullptr;
        queues_.Clear();
        lastClose_ = why;
    }
    NativeLog("native: Takaro WebSocket epoch %llu ended: %s", (unsigned long long)id, why.c_str());
    if (sink_) sink_({NoticeType::Closed, id, why, 0});
    if (sendJoined && recvJoined && wsClosed && reqClosed) {
        Sleep(100);
        delete e;
    } else {
        NativeLog("native: epoch %llu teardown incomplete (send %d recv %d wsClosing %d reqClosing %d); leaking its "
                  "context",
                  (unsigned long long)id, sendJoined, recvJoined, wsClosed, reqClosed);
    }
}

unsigned long __stdcall WinHttpTransport::SendThunk(void* p) {
    auto* e = (Epoch*)p;
    e->owner->SendLoop(e);
    return 0;
}

unsigned long __stdcall WinHttpTransport::RecvThunk(void* p) {
    auto* e = (Epoch*)p;
    e->owner->RecvLoop(e);
    return 0;
}

void WinHttpTransport::SendLoop(Epoch* e) {
    static const std::shared_ptr<const std::string> ping = std::make_shared<const std::string>(kPing);
    for (;;) {
        OutFrame f;
        bool isPing = false;
        {
            std::unique_lock<std::mutex> l(mu_);
            for (;;) {
                if (e->dead) return;
                int64_t now = Now();
                if (hb_.PingDue(now, queues_.Frames(FrameKind::Event) == 0)) {
                    hb_.OnPingSent(now);  // before the write, so its pong always finds the FIFO entry
                    isPing = true;
                    f.text = ping;
                    break;
                }
                if (queues_.Pop(f)) break;
                sendCv_.wait_for(l, std::chrono::milliseconds(200));
            }
        }
        int64_t t0 = Now();
        DWORD r = WinHttpWebSocketSend(e->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)f.text->data(),
                                       (DWORD)f.text->size());
        if (r == NO_ERROR) {
            // f.text stays alive until WRITE_COMPLETE; a dead link still completes into the kernel buffer
            r = WaitForSingleObject(e->evSend, 30000) == WAIT_OBJECT_0 ? e->sendErr : ERROR_WINHTTP_TIMEOUT;
        }
        {
            std::lock_guard<std::mutex> g(mu_);
            int64_t dt = Now() - t0;
            if (dt > maxSendMs_) maxSendMs_ = dt;
            if (r == NO_ERROR) {
                framesSent_++;
                bytesSent_ += f.text->size();
                if (!isPing && f.kind == FrameKind::Event) hb_.OnEventWritten(f.outboxId);
            } else {
                sendErrors_++;
            }
        }
        if (r != NO_ERROR) {
            std::string name = ErrName(r);
            MarkDead(e, "send error " + std::to_string(r) + (name.empty() ? "" : " (" + name + ")"));
            return;
        }
    }
}

void WinHttpTransport::Deliver(Epoch* e, std::string&& msg) {
    uint64_t confirmed = 0;
    bool pong = false;
    {
        std::lock_guard<std::mutex> g(mu_);
        framesReceived_++;
        bytesReceived_ += msg.size();
        hb_.OnInbound(Now());
        if (msg.size() < 512) {
            JsonValue v;
            if (ParseJson(msg, v) && v.type == JsonValue::Object) {
                const JsonValue* t = v.get("type");
                if (t && t->type == JsonValue::String && t->str == "pong") {
                    pong = true;
                    confirmed = hb_.OnPong();
                }
            }
        }
    }
    if (!sink_) return;
    if (pong) {
        if (confirmed) sink_({NoticeType::Confirmed, e->id, "", confirmed});
        return;
    }
    if (!sink_({NoticeType::Frame, e->id, std::move(msg), 0})) MarkDead(e, "bridge overloaded (inbound notices full)");
}

void WinHttpTransport::RecvLoop(Epoch* e) {
    std::string acc;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bt = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        DWORD r = WinHttpWebSocketReceive(e->ws, buf.data(), (DWORD)buf.size(), &got, &bt);
        if (r == NO_ERROR) {
            WaitForSingleObject(e->evRecv, INFINITE);  // READ_COMPLETE, or REQUEST_ERROR (12017 when cancelled)
            r = e->recvErr;
            got = e->recvBytes;
            bt = e->recvType;
        }
        if (r != NO_ERROR) {
            if (r == 12152) {
                std::lock_guard<std::mutex> g(mu_);
                desyncs_++;
                // Wine reports an aborted TCP connection this way, and also a stream desynced by a server PING
                // with a payload (its WinHTTP never drains PING payloads; Takaro's pings are empty today).
                NativeLog("native: WARNING WinHTTP receive failed with 12152 (connection aborted, or a WebSocket "
                          "PING payload Wine did not drain); reconnecting");
            }
            std::string name = ErrName(r);
            MarkDead(e, "receive error " + std::to_string(r) + (name.empty() ? "" : " (" + name + ")"));
            return;
        }
        if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 0;
            char reason[128] = {0};
            DWORD rl = 0;
            WinHttpWebSocketQueryCloseStatus(e->ws, &code, reason, sizeof reason - 1, &rl);
            e->serverClosed = true;
            MarkDead(e, "server closed the connection (code " + std::to_string(code) + " " + std::string(reason, rl) + ")");
            return;
        }
        acc.append(buf.data(), got);
        if (acc.size() > kMaxInboundBytes) {
            MarkDead(e, "inbound message exceeds 1 MiB");
            return;
        }
        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            Deliver(e, std::move(acc));
            acc.clear();
        }
    }
}

}  // namespace takaro
