// Takaro WebSocket over WinHTTP in ASYNC mode, ported from the Enshrouded native connector
// (games/enshrouded/mod/src/native/transport_winhttp.cpp; sync mode wedged under Takaro's pings, best-practice
// ledger #27). Per connection ("epoch"):
//  - a supervisor thread opens it, watches it and tears it down;
//  - one send thread is the only caller of WinHttpWebSocketSend, one send outstanding at a time, the
//    buffer alive until WRITE_COMPLETE;
//  - one receive thread is the only caller of WinHttpWebSocketReceive; closing the handle cancels it (12017).
// Dead links are detected by Takaro's application heartbeat ({"type":"ping"} -> {"type":"pong"}, confirmed in
// order with Heartbeat::OnPong()). TLS uses the Windows system store, or only caFile when set (pinned root,
// hostname and dates checked); an unusable CA file fails closed. There is no insecure fallback.
#pragma once
#include "takaro/transport.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace takaro {

struct WinHttpConfig {
    std::string url;     // wss://host[:port]/path
    std::string caFile;  // PEM bundle to pin; empty = the Windows system store
    unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;
    Heartbeat::Params heartbeat;
};

class WinHttpTransport : public ITransport {
public:
    explicit WinHttpTransport(WinHttpConfig cfg);
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

    WinHttpConfig o_;
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

}  // namespace takaro
