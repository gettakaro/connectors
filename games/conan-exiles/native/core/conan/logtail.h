// The `log` event: a worker-thread tail of ConanSandbox/Saved/Logs/ConanSandbox.log (S4: no GLog
// hook, which would need two more signature pins and run on the logging thread). Zero game-thread
// cost; S4 measured the file lag at p50 114 ms, p90 201 ms.
//
// Rotation: the engine renames the file to ConanSandbox-backup-<date>.log at server start and opens
// a new one. The tail follows the path and restarts at offset 0 when the file identity (inode)
// changes or the size shrinks. The file that exists when the library loads is the previous run's,
// so the first observation starts at its end; a file created later is read from the beginning.
//
// Every line is redacted (config tokens, password=/token= values) and rate limited below hosted
// Takaro's log budget (50 per 5 s and 300 per 30 s per game server; above it Takaro drops lines and
// stores event-rate-limited rows): at most 40 lines in any 5 s and 250 in any 30 s. Dropped lines
// are counted and summarised in one line every 10 s (the summary counts against the same budget).
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
    uint64_t shortMs = 5000, longMs = 30000;
    size_t shortMax = 40, longMax = 250;
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
    events::WindowLimiter rate_;
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
