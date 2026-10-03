#include "conan/logtail.h"

#include "common.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <chrono>

namespace conan {

namespace {
constexpr uint64_t kSummaryEveryMs = 10000;
}

LogTail::LogTail(LogTailOptions o) : o_(std::move(o)), split_(2000), rate_(o_.perSecond, o_.burst) {}

LogTail::~LogTail() { Stop(); }

void LogTail::Start() {
    if (o_.path.empty() || !o_.emit) return;
    thread_ = std::thread([this] {
        std::unique_lock<std::mutex> l(mu_);
        while (!stopping_) {
            l.unlock();
            Step(NowMs());
            l.lock();
            cv_.wait_for(l, std::chrono::milliseconds(o_.pollMs), [this] { return stopping_; });
        }
    });
}

void LogTail::Stop() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void LogTail::Step(uint64_t now) {
    takaro::FileStat st = takaro::StatFile(o_.path);
    if (!st.exists) {
        // Nothing there yet: whatever appears later is new, so read it from the start.
        first_ = false;
        known_ = false;
        return;
    }
    if (!known_) {
        known_ = true;
        identity_ = st.identity;
        offset_ = first_ ? st.size : 0;  // the previous run's file at load: skip its backlog
        first_ = false;
        split_.Reset();
    } else if (st.identity != identity_ || st.size < offset_) {
        identity_ = st.identity;
        offset_ = 0;
        split_.Reset();
        rotations_++;
        NativeLog("logtail: %s was rotated; following the new file from the start", o_.path.c_str());
    }
    if (st.size > offset_) {
        std::string bytes;
        uint64_t want = st.size - offset_;
        if (want > o_.maxReadBytes) want = o_.maxReadBytes;
        if (!takaro::ReadFileRange(o_.path, offset_, want, bytes)) {
            readErrors_++;
            return;
        }
        offset_ += bytes.size();
        for (auto& line : split_.Feed(bytes)) {
            lines_++;
            if (!rate_.Allow(now)) continue;
            emitted_++;
            o_.emit(events::RedactLine(line, o_.secrets));
        }
    }
    if (now - lastSummaryMs_ >= kSummaryEveryMs) {
        lastSummaryMs_ = now;
        uint64_t dropped = rate_.TakeDroppedSinceLast();
        if (dropped) {
            char buf[200];
            snprintf(buf, sizeof buf,
                     "[Takaro Conan native] %llu server log line(s) were not forwarded (rate limit %.0f lines/s)",
                     (unsigned long long)dropped, o_.perSecond);
            o_.emit(buf);
            NativeLog("logtail: %s", buf);
        }
    }
}

std::string LogTail::HealthJson() {
    return takaro::ObjBuilder()
        .S("path", o_.path)
        .N("offset", (double)offset_)
        .N("lines", (double)lines_)
        .N("forwarded", (double)emitted_)
        .N("rateLimited", (double)rate_.Dropped())
        .N("rotations", (double)rotations_)
        .N("readErrors", (double)readErrors_)
        .Done();
}

std::string ServerLogPath(const std::string& savedDir) {
    return EnvOr("TAKARO_CONAN_SERVER_LOG", savedDir.empty() ? "" : savedDir + "/Logs/ConanSandbox.log");
}

uint64_t ServerLogSize(const std::string& path) { return takaro::StatFile(path).size; }

}  // namespace conan
