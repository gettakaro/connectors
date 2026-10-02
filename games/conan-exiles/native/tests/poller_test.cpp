// Drives the real poller, HTTP client and game-thread queue against fake_bridge_test.py. The
// game is replaced by a loop on the main thread that drains the queue, as the ProcessEvent
// detour does, and a stub "chat renderer" that runs inside the drained job.
//
// Usage: poller_test <bridge-url> <seconds>
// Stub behaviour, keyed on the command: recipient "offline" -> not online; message "stall" ->
// the fake game thread stops draining for 3 s, so the 2 s game-thread wait must time out.
#include "common.h"
#include "gamethread.h"
#include "poller.h"
#include "proto.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

static std::atomic<uint64_t> g_stallUntil{0};

static ChatOutcome StubChat(const PollCommand& cmd) {
    ChatOutcome o;
    if (cmd.message == "stall") g_stallUntil = NowMs() + 3000;
    int delivered = 0;
    bool ran = GameThread::Run(
        [&] {
            if (cmd.recipient != "offline") delivered = 1;
        },
        2000);
    if (!ran) {
        o.error = "game thread did not respond";
        return o;
    }
    o.delivered = delivered;
    o.success = delivered > 0;
    if (!o.success) o.error = "Recipient " + cmd.recipient + " is not online";
    return o;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: poller_test <bridge-url> <seconds>\n");
        return 2;
    }
    SetNativeLogPath("/dev/stderr");
    Poller::Config cfg;
    if (!ParseHttpUrl(argv[1], cfg.url)) return 2;
    cfg.intervalMs = 50;
    cfg.maxBackoffMs = 400;
    cfg.requestTimeoutMs = 1000;
    Poller::Start(cfg, StubChat);
    const uint64_t end = NowMs() + (uint64_t)atoi(argv[2]) * 1000;
    while (NowMs() < end) {
        if (NowMs() >= g_stallUntil && GameThread::g_pending.load()) GameThread::Drain();
        usleep(2000);
    }
    GameThread::Stop();
    Poller::Stop();
    FlushNativeLogs();
    return 0;
}
