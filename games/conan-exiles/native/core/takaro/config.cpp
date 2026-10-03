#include "takaro/config.h"

#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <cstdlib>

namespace takaro {

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

const char* const kKnownKeys[] = {"url", "takaroWsUrl", "identityToken", "registrationToken", "name",
                                  "serverName", "caFile", "stateDir"};

}  // namespace

std::string ConfigFilePath(const std::string& savedDir, const EnvFn& env) {
    std::string p = Trim(env("TAKARO_CONAN_CONFIG"));
    if (!p.empty()) return ResolvePath(savedDir, p);
    return JoinPath(JoinPath(JoinPath(savedDir, "Config"), "Takaro"), "takaro.json");
}

Config LoadConfig(const std::string& savedDir, const EnvFn& env, const std::string& fileText, bool fileExists) {
    Config c;
    c.configFile = ConfigFilePath(savedDir, env);
    JsonValue file = JObj();
    std::string fileError;
    if (fileExists) {
        JsonValue parsed;
        if (!ParseJson(fileText, parsed) || parsed.type != JsonValue::Object) {
            fileError = c.configFile + " is not a JSON object";
        } else {
            file = parsed;
            for (auto& kv : file.obj) {
                bool known = kv.first.compare(0, 1, "_") == 0;  // "_comment" and friends
                for (auto* k : kKnownKeys) known = known || kv.first == k;
                if (!known) c.warnings.push_back("takaro.json: unknown key '" + kv.first + "' ignored");
                if (kv.second.type != JsonValue::String && known && kv.first[0] != '_')
                    fileError = c.configFile + ": '" + kv.first + "' must be a string";
            }
        }
    }
    auto pick = [&](const char* label, std::initializer_list<const char*> envNames,
                    std::initializer_list<const char*> fileKeys, const std::string& fallback) {
        for (auto* e : envNames) {
            std::string v = Trim(env(e));
            if (!v.empty()) {
                c.sources.push_back({label, std::string("env ") + e});
                return v;
            }
        }
        for (auto* k : fileKeys) {
            const JsonValue* v = file.get(k);
            if (v && v->type == JsonValue::String && !Trim(v->str).empty()) {
                c.sources.push_back({label, std::string("takaro.json ") + k});
                return Trim(v->str);
            }
        }
        c.sources.push_back({label, "default"});
        return fallback;
    };

    c.url = pick("url", {"TAKARO_WS_URL", "TAKARO_URL"}, {"url", "takaroWsUrl"}, c.url);
    c.identityToken = pick("identityToken", {"TAKARO_IDENTITY_TOKEN"}, {"identityToken"}, "");
    c.registrationToken = pick("registrationToken", {"TAKARO_REGISTRATION_TOKEN"}, {"registrationToken"}, "");
    c.serverName = pick("name", {"TAKARO_SERVER_NAME", "TAKARO_NAME"}, {"name", "serverName"}, c.serverName);
    c.caFile = ResolvePath(savedDir, pick("caFile", {"TAKARO_CA_FILE"}, {"caFile"}, ""));
    c.stateDir = ResolvePath(savedDir, pick("stateDir", {"TAKARO_STATE_DIR"}, {"stateDir"},
                                            JoinPath(JoinPath(savedDir, "Takaro"), "state")));

    c.reconnectBaseMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_BASE_MS")), 2000, 100, 600000,
                                      "TAKARO_RECONNECT_BASE_MS", c.warnings);
    c.reconnectMaxMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_MAX_MS")), 60000, 100, 3600000,
                                     "TAKARO_RECONNECT_MAX_MS", c.warnings);
    if (c.reconnectMaxMs < c.reconnectBaseMs) c.reconnectMaxMs = c.reconnectBaseMs;
    c.actionTimeoutMs = ParseUnsigned(Trim(env("TAKARO_ACTION_TIMEOUT_MS")), 15000, 1000, 600000,
                                      "TAKARO_ACTION_TIMEOUT_MS", c.warnings);
    c.actionWorkers = ParseUnsigned(Trim(env("TAKARO_ACTION_WORKERS")), 2, 1, 8, "TAKARO_ACTION_WORKERS", c.warnings);

    if (Trim(env("TAKARO_CONAN_NATIVE_DISABLE")) == "1") {
        c.disabledReason = "disabled by TAKARO_CONAN_NATIVE_DISABLE=1";
    } else if (!fileError.empty()) {
        c.disabledReason = "config error (failing closed): " + fileError;
    } else if (c.identityToken.empty() || c.registrationToken.empty()) {
        std::string missing = c.identityToken.empty() ? "identityToken" : "";
        if (c.registrationToken.empty()) missing += std::string(missing.empty() ? "" : " and ") + "registrationToken";
        c.disabledReason = "not configured: set " + missing + " in " + c.configFile +
                           " (or TAKARO_IDENTITY_TOKEN / TAKARO_REGISTRATION_TOKEN)";
    } else if (Lower(c.url.substr(0, 6)) != "wss://") {
        c.disabledReason = "url must start with wss:// (no plaintext connection to Takaro)";
    } else {
        c.enabled = true;
    }
    return c;
}

std::string Config::Redact(std::string text) const {
    for (const std::string* secret : {&identityToken, &registrationToken}) {
        if (secret->size() < 4) continue;
        for (size_t at = 0; (at = text.find(*secret, at)) != std::string::npos;) {
            text.replace(at, secret->size(), "[redacted]");
            at += 10;
        }
    }
    return text;
}

std::string ConfigSummaryJson(const Config& c) {
    std::string sources = "{";
    for (size_t i = 0; i < c.sources.size(); i++)
        sources += (i ? "," : "") + JsonStr(c.sources[i].first) + ":" + JsonStr(c.sources[i].second);
    sources += "}";
    std::string warnings = "[";
    for (size_t i = 0; i < c.warnings.size(); i++) warnings += (i ? "," : "") + JsonStr(c.warnings[i]);
    warnings += "]";
    return ObjBuilder()
        .B("enabled", c.enabled)
        .S("disabledReason", c.disabledReason)
        .S("url", c.Redact(c.url))
        .S("serverName", c.serverName)
        .S("caFile", c.caFile)
        .S("stateDir", c.stateDir)
        .S("configFile", c.configFile)
        .B("identityTokenSet", !c.identityToken.empty())
        .B("registrationTokenSet", !c.registrationToken.empty())
        .N("reconnectBaseMs", c.reconnectBaseMs)
        .N("reconnectMaxMs", c.reconnectMaxMs)
        .N("actionTimeoutMs", c.actionTimeoutMs)
        .N("actionWorkers", c.actionWorkers)
        .Raw("sources", sources)
        .Raw("warnings", warnings)
        .Done();
}

}  // namespace takaro
