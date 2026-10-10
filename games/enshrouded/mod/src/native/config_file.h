// The connection settings that apply without a restart: Takaro URL, registration token, identity and
// server name. They come from the environment, takaro\plugin.json next to enshrouded_server.exe, and
// the connector's own saved copy in the state folder (which an upgrade never replaces), per field in
// that order. ConfigWatcher re-reads plugin.json while the server runs; the caller polls it from one
// thread. Pure apart from fileio, so the host tests drive it on real files.
#pragma once
#include "native/persistence.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace native {

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";
// What an install that ran an older connector used: the server name when none was set, and the
// identity the shipped plugin.json.example carried.
constexpr const char* kLegacyServerName = "Takaro Dev Enshrouded";
constexpr const char* kLegacyIdentity = "my-enshrouded-server";
constexpr const char* kSavedFileName = "saved-settings.json";

enum class Source { Env, File, Saved, Current, Legacy, Generated, Default };
const char* SourceName(Source s);

struct LiveSettings {
    std::string url, registrationToken, identityToken, serverName;
    bool operator==(const LiveSettings& o) const {
        return url == o.url && registrationToken == o.registrationToken && identityToken == o.identityToken &&
               serverName == o.serverName;
    }
    bool operator!=(const LiveSettings& o) const { return !(*this == o); }
};

struct LiveResolution {
    LiveSettings s;
    Source url = Source::Default, registration = Source::Default, identity = Source::Default,
           name = Source::Default;
};

// One parsed settings file. found=false when the file is absent; ok=false when it is not a JSON object.
struct SettingsFile {
    bool found = false, ok = false;
    JsonValue obj;
    std::string String(const char* key) const;  // trimmed string value, "" when absent or not a string
    bool HasValue(const char* key) const { return !String(key).empty(); }
};
SettingsFile ParseSettingsFile(bool found, const std::string& text);  // tolerates a UTF-8 BOM

struct ResolveInputs {
    const SettingsFile* user = nullptr;
    const SettingsFile* saved = nullptr;
    // The state folder existed before this start and holds no saved copy: an install that ran a
    // connector from before the saved copy existed.
    bool legacyInstall = false;
    std::string currentIdentity;  // the identity this run already uses ("" at startup)
    std::string currentName;      // the generated name this run already uses ("" when none)
    std::string hostName;         // "name" from enshrouded_server.json, for a generated server name
};

// Per field: a non-empty environment variable, else plugin.json, else the saved copy.
//  - url from plugin.json only when it differs from the default, so a freshly shipped file cannot
//    undo a saved custom URL;
//  - identity then falls back to the one this run uses, then kLegacyIdentity for a legacy install,
//    then a new UUID;
//  - name then falls back to kLegacyServerName when the identity was chosen by the operator or a
//    legacy install, else "<hostName or Enshrouded> (<first 8 identity characters>)": Takaro keeps
//    game-server names unique per domain, so a second fresh install must not reuse a shared name.
LiveResolution ResolveLive(const ResolveInputs& in, const EnvFn& env, const std::function<std::string()>& newIdentity);

// What the connector must not connect without: "" when the settings can connect.
std::string ConnectBlocker(const LiveSettings& s);

// plugin.json text with `key` set (or added) to a string value, as a 2-space indented object.
std::string SetJsonKeys(const SettingsFile& file, const std::vector<std::pair<std::string, std::string>>& keys);
// The text of a fresh plugin.json carrying the identity and name in use.
std::string FreshPluginJson(const LiveSettings& s);

// The saved copy: url / identityToken / name in use when they did not come from the environment
// or the default, and the registration token only when `withRegistration` (Takaro accepted it).
// A value the environment supplies keeps what the old saved copy had.
std::string RenderSaved(const LiveResolution& r, const SettingsFile& oldSaved, bool withRegistration);

// A random UUID (version 4) from 16 random bytes.
std::string UuidFromBytes(const unsigned char b[16]);

// Re-reads plugin.json and resolves the live settings. Not thread-safe: one thread calls Start and
// Poll; MarkAccepted may be called from any thread.
class ConfigWatcher {
public:
    struct Options {
        std::string userPath, savedPath;
        bool legacyInstall = false;
        std::string hostName;
        EnvFn env;
        std::function<std::string()> newIdentity;
        int64_t pollMs = 5000, settleMs = 1000;
        std::function<void(const std::string&)> log;  // plugin.log only
    };
    explicit ConfigWatcher(Options o);

    // First resolution at startup; pins the identity in plugin.json and the saved copy.
    const LiveResolution& Start(int64_t nowMs);
    // Re-reads plugin.json when due. A text that differs from the applied one is taken only when the
    // next read, settleMs later, returns the same text, and only when it parses; an unreadable or
    // half-saved file keeps the current settings. True when the live settings changed.
    bool Poll(int64_t nowMs);
    const LiveResolution& Active() const { return active_; }
    // Warnings meant for the server console (an unparseable plugin.json), each returned once.
    std::vector<std::string> TakeConsoleWarnings();
    // Takaro accepted the current settings: the next Poll stores the registration token in the saved copy.
    void MarkAccepted(const LiveSettings& accepted);

private:
    LiveResolution Resolve(const SettingsFile& user, const SettingsFile& saved) const;
    void Persist(const SettingsFile& user);
    SettingsFile ReadSaved() const;
    void Log(const std::string& line) const;

    Options o_;
    LiveResolution active_;
    SettingsFile appliedUser_;
    bool appliedFound_ = false;
    std::string appliedText_;
    bool haveCandidate_ = false, candidateFound_ = false;
    std::string candidateText_;
    int64_t nextPoll_ = 0;
    std::vector<std::string> consoleWarnings_;
    std::string acceptedRegistration_;  // the registration token Takaro accepted with the current settings
    std::mutex acceptedMu_;
    bool acceptedPending_ = false;
    LiveSettings accepted_;
};

}  // namespace native
