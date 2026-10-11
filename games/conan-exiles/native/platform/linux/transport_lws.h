// Linux transport: libwebsockets 4.5.8 + OpenSSL 3.5.8, both static and built on buster
// (platform/linux/Dockerfile.build). Ported from the VEIN native connector
// (games/vein/mod/src/native_transport.cpp), behind the core ITransport contract.
//
// One service thread owns the socket. Queue() and RequestClose() only take a lock and wake it.
// Heartbeat: a WebSocket ping every 5 s whose payload is the ping number; the matching pong
// confirms every event written before that ping (core Heartbeat). No inbound frame for 20 s, or two
// pings unanswered for 5 s, closes the link; the bridge replays the unconfirmed outbox after the
// next identify. Reconnect backoff doubles from reconnectBaseMs to reconnectMaxMs and resets only
// after a successful identify, so a server that accepts the socket but rejects identify is not
// hammered.
#pragma once
#include "takaro/transport.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace takaro {

struct LwsConfig {
    std::string url;     // wss://host[:port]/path
    std::string caFile;  // PEM bundle; empty = the first system bundle found (see Run())
    unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;
    bool connect = true;  // false: stay idle until Retarget enables it
    Heartbeat::Params heartbeat;
};

class LwsTransport : public ITransport {
public:
    explicit LwsTransport(LwsConfig cfg);
    ~LwsTransport() override;
    bool Start(NoticeSink sink) override;
    void Stop() override;
    QueueStatus Queue(OutFrame frame) override;
    void RequestClose(uint64_t epoch, const std::string& reason) override;
    void MarkIdentified(uint64_t epoch) override;
    void Retarget(const std::string& url, bool connect) override;
    std::string StatsJson() override;

    struct Impl;  // public for the C callback

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace takaro
