#include "conan/perf_sampler.h"

#include "common.h"
#include "conan/actions_ue.h"
#include "gamethread.h"
#include "gtstats.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace conan {
namespace {

constexpr int kJobTimeoutMs = 3000;

struct Sampler {
    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    bool started = false, stopping = false;
};
Sampler& S() {
    static Sampler* s = new Sampler;
    return *s;
}

// The frame counter, or -1. The CDO and the UFunction are found once (EnsureLookups, a sliced
// object walk); later samples are one game-thread job.
int64_t ReadFrames(std::string& state) {
    static uintptr_t lib = 0;
    static rx::Func fn;
    static bool haveFn = false;
    std::string error;
    if (!lib) {
        rx::Lookups l;
        if (!rx::EnsureWorld(error) || !rx::EnsureLookups(l, error)) {
            state = "waiting: " + error;
            return -1;
        }
        lib = l.systemLibrary;
    }
    int64_t frames = -1;
    bool dead = false;
    bool ran = GameThread::Run(
        [&] {
            if (!rx::Alive(lib)) {
                dead = true;
                return;
            }
            if (!haveFn) {
                if (!rx::ResolveFunc(rx::ClassOf(lib), "GetFrameCount", {{"ReturnValue", "Int64Property", 8}}, fn, error))
                    return;
                haveFn = true;
            }
            rx::Frame f(fn);
            if (rx::Call(lib, fn, f, &error)) frames = f.GetInt64("ReturnValue");
        },
        kJobTimeoutMs);
    if (dead) {
        lib = 0;
        state = "waiting: KismetSystemLibrary CDO is gone";
        return -1;
    }
    if (!ran) {
        state = "game thread did not respond";
        return -1;
    }
    state = frames >= 0 ? "ok" : "GetFrameCount failed: " + error;
    return frames;
}

void Loop(int periodMs) {
    std::unique_lock<std::mutex> l(S().mu);
    while (!S().stopping) {
        l.unlock();
        std::string state;
        const int64_t frames = ReadFrames(state);
        GtStats::RecordSample(frames, NowNs());
        GtStats::SetSamplerState(state);
        l.lock();
        S().cv.wait_for(l, std::chrono::milliseconds(periodMs), [] { return S().stopping; });
    }
}

}  // namespace

void StartPerfSampler(int periodMs) {
    std::lock_guard<std::mutex> g(S().mu);
    if (S().started) return;
    S().started = true;
    S().thread = std::thread(Loop, periodMs);
}

void StopPerfSampler() {
    {
        std::lock_guard<std::mutex> g(S().mu);
        if (!S().started) return;
        S().stopping = true;
    }
    S().cv.notify_all();
    if (S().thread.joinable()) S().thread.join();
}

}  // namespace conan
