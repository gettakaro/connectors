#include "gamethread.h"

#include "common.h"


#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace GameThread {

std::atomic<bool> g_pending{false};

namespace {
enum class State { Queued, Running, Done, Cancelled };
struct Job {
    std::function<void()> fn;
    State state = State::Queued;
};

// Function-local and leaked on purpose: independent of static init order, and the poller may
// still be waiting in Run() when the process exits.
struct Queue {
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Job>> jobs;
    bool stopped = false;
};
Queue& Q() {
    static Queue* q = new Queue;
    return *q;
}

constexpr int kMaxJobsPerDrain = 4;
constexpr uint64_t kBudgetNs = 500 * 1000;

}  // namespace

bool Run(std::function<void()> fn, int timeoutMs) {
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    Queue& q = Q();
    std::unique_lock<std::mutex> lk(q.lock);
    if (q.stopped) return false;
    q.jobs.push_back(job);
    g_pending.store(true, std::memory_order_release);
    bool finished = q.cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                  [&] { return job->state == State::Done || job->state == State::Cancelled; });
    if (finished) return job->state == State::Done;
    if (job->state == State::Queued) {
        job->state = State::Cancelled;  // Drain skips it
        return false;
    }
    // Running: the job may reference our stack, so it must finish before we return.
    q.cv.wait(lk, [&] { return job->state == State::Done; });
    return true;
}

void Drain() {
    const uint64_t start = NowNs();
    Queue& q = Q();
    for (int n = 0; n < kMaxJobsPerDrain; n++) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lk(q.lock);
            while (!q.jobs.empty() && q.jobs.front()->state == State::Cancelled) q.jobs.pop_front();
            if (q.jobs.empty()) {
                g_pending.store(false, std::memory_order_relaxed);
                return;
            }
            job = q.jobs.front();
            q.jobs.pop_front();
            job->state = State::Running;
        }
        job->fn();
        {
            std::lock_guard<std::mutex> lk(q.lock);
            job->state = State::Done;
        }
        q.cv.notify_all();
        if (NowNs() - start > kBudgetNs) break;
    }
}

void Stop() {
    Queue& q = Q();
    std::lock_guard<std::mutex> lk(q.lock);
    q.stopped = true;
    for (auto& j : q.jobs)
        if (j->state == State::Queued) j->state = State::Cancelled;
    q.jobs.clear();
    g_pending.store(false, std::memory_order_relaxed);
    q.cv.notify_all();
}

}  // namespace GameThread
