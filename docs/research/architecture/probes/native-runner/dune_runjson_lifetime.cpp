// Research probe for issue #298 (C4 / DUN-2): does a Dune game-thread job that has STARTED keep
// running after GameThread::RunJson has timed out and returned, and does it then write into the
// caller's stack? Built only by run.sh in this folder; not part of any connector build or CI job.
#include "gamethread.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace std::chrono_literals;

static std::atomic<bool> g_started{false}, g_release{false};

static void Caller() {
    std::string out = "unchanged";
    bool ok = GameThread::RunJson([] {
        g_started = true;
        while (!g_release) std::this_thread::sleep_for(1ms);
        return std::string(512, 'x');  // assigned into RunJson's stack `result` after it returned
    }, out, 200);
    printf("RunJson returned ok=%d out=%s (caller frame is now gone)\n", ok, out.c_str());
}

int main() {
    GameThread::MarkPumpAvailable("research-probe");
    std::thread caller(Caller);
    std::this_thread::sleep_for(20ms);  // let RunJson queue its job
    std::thread pump([] { GameThread::PumpOnce(); });  // the "game thread" starts the job
    pump.detach();
    while (!g_started) std::this_thread::sleep_for(1ms);
    caller.join();                    // RunJson timed out and returned while the job still runs
    std::thread([] { char burn[8192]; for (auto& c : burn) c = 1; (void)burn; }).join();
    g_release = true;                 // the job now writes into a dead frame
    std::this_thread::sleep_for(500ms);
    puts("probe finished without a sanitizer report");
    return 0;
}
