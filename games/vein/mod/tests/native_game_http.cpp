#include "native_transport.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    NativeTransport::Config config;
    config.url = argv[1];
    config.caFile = argv[2];
    config.gameHttpUrl = argv[3];
    const std::string mode = argv[4];
    if (!NativeTransport::Start(config, [](NativeTransport::Notice) { return true; })) return 3;
    bool passed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !NativeTransport::Snapshot().connected)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto before = NativeTransport::Snapshot();
    if (before.connected) {
        const auto started = std::chrono::steady_clock::now();
        if (mode == "timeout-retry") {
            auto first = NativeTransport::FetchGamePlayers();
            auto elapsed = std::chrono::steady_clock::now() - started;
            // The canceled socket may take one service tick to close. Retry
            // boundedly until its mailbox has been released.
            std::optional<std::string> second;
            const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < retryDeadline && !second) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                second = NativeTransport::FetchGamePlayers();
            }
            passed = !first && elapsed < std::chrono::milliseconds(3500) &&
                     second && second->find("steamId") != std::string::npos;
        } else {
            auto result = NativeTransport::FetchGamePlayers();
            const bool expect = mode == "valid";
            passed = expect ? (result && result->find("steamId") != std::string::npos)
                            : !result;
            passed = passed && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(3500);
        }
        const auto after = NativeTransport::Snapshot();
        passed = passed && after.connected && after.epoch == before.epoch;
    }
    NativeTransport::Stop();
    if (!passed) std::cerr << "game HTTP fallback failed: " << mode << '\n';
    return passed ? 0 : 1;
}
