// Native connector configuration: environment first, then ConanSandbox/Saved/Config/Takaro/takaro.json
// (the rented-server path; TAKARO_CONAN_CONFIG moves it). Fails closed: missing tokens, a non-wss
// URL or a takaro.json that exists but does not parse leave the connector disabled with a reason.
// Token values never leave this struct: logs and health name the source of each setting only, and
// Redact() scrubs them from any text that might echo them (Takaro error payloads, peer messages).
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace takaro {

using EnvFn = std::function<std::string(const char*)>;

struct Config {
    bool enabled = false;
    std::string disabledReason;  // set when !enabled

    std::string url = "wss://connect.takaro.io/";
    std::string identityToken, registrationToken;
    std::string serverName = "Conan Exiles";
    std::string caFile;    // empty = the first system CA bundle found (Linux transport)
    std::string stateDir;  // durable outbox; default <Saved>/Takaro/state
    unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;
    unsigned actionTimeoutMs = 15000;
    unsigned actionWorkers = 2;

    std::string configFile;  // the takaro.json path that was consulted
    // Where each setting came from ("env NAME", "takaro.json key", "default"); never values.
    std::vector<std::pair<std::string, std::string>> sources;
    std::vector<std::string> warnings;

    std::string Redact(std::string text) const;
};

// savedDir: <server>/ConanSandbox/Saved. fileText/fileExists: contents of the config file (read by
// the caller, so this stays free of I/O). The file path is TAKARO_CONAN_CONFIG or
// <savedDir>/Config/Takaro/takaro.json; ConfigFilePath() returns it.
std::string ConfigFilePath(const std::string& savedDir, const EnvFn& env);
Config LoadConfig(const std::string& savedDir, const EnvFn& env, const std::string& fileText, bool fileExists);
std::string ConfigSummaryJson(const Config& c);  // for health and logs: no secrets

}  // namespace takaro
