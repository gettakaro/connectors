// Research probe for issue #298 (C4): what does Conan's GameThread::Run do when its timeout
// expires while the job is already running? Built only by run.sh in this folder.
#include "gamethread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int main() {
    std::atomic<bool> started{false};
    int captured = 0;  // caller stack data captured by reference, as chat.cpp/actions_ue.cpp do
    auto t0 = Clock::now();
    std::thread caller([&] {
        bool ok = GameThread::Run([&] {
            started = true;
            std::this_thread::sleep_for(1500ms);  // a slow game-thread job
            captured = 42;
        }, 200);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        printf("Run(timeout=200ms) returned ok=%d after %lld ms, captured=%d\n", ok, (long long)ms, captured);
    });
    std::this_thread::sleep_for(20ms);
    std::thread pump([] { GameThread::Drain(); });
    caller.join();
    pump.join();
    // Queued (not started) job past its deadline: cancelled, never runs.
    int effects = 0;
    bool ok = GameThread::Run([&] { ++effects; }, 1);
    GameThread::Drain();
    printf("queued job past deadline: ok=%d effects=%d\n", ok, effects);
    // Admission: count how many jobs are accepted while nothing drains (non-blocking submitters).
    std::atomic<int> accepted{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 600; ++i)
        ts.emplace_back([&] { if (GameThread::Run([] {}, 30000)) ++accepted; });
    std::this_thread::sleep_for(500ms);  // all 600 are queued while nothing drains
    std::atomic<bool> done{false};
    std::thread drainer([&] { while (!done) { GameThread::Drain(); } });
    for (auto& t : ts) t.join();
    done = true;
    drainer.join();
    printf("600 concurrent submitters while the pump was stalled (Dune and Dragonwilds cap at 256): admitted and run=%d\n", accepted.load());
    return 0;
}
