#include "native/config.h"

#include "native/fileio.h"
#include "native/json_util.h"

#include <cstdlib>

namespace native {

namespace {

unsigned ParseUnsigned(const std::string& text, unsigned fallback, unsigned lo, unsigned hi, const char* name,
                       std::vector<std::string>& warnings) {
    if (text.empty()) return fallback;
    char* end = nullptr;
    unsigned long v = strtoul(text.c_str(), &end, 10);
    if (!end || *end || v < lo || v > hi) {
        warnings.push_back(std::string(name) + " must be " + std::to_string(lo) + ".." + std::to_string(hi) +
                           "; using " + std::to_string(fallback));
        return fallback;
    }
    return (unsigned)v;
}

}  // namespace

NativeConfig LoadNativeConfig(const std::string& baseDir, const EnvFn& env, const std::string& pluginJson) {
    NativeConfig c;
    JsonValue file = JObj();
    if (!pluginJson.empty()) {
        JsonValue parsed;
        if (ParseJson(pluginJson, parsed) && parsed.type == JsonValue::Object) file = parsed;
        else c.warnings.push_back("takaro\\plugin.json is not a JSON object; ignoring it");
    }
    // env first, then plugin.json; blank values count as unset
    auto pick = [&](const char* label, std::initializer_list<const char*> envNames, const char* fileKey,
                    const std::string& fallback) {
        for (auto* e : envNames) {
            std::string v = Trim(env(e));
            if (!v.empty()) {
                c.sources.push_back({label, std::string("env ") + e});
                return v;
            }
        }
        if (fileKey) {
            const JsonValue* v = file.get(fileKey);
            if (v && v->type == JsonValue::String && !Trim(v->str).empty()) {
                c.sources.push_back({label, std::string("plugin.json ") + fileKey});
                return Trim(v->str);
            }
        }
        c.sources.push_back({label, "default"});
        return fallback;
    };

    c.url = pick("url", {"TAKARO_WS_URL", "TAKARO_URL"}, "url", c.url);
    c.identityToken = pick("identityToken", {"TAKARO_IDENTITY_TOKEN"}, "identityToken", "");
    c.registrationToken = pick("registrationToken", {"TAKARO_REGISTRATION_TOKEN"}, "registrationToken", "");
    c.serverName = pick("name", {"TAKARO_SERVER_NAME", "TAKARO_NAME"}, "name", c.serverName);
    std::string ca = pick("caFile", {"TAKARO_CA_FILE"}, "caFile", "");
    c.caFile = ResolvePath(baseDir, ca);

    c.reconnectBaseMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_BASE_MS")), 2000, 500, 600000, "TAKARO_RECONNECT_BASE_MS", c.warnings);
    c.reconnectMaxMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_MAX_MS")), 60000, 500, 3600000, "TAKARO_RECONNECT_MAX_MS", c.warnings);
    if (c.reconnectMaxMs < c.reconnectBaseMs) c.reconnectMaxMs = c.reconnectBaseMs;
    c.pollIntervalMs = ParseUnsigned(Trim(env("TAKARO_POLL_INTERVAL_MS")), 250, 50, 60000, "TAKARO_POLL_INTERVAL_MS", c.warnings);
    c.actionTimeoutMs = ParseUnsigned(Trim(env("TAKARO_ACTION_TIMEOUT_MS")), 30000, 1000, 600000, "TAKARO_ACTION_TIMEOUT_MS", c.warnings);
    c.actionWorkers = ParseUnsigned(Trim(env("TAKARO_ACTION_WORKERS")), 4, 1, 16, "TAKARO_ACTION_WORKERS", c.warnings);

    std::string le = Trim(env("ENSHROUDED_LOG_EVENTS"));
    if (!ParseLogEventsMode(le, c.logEvents))
        c.warnings.push_back("ENSHROUDED_LOG_EVENTS must be all|filtered|none, got '" + le + "'; using filtered");
    std::string lt = Trim(env("ENSHROUDED_LOG_TAIL"));
    if (!ParseLogTailMode(lt, c.logTail))
        c.warnings.push_back("ENSHROUDED_LOG_TAIL must be auto|always|never, got '" + lt + "'; using auto");
    std::string lf = Trim(env("ENSHROUDED_LOG_FILE"));
    c.logFile = lf.empty() ? JoinPath(JoinPath(baseDir, "logs"), "enshrouded_server.log") : ResolvePath(baseDir, lf);
    c.legacyHttp = Trim(env("TAKARO_LEGACY_HTTP")) == "1";

    if (Trim(env("TAKARO_NATIVE_DISABLE")) == "1") {
        c.disabledReason = "disabled by TAKARO_NATIVE_DISABLE=1";
    } else if (c.identityToken.empty() || c.registrationToken.empty()) {
        std::string missing = c.identityToken.empty() ? "identityToken" : "";
        if (c.registrationToken.empty()) missing += std::string(missing.empty() ? "" : " and ") + "registrationToken";
        c.disabledReason = "not configured: set " + missing +
                           " in takaro\\plugin.json next to enshrouded_server.exe (or TAKARO_IDENTITY_TOKEN / "
                           "TAKARO_REGISTRATION_TOKEN)";
    } else if (c.url.compare(0, 6, "wss://") != 0) {
        c.disabledReason = "url must start with wss:// (no plaintext connection to Takaro)";
    } else {
        c.enabled = true;
    }
    return c;
}

std::string ConfigSummaryJson(const NativeConfig& c) {
    std::string sources = "{";
    for (size_t i = 0; i < c.sources.size(); i++)
        sources += (i ? "," : "") + JsonStr(c.sources[i].first) + ":" + JsonStr(c.sources[i].second);
    sources += "}";
    std::string warnings = "[";
    for (size_t i = 0; i < c.warnings.size(); i++) warnings += (i ? "," : "") + JsonStr(c.warnings[i]);
    warnings += "]";
    const char* le = c.logEvents == LogEventsMode::All ? "all" : c.logEvents == LogEventsMode::None ? "none" : "filtered";
    const char* lt = c.logTail == LogTailMode::Always ? "always" : c.logTail == LogTailMode::Never ? "never" : "auto";
    return ObjBuilder()
        .S("url", c.url)
        .S("serverName", c.serverName)
        .S("caFile", c.caFile)
        .B("identityTokenSet", !c.identityToken.empty())
        .B("registrationTokenSet", !c.registrationToken.empty())
        .N("reconnectBaseMs", c.reconnectBaseMs)
        .N("reconnectMaxMs", c.reconnectMaxMs)
        .N("pollIntervalMs", c.pollIntervalMs)
        .N("actionTimeoutMs", c.actionTimeoutMs)
        .N("actionWorkers", c.actionWorkers)
        .S("logEvents", le)
        .S("logTail", lt)
        .S("logFile", c.logFile)
        .B("legacyHttp", c.legacyHttp)
        .Raw("sources", sources)
        .Raw("warnings", warnings)
        .Done();
}

}  // namespace native
