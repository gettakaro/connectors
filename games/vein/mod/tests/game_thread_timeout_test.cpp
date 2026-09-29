#include "gamethread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;

static bool Until(const std::atomic<bool>& flag) {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return flag;
}

static bool UntilQueued() {
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!GameThread::TestQueued() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    return GameThread::TestQueued() != 0;
}

int main() {
    GameThread::TestEnable();
    std::atomic<int> effects{0};

    // An expired queued job must be cancelled before it starts.
    if (GameThread::Run([&effects] { ++effects; }, 1)) return 1;
    GameThread::TestPumpOnce();
    if (effects != 0 || GameThread::TestQueued() != 0) return 2;

    // Start the job while Run is waiting, then keep it blocked until after the waiter times out.
    // Its payload is owned by the job and remains valid when the caller drops its reference.
    std::atomic<bool> started{false}, release{false}, finished{false}, completed{true};
    auto payload = std::make_shared<std::string>("owned message");
    std::thread caller([&] {
        completed = GameThread::Run([payload, &started, &release, &finished] {
            started = true;
            if (!Until(release)) return;
            if (*payload == "owned message") finished = true;
        }, 1000);
    });
    if (!UntilQueued()) return 3;
    std::thread pump([] { GameThread::TestPumpOnce(); });
    if (!Until(started)) return 4;
    caller.join();
    payload.reset();
    release = true;
    pump.join();
    if (completed || !finished) return 5;

    // This caller's `output` and lambda frame are destroyed before the game job is released.
    std::atomic<bool> jsonStarted{false}, jsonRelease{false}, jsonFinished{false}, jsonCompleted{true};
    std::thread jsonCaller([&] {
        std::string output = "unchanged";
        jsonCompleted = GameThread::RunJson([&jsonStarted, &jsonRelease, &jsonFinished] {
            jsonStarted = true;
            if (!Until(jsonRelease)) return std::string("timeout watchdog");
            jsonFinished = true;
            return std::string("{\"ok\":true}");
        }, output, 1000);
        if (output != "unchanged") jsonCompleted = true;
    });
    if (!UntilQueued()) return 6;
    std::thread jsonPump([] { GameThread::TestPumpOnce(); });
    if (!Until(jsonStarted)) return 7;
    jsonCaller.join();
    jsonRelease = true;
    jsonPump.join();
    if (jsonCompleted || !jsonFinished) return 8;

    // The boundary is exactly 256 queued jobs; the 257th is rejected without admission.
    for (int i = 0; i < 256; ++i) GameThread::Run([] {}, 0);
    if (GameThread::TestQueued() != 256) return 9;
    GameThread::Run([] {}, 0);
    if (GameThread::TestQueued() != 256) return 10;
    for (int i = 0; i < 16; ++i) GameThread::TestPumpOnce();
    if (GameThread::TestQueued() != 0) return 11;

    puts("game-thread timeout ownership: passed");
}
