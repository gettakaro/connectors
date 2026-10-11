// takaro.json while the server runs, and the console banners a panel user sees.
//
// Load() (startup, before the Store creates the state dir) reads takaro.json and the saved copy,
// creates takaro.json from the shipped template when it is missing (unless the environment carries
// both tokens), resolves the settings (core/takaro/config.h) and pins the identity: a saved,
// current or generated identity (and the generated name) is written into the file when it leaves
// them empty, and into <stateDir>/saved-settings.json with the URL. The registration token goes
// into the saved copy only after Takaro accepted it (Identified), and never into takaro.json.
//
// Poll() re-reads the file every pollMs on the bridge thread and compares the text. A changed text
// is applied when the next read, settleMs later, returns the same text, so a save in progress is
// never applied half-written; an unparseable text keeps the current settings. It returns true
// when the connection settings (URL, tokens, identity, name) or the hold state changed.
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
    // Takaro accepted an identify made with `used`: the saved copy gains its registration token
    // (only when `used` is still what runs).
    void Identified(const Config& used);

    // Console: the banner for a hold (or a "connecting" line), once per state; Remind repeats a
    // hold banner 60 s after the first (map loading buries it) and then every 15 minutes.
    void Announce(const Config& c, int64_t nowMs);
    void Remind(const Config& c, int64_t nowMs);
    void Refused(const Config& c, const std::string& why, bool nameTaken);
    void Connected(const Config& c);

    std::string HealthJson() const;
    const std::string& FilePath() const { return path_; }
    const std::string& SavedPath() const { return savedPath_; }

private:
    void Persist(const Config& c, bool withRegistration);
    void Banner(const std::string& key, const std::vector<std::string>& lines);
    void HoldBanner(const Config& c, bool force);

    Options o_;
    std::string path_, savedPath_;
    bool priorInstall_ = false;
    Config active_;
    std::optional<std::string> appliedText_, candidateText_;
    std::string lastReadError_;
    int64_t nextPoll_ = 0;
    std::string problem_;  // the banner shown last; cleared when the settings change
    bool live_ = false;
    int64_t nextReminder_ = 0, reminderGap_ = 60000;
    uint64_t reloads_ = 0, ignoredSaves_ = 0;
    bool createdFile_ = false;
};

}  // namespace takaro
