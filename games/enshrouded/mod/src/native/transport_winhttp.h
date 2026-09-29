// Takaro WebSocket over WinHTTP in ASYNC mode (the only mode that passed G0 under GE-Proton10-30:
// evidence 2026-09-29-g0-winhttp-probe). Per connection ("epoch"):
//  - a supervisor thread opens it, watches it and tears it down;
//  - one send thread is the only caller of WinHttpWebSocketSend, one send outstanding at a time, the
//    buffer alive until WRITE_COMPLETE (Wine answers a second concurrent send with 4317);
//  - one receive thread is the only caller of WinHttpWebSocketReceive; closing the handle cancels it (12017).
// Dead links are detected by the application heartbeat (Heartbeat): WinHTTP's receive timeout does not
// apply to WebSockets under Wine. TLS uses the system store, or only TAKARO_CA_FILE when set (pinned root,
// hostname and dates checked); an unusable CA file fails closed. There is no insecure fallback.
#pragma once
#include "native/transport.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace native {

class WinHttpTransport : public ITransport {
public:
    struct Options {
        std::string url;
        std::string caFile;
        unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;
        Heartbeat::Params heartbeat;
    };
    explicit WinHttpTransport(Options o);
    ~WinHttpTransport() override;
    bool Start(NoticeSink sink) override;
    void Stop() override;
    QueueStatus Queue(OutFrame frame) override;
    void RequestClose(uint64_t epoch, const std::string& reason) override;
    void MarkIdentified(uint64_t epoch) override;
    std::string StatsJson() override;

    struct Epoch;  // per-connection context (transport_winhttp.cpp)

private:
    void Supervisor();
    void RunEpoch();
    void MarkDead(Epoch* e, const std::string& why);
    static unsigned long __stdcall SendThunk(void* p);
    static unsigned long __stdcall RecvThunk(void* p);
    void SendLoop(Epoch* e);
    void RecvLoop(Epoch* e);
    void Deliver(Epoch* e, std::string&& msg);
    void Fail(const std::string& stage, unsigned long err);

    Options o_;
    NoticeSink sink_;
    std::thread supervisor_;
    std::atomic<bool> stopping_{false};

    std::mutex mu_;
    std::condition_variable sendCv_, supCv_;
    Epoch* cur_ = nullptr;
    bool open_ = false;
    uint64_t epoch_ = 0;
    unsigned attempts_ = 0;
    FrameQueues queues_;
    Heartbeat hb_;

    // stats (under mu_)
    uint64_t connects_ = 0, connectFailures_ = 0, framesSent_ = 0, bytesSent_ = 0, framesReceived_ = 0,
             bytesReceived_ = 0, sendErrors_ = 0, desyncs_ = 0;
    int64_t openedAtMs_ = 0, nextAttemptAtMs_ = 0, maxSendMs_ = 0;
    std::string lastError_, lastClose_, tlsSubject_, tlsIssuer_, tlsMode_;
};

}  // namespace native
