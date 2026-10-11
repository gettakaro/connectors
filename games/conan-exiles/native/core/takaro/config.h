// Native connector configuration: environment first, then ConanSandbox/Saved/Config/Takaro/takaro.json
// (the rented-server path; TAKARO_CONAN_CONFIG moves it), then the connector's own saved copy
// (<stateDir>/saved-settings.json). URL, registration token, identity and name apply without a
// restart (core/takaro/config_watch.h); everything else is read at startup.
//
// Holding vs inert: a missing token, an unreadable or unparseable file or a non-wss URL leaves the
// connector loaded but not connected (`enabled` false, `disabledReason` says why) until the file is
// fixed and saved. Only TAKARO_CONAN_NATIVE_DISABLE=1 makes it inert (`inert`).
// Token values never leave this struct: logs and health name the source of each setting only, and
// Redact() scrubs them from any text that might echo them (Takaro error payloads, peer messages).
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace takaro {

using EnvFn = std::function<std::string(const char*)>;

constexpr const char* kDefaultUrl = "wss://connect.takaro.io/";
constexpr const char* kDefaultServerName = "Conan Exiles";
// What the takaro.json.example of 3.2.0 and older shipped as the token: never a real token.
constexpr const char* kPlaceholderToken = "paste-your-registration-token-here";
constexpr const char* kSavedFileName = "saved-settings.json";
// The takaro.json the release ships (native/takaro.json, checked by tests/unit_test.cpp) and the
// connector writes when the file is missing.
extern const char* const kConfigTemplate;

enum class Source { Env, File, Saved, Current, Generated, Default };
const char* SourceName(Source s);

// The settings that reconnect when they change.
struct Settings {
    std::string url, identityToken, registrationToken, serverName;
    bool operator==(const Settings& o) const {
        return url == o.url && identityToken == o.identityToken && registrationToken == o.registrationToken &&
               serverName == o.serverName;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

enum class Hold { None, FileError, NoIdentity, NoToken, BadUrl };

struct Config {
    bool inert = false;  // TAKARO_CONAN_NATIVE_DISABLE=1: nothing is installed, nothing connects
    bool enabled = false;
    Hold hold = Hold::None;
    std::string disabledReason;  // set when !enabled
    std::string fileError;       // takaro.json could not be read or parsed (empty: fine or absent)

    std::string url = kDefaultUrl;
    std::string identityToken, registrationToken;
    std::string serverName = kDefaultServerName;
    Source urlSource = Source::Default, identitySource = Source::Default, registrationSource = Source::Default,
           nameSource = Source::Default;
    std::string caFile;    // empty = the platform default (Linux: next to the library, then the system)
    std::string stateDir;  // durable outbox + saved copy; default <Saved>/Takaro/state
    unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;
    unsigned actionTimeoutMs = 15000;
    unsigned actionWorkers = 2;

    std::string configFile;  // the takaro.json path that was consulted
    // Where each setting came from ("env NAME", "takaro.json key", "default"); never values.
    std::vector<std::pair<std::string, std::string>> sources;
    std::vector<std::string> warnings;

    Settings Connection() const { return {url, identityToken, registrationToken, serverName}; }
    std::string Redact(std::string text) const;
};

struct ConfigInput {
    std::string savedDir;  // <server>/ConanSandbox/Saved
    EnvFn env;
    std::string fileText;  // takaro.json as read by the caller (this stays free of I/O)
    bool fileExists = false;
    std::string readError;  // set when takaro.json exists but could not be read
    std::string savedText;  // the saved copy
    bool savedExists = false;
    // The identity and name this process already runs with (empty at startup).
    std::string currentIdentity, currentName;
    // The state dir existed before this process: an older connector ran here. A missing identity is
    // then never replaced by a new one (that would orphan the Takaro server record).
    bool priorInstall = false;
    std::function<std::string()> newIdentity;  // default: NewIdentity()
};

// TAKARO_CONAN_CONFIG or <savedDir>/Config/Takaro/takaro.json.
std::string ConfigFilePath(const std::string& savedDir, const EnvFn& env);
// The state dir from the environment, takaro.json `stateDir`, or <savedDir>/Takaro/state.
std::string StateDirFor(const std::string& savedDir, const EnvFn& env, const std::string& fileText);

// Per field: a non-empty environment variable, then takaro.json, then the saved copy. The URL and
// name are taken from takaro.json only when they differ from the defaults, so a freshly shipped
// file cannot undo saved values. Identity then falls back to the current one, and only on a
// fresh install (no prior state) to a generated UUID; the name of a generated identity is
// "Conan Exiles (<first 8 identity characters>)" (Takaro server names are unique per domain).
Config ResolveConfig(const ConfigInput& in);
// Startup-only convenience used by the host harness: no saved copy, no prior install.
Config LoadConfig(const std::string& savedDir, const EnvFn& env, const std::string& fileText, bool fileExists);

// The saved copy to write: URL, identity, name and (only when `withRegistration`) the registration
// token in use. Values from the environment or defaults are not written; the old saved value stays.
std::string RenderSaved(const Config& c, const std::string& oldSavedText, bool withRegistration);

// `text` (a JSON object) with top-level `key` set to the string `value`: the existing value is
// replaced in place, or the key is appended; all other text is kept byte for byte. nullopt when
// `text` is not a JSON object or `key` holds a non-string.
std::optional<std::string> SetJsonString(const std::string& text, const std::string& key, const std::string& value);

std::string NewIdentity();  // random UUID v4
std::string ConfigSummaryJson(const Config& c);  // for health and logs: no secrets

}  // namespace takaro
