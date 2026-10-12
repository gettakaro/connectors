// takaro.json while the server runs, and the console banners a panel user sees.
//
// Load() (startup) reads takaro.json and creates it when it is missing (unless the environment
// carries both tokens). When neither the environment nor the file sets an identity, it writes a
// generated UUID (and, when no name is set, "Conan Exiles (<first 8>)") into the file, keeping the
// rest of the text, its owner and mode. That is the only thing the connector ever writes into it.
//
// Poll() re-reads the file every pollMs on the bridge thread and compares the text. A changed text
// is applied when the next read, settleMs later, returns the same text, so a save in progress is
// never applied half-written; an unparseable text keeps the current settings. A save that empties
// the identity gets the running one written back. It returns true when the connection settings
// (URL, tokens, identity, name) or the hold state changed.
//
// After Load() every method is called from the bridge thread only.
#pragma once
#include "common.h"
#include "takaro/config.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace takaro {

// A block of lines framed by rules, in one write to the server's stdout, and in the native log.
void ConsoleBanner(const std::vector<std::string>& lines);
void ConsoleLine(const std::string& line);

// Takaro's identify / error payload as "name: message (HTTP n)" without anything else it carries
// (it can embed Takaro's internal request, including an x-takaro-token JWT). `config` redacts our
// own tokens too. nameTaken: Takaro refused because another game server already has the name.
std::string DescribeTakaroError(const JsonValue* error, const Config& config, bool* nameTaken = nullptr);

class ConfigWatcher {
public:
    struct Options {
        std::string savedDir;
        EnvFn env;
        std::function<std::string()> newIdentity;  // default NewIdentity
        int64_t pollMs = 5000, settleMs = 1000;
    };
    explicit ConfigWatcher(Options o);

    Config Load();
    bool Poll(int64_t nowMs, Config& out);
    // Console: the banner for a hold (or a "connecting" line), once per state; Remind repeats a
    // hold banner 60 s after the first (map loading buries it) and then every 15 minutes.
    void Announce(const Config& c, int64_t nowMs);
    void Remind(const Config& c, int64_t nowMs);
    void Refused(const Config& c, const std::string& why, bool nameTaken);
    void Connected(const Config& c);

    std::string HealthJson() const;
    const std::string& FilePath() const { return path_; }

private:
    // Resolves the file text; an identity neither the environment nor the file sets is written into
    // the file first (`identity`, or a new one when empty). `text` becomes what is on disk.
    Config Resolve(std::string& text, bool exists, const std::string& readError, const std::string& identity,
                   std::string writeError = "");
    void Banner(const std::string& key, const std::vector<std::string>& lines);
    void HoldBanner(const Config& c, bool force);

    Options o_;
    std::string path_;
    Config active_;
    std::optional<std::string> appliedText_, candidateText_;
    std::string lastReadError_;
    int64_t nextPoll_ = 0;
    std::string problem_;  // the banner shown last; cleared when the settings change
    bool live_ = false;
    int64_t nextReminder_ = 0, reminderGap_ = 60000;
    uint64_t reloads_ = 0, ignoredSaves_ = 0;
    bool createdFile_ = false;
    bool raced_ = false;  // the last Resolve skipped its write: the file changed under it
};

}  // namespace takaro
