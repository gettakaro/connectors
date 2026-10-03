// The `log` event: a worker-thread tail of ConanSandbox/Saved/Logs/ConanSandbox.log (S4: no GLog
// hook, which would need two more signature pins and run on the logging thread). Zero game-thread
// cost; S4 measured the file lag at p50 114 ms, p90 201 ms.
//
// Rotation: the engine renames the file to ConanSandbox-backup-<date>.log at server start and opens
// a new one. The tail follows the path and restarts at offset 0 when the file identity (inode)
// changes or the size shrinks. The file that exists when the library loads is the previous run's,
// so the first observation starts at its end; a file created later is read from the beginning.
//
// Every line is redacted (config tokens, password=/token= values) and rate limited (token bucket,
// default 30 lines/s with a burst of 300); dropped lines are counted and summarised every 10 s.
//
// Other lanes (console output bracketing): record ServerLogSize() before an engine call and read
// the bytes after it with takaro::ReadFileRange(ServerLogPath(), ...).
#pragma once

#include "conan/events_payload.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace conan {

struct LogTailOptions {
    std::string path;                  // ConanSandbox.log
    std::vector<std::string> secrets;  // literal values to scrub (tokens)
    double perSecond = 30, burst = 300;
    int pollMs = 200;
    size_t maxReadBytes = 256 * 1024;  // per poll
    std::function<void(const std::string& line)> emit;
};

class LogTail {
public:
    explicit LogTail(LogTailOptions o);
    ~LogTail();
    void Start();
    void Stop();
    // One poll (the thread calls it; tests call it directly with their own clock).
    void Step(uint64_t nowMs);
    std::string HealthJson();

private:
    LogTailOptions o_;
    events::LineSplitter split_;
    events::RateLimiter rate_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stopping_ = false;
    bool first_ = true, known_ = false;
    std::string identity_;
    uint64_t offset_ = 0;
    uint64_t lines_ = 0, emitted_ = 0, rotations_ = 0, readErrors_ = 0, lastSummaryMs_ = 0;
};

// <Saved>/Logs/ConanSandbox.log, or TAKARO_CONAN_SERVER_LOG.
std::string ServerLogPath(const std::string& savedDir);
uint64_t ServerLogSize(const std::string& path);

}  // namespace conan
