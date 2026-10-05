// Native connector configuration: environment first, then takaro\plugin.json next to the server exe
// (the rented-server path). Token values never leave this struct: health and logs name the source only.
#pragma once
#include "native/mapping.h"
#include "native/persistence.h"

#include <string>
#include <vector>

namespace native {

struct NativeConfig {
    bool enabled = false;
    std::string disabledReason;  // set when !enabled

    std::string url = "wss://connect.takaro.io/";
    std::string identityToken, registrationToken;
    std::string serverName = "Takaro Dev Enshrouded";
    std::string caFile;  // resolved against the exe dir; empty = system trust
    unsigned reconnectBaseMs = 2000, reconnectMaxMs = 60000;

    LogEventsMode logEvents = LogEventsMode::Filtered;
    LogTailMode logTail = LogTailMode::Auto;
    std::string logFile;
    unsigned pollIntervalMs = 250;
    unsigned actionTimeoutMs = 30000;
    unsigned actionWorkers = 4;
    bool legacyHttp = false;
    bool logFrames = false;  // TAKARO_LOG_FRAMES=1: log outbound event/response frames (truncated; never identify)

    // Where each setting came from ("env", "plugin.json", "default"); values are never recorded.
    std::vector<std::pair<std::string, std::string>> sources;
    std::vector<std::string> warnings;
};

// pluginJson: contents of <baseDir>\takaro\plugin.json (empty when absent).
NativeConfig LoadNativeConfig(const std::string& baseDir, const EnvFn& env, const std::string& pluginJson);
std::string ConfigSummaryJson(const NativeConfig& c);  // for /health: no secrets

}  // namespace native
