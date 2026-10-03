// Windows transport (stub interface; the Windows lane fills it in): Enshrouded's WinHTTP WebSocket
// transport (games/enshrouded/mod/src/native/transport_winhttp.cpp) in async mode, behind the core
// ITransport contract. Under WinHTTP the heartbeat is Takaro's application ping {"type":"ping"},
// confirmed in order with Heartbeat::OnPong(); one outstanding send, one receive thread, cancel by
// closing the handle (best-practice ledger #27).
#pragma once
#include "takaro/transport.h"

#include <memory>
#include <string>

namespace takaro {

struct WinHttpConfig {
    std::string url;  // wss://host[:port]/path
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

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace takaro
