// The connection settings that apply without a restart: Takaro URL, registration token, identity and
// server name. takaro\plugin.json next to enshrouded_server.exe is the one config file; a non-empty
// environment variable wins per field and is never written to disk. ConfigWatcher creates the file
// when it is missing, writes a generated identity into it when its identity is empty, and re-reads it
// while the server runs; the caller polls it from one thread. Pure apart from fileio, so the host
// tests drive it on real files.
#pragma once
#include "native/persistence.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace native {

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";

enum class Source { Env, File, Default };
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

// Per field: a non-empty environment variable, else plugin.json, else the default (the Takaro URL; no
// tokens; for the name "<hostName or Enshrouded> (<first 8 identity characters>)", because Takaro keeps
// game-server names unique per domain).
LiveResolution ResolveLive(const SettingsFile& file, const std::string& hostName, const EnvFn& env);
std::string DefaultServerName(const std::string& hostName, const std::string& identity);

// What the connector must not connect without: "" when the settings can connect.
std::string ConnectBlocker(const LiveSettings& s);

// plugin.json text with `key` set (or added) to a string value, as a 2-space indented object.
std::string SetJsonKey(const SettingsFile& file, const std::string& key, const std::string& value);
// The text of the plugin.json the connector creates: every key, empty tokens, the identity and name.
std::string FreshPluginJson(const std::string& identity, const std::string& name);

// A random UUID (version 4) from 16 random bytes.
std::string UuidFromBytes(const unsigned char b[16]);

// Reads plugin.json at Start and again every pollMs. Not thread-safe: one thread calls Start and Poll.
class ConfigWatcher {
public:
    struct Options {
        std::string path;
        std::string hostName;  // "name" from enshrouded_server.json, for the default server name
        EnvFn env;
        std::function<std::string()> newIdentity;
        int64_t pollMs = 5000, settleMs = 1000;
        std::function<void(const std::string&)> log;  // plugin.log only
    };
    explicit ConfigWatcher(Options o);

    // Creates plugin.json when it is missing and resolves the settings.
    const LiveResolution& Start(int64_t nowMs);
    // Re-reads plugin.json when due. A text that differs from the applied one is taken only when the
    // next read, settleMs later, returns the same text, and only when it parses; an unreadable,
    // half-saved or removed file keeps the current settings. True when the live settings changed.
    bool Poll(int64_t nowMs);
    const LiveResolution& Active() const { return active_; }
    // Warnings meant for the server console, each returned once.
    std::vector<std::string> TakeConsoleWarnings();

private:
    // Resolves `text`; an empty identityToken in the file gets a generated one, written back.
    void Apply(bool found, const std::string& text);
    void Log(const std::string& line) const;

    Options o_;
    LiveResolution active_;
    bool appliedFound_ = false;
    std::string appliedText_;
    bool haveCandidate_ = false;
    std::string candidateText_;
    int64_t nextPoll_ = 0;
    std::vector<std::string> consoleWarnings_;
    std::string writeError_;  // the last identity write-back failure, reported once
};

}  // namespace native
