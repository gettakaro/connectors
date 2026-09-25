#include "native_transport.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string mode = argv[3];
    std::mutex mutex;
    std::condition_variable changed;
    unsigned opens = 0, closes = 0, errors = 0;
    uint64_t epoch = 0, confirmed = 0;
    bool pruneReady = false;
    NativeTransport::Ticket writtenTicket, confirmedTicket;
    NativeTransport::Config config;
    config.url = argv[1];
    config.caFile = argv[2];
    if (!NativeTransport::Start(config, [&](NativeTransport::Notice n) {
        std::lock_guard<std::mutex> lock(mutex);
        if (std::getenv("VEIN_TLS_TRACE")) std::cerr << "notice=" << static_cast<int>(n.type) << " seq=" << n.eventSeq << " " << n.text << '\n';
        if (n.type == NativeTransport::NoticeType::Open) {
            ++opens; epoch = n.epoch;
            if (mode == "prune") {
                auto first = NativeTransport::Queue({NativeTransport::Kind::Event,
                    std::make_shared<const std::string>(R"({"seq":1})"), n.epoch, 1, false});
                auto second = NativeTransport::Queue({NativeTransport::Kind::Event,
                    std::make_shared<const std::string>(R"({"seq":2})"), n.epoch, 2, false});
                pruneReady = first && second && NativeTransport::PruneEventsBefore(2, n.epoch) == 1;
            }
        }
        if (n.type == NativeTransport::NoticeType::Closed) ++closes;
        if (n.type == NativeTransport::NoticeType::Error) ++errors;
        if (n.type == NativeTransport::NoticeType::Confirmed) confirmed = n.eventSeq;
        if (n.type == NativeTransport::NoticeType::Written && n.pingAfterWrite) writtenTicket = n.ticket;
        if (n.type == NativeTransport::NoticeType::Confirmed) confirmedTicket = n.ticket;
        changed.notify_all();
        return true;
    })) return 3;
    bool passed = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (mode == "reject") {
            changed.wait_for(lock, std::chrono::seconds(8), [&] { return opens || errors; });
            passed = !opens && errors;
        } else {
            changed.wait_for(lock, std::chrono::seconds(8), [&] { return opens || errors; });
            if (opens) {
                const uint64_t connectedEpoch = epoch;
                lock.unlock();
                bool staleRejected = !NativeTransport::Queue(NativeTransport::Kind::Response, "{}", 0, connectedEpoch + 1);
                bool largeResponseRejected = !NativeTransport::Queue(NativeTransport::Kind::Response,
                    std::string(8 * 1024 * 1024 + 1, 'x'), 0, connectedEpoch);
                bool largeEventRejected = !NativeTransport::Queue(NativeTransport::Kind::Event,
                    std::string(32 * 1024 * 1024 + 1, 'x'), 7, connectedEpoch);
                bool queued = mode == "prune" ? pruneReady : NativeTransport::Queue(NativeTransport::Kind::Event,
                    R"({"type":"gameEvent","payload":{"type":"chat-message","data":{"msg":"tls-test"}}})",
                    7, connectedEpoch);
                NativeTransport::QueueResult shutdownTicket;
                if (mode == "ticket")
                    shutdownTicket = NativeTransport::Queue({NativeTransport::Kind::CriticalResponse,
                        std::make_shared<const std::string>(R"({"type":"response","requestId":"stop","payload":{}})"),
                        connectedEpoch, 0, true});
                if (mode == "blackhole") {
                    // Fill the peer's receive window so heartbeat failure cannot depend
                    // on another writable callback arriving.
                    for (int i = 0; i < 3; ++i)
                        NativeTransport::Queue(NativeTransport::Kind::Response,
                            std::string(8 * 1024 * 1024, 'x'), 0, connectedEpoch);
                }
                lock.lock();
                if (mode == "prune") {
                    changed.wait_for(lock, std::chrono::seconds(9), [&] { return confirmed == 2 || closes; });
                    passed = confirmed == 2 && !closes && NativeTransport::Snapshot().prunedEvents >= 1;
                } else if (mode == "valid") {
                    changed.wait_for(lock, std::chrono::seconds(9), [&] { return confirmed || closes; });
                    passed = confirmed == 7 && !closes;
                } else if (mode == "ticket") {
                    changed.wait_for(lock, std::chrono::seconds(3), [&] {
                        return confirmedTicket.writeId == shutdownTicket.ticket.writeId || closes;
                    });
                    passed = shutdownTicket && writtenTicket.writeId == shutdownTicket.ticket.writeId &&
                             confirmedTicket.writeId == shutdownTicket.ticket.writeId && !closes;
                } else if (mode == "bad-pong" || mode == "blackhole") {
                    changed.wait_for(lock, std::chrono::seconds(28), [&] { return opens >= 2 || confirmed; });
                    passed = opens >= 2 && closes >= 1 && confirmed == 0 && epoch > connectedEpoch;
                } else if (mode == "oversize") {
                    changed.wait_for(lock, std::chrono::seconds(8), [&] { return closes; });
                    passed = closes > 0 && errors > 0 && confirmed == 0;
                }
                passed = passed && staleRejected && largeResponseRejected && largeEventRejected && queued;
            }
        }
    }
    NativeTransport::Stop();
    std::cout << mode << ": " << (passed ? "PASS" : "FAIL") << " opens=" << opens
              << " closes=" << closes << " errors=" << errors << " confirmed=" << confirmed << '\n';
    return passed ? 0 : 1;
}
