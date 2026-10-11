#include "takaro/config.h"

#include "common.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace takaro {

const char* const kConfigTemplate =
    "{\n"
    "  \"_comment\": \"Paste the registration token from Takaro (a game server of type Generic) into "
    "registrationToken and save: the connector connects within a few seconds, no restart needed. Leave "
    "identityToken empty: the connector fills in a unique one. Never change it afterwards, Takaro keys "
    "this server on it.\",\n"
    "  \"url\": \"wss://connect.takaro.io/\",\n"
    "  \"registrationToken\": \"\",\n"
    "  \"identityToken\": \"\",\n"
    "  \"name\": \"\"\n"
    "}\n";

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

// A parsed JSON object, or the reason it is not one.
JsonValue ParseObject(const std::string& text, std::string& error) {
    JsonValue parsed;
    if (!ParseJson(text, parsed) || parsed.type != JsonValue::Object) {
        error = "not a JSON object";
        return JObj();
    }
    return parsed;
}

std::string FileString(const JsonValue& file, std::initializer_list<const char*> keys) {
    for (auto* k : keys) {
        const JsonValue* v = file.get(k);
        if (v && v->type == JsonValue::String && !Trim(v->str).empty()) return Trim(v->str);
    }
    return "";
}

}  // namespace

const char* SourceName(Source s) {
    switch (s) {
        case Source::Env: return "environment";
        case Source::File: return "takaro.json";
        case Source::Saved: return "saved copy";
        case Source::Current: return "current";
        case Source::Generated: return "generated";
        case Source::Default: return "default";
    }
    return "default";
}

std::string ConfigFilePath(const std::string& savedDir, const EnvFn& env) {
    std::string p = Trim(env("TAKARO_CONAN_CONFIG"));
    if (!p.empty()) return ResolvePath(savedDir, p);
    return JoinPath(JoinPath(JoinPath(savedDir, "Config"), "Takaro"), "takaro.json");
}

std::string StateDirFor(const std::string& savedDir, const EnvFn& env, const std::string& fileText) {
    std::string p = Trim(env("TAKARO_STATE_DIR"));
    if (p.empty()) {
        std::string ignored;
        p = FileString(ParseObject(fileText, ignored), {"stateDir"});
    }
    return ResolvePath(savedDir, p.empty() ? JoinPath(JoinPath(savedDir, "Takaro"), "state") : p);
}

Config ResolveConfig(const ConfigInput& in) {
    Config c;
    const EnvFn& env = in.env;
    c.configFile = ConfigFilePath(in.savedDir, env);
    JsonValue file = JObj();
    if (!in.readError.empty()) {
        c.fileError = in.readError;
    } else if (in.fileExists) {
        std::string error;
        file = ParseObject(in.fileText, error);
        if (!error.empty()) c.fileError = c.configFile + " is not valid JSON (" + error + ")";
        for (auto& kv : file.obj) {
            bool known = kv.first.compare(0, 1, "_") == 0;  // "_comment" and friends
            for (auto* k : kKnownKeys) known = known || kv.first == k;
            if (!known) c.warnings.push_back("takaro.json: unknown key '" + kv.first + "' ignored");
            if (kv.second.type != JsonValue::String && known && kv.first[0] != '_')
                c.fileError = c.configFile + ": '" + kv.first + "' must be a string";
        }
        // A file with a wrong type is not applied at all; fields of it would be guesses.
        if (!c.fileError.empty()) file = JObj();
    }
    std::string savedError;
    JsonValue saved = in.savedExists ? ParseObject(in.savedText, savedError) : JObj();
    if (!savedError.empty()) c.warnings.push_back("the saved copy is not valid JSON; ignoring it");

    auto envValue = [&](std::initializer_list<const char*> names, std::string& label) {
        for (auto* e : names) {
            std::string v = Trim(env(e));
            if (!v.empty()) {
                label = std::string("env ") + e;
                return v;
            }
        }
        return std::string();
    };
    // env, then the file (unless it holds `ignore`), then the saved copy.
    auto pick = [&](const char* label, std::initializer_list<const char*> envNames,
                    std::initializer_list<const char*> fileKeys, const char* savedKey, const std::string& ignore,
                    std::string& out, Source& src) {
        std::string from;
        std::string v = envValue(envNames, from);
        if (!v.empty()) {
            out = v;
            src = Source::Env;
            c.sources.push_back({label, from});
            return true;
        }
        for (auto* k : fileKeys) {
            v = FileString(file, {k});
            if (!v.empty() && v != ignore) {
                out = v;
                src = Source::File;
                c.sources.push_back({label, std::string("takaro.json ") + k});
                return true;
            }
        }
        if (savedKey && !(v = FileString(saved, {savedKey})).empty()) {
            out = v;
            src = Source::Saved;
            c.sources.push_back({label, "saved copy"});
            return true;
        }
        return false;
    };

    if (!pick("url", {"TAKARO_WS_URL", "TAKARO_URL"}, {"url", "takaroWsUrl"}, "url", kDefaultUrl, c.url,
              c.urlSource)) {
        c.url = kDefaultUrl;
        c.sources.push_back({"url", "default"});
    }
    if (FileString(file, {"registrationToken"}) == kPlaceholderToken)
        c.warnings.push_back(std::string("takaro.json: registrationToken still holds the example text '") +
                             kPlaceholderToken + "'; ignoring it");
    if (!pick("registrationToken", {"TAKARO_REGISTRATION_TOKEN"}, {"registrationToken"}, "registrationToken",
              kPlaceholderToken, c.registrationToken, c.registrationSource)) {
        c.registrationToken.clear();
        c.sources.push_back({"registrationToken", "not set"});
    }
    if (!pick("identityToken", {"TAKARO_IDENTITY_TOKEN"}, {"identityToken"}, "identityToken", "", c.identityToken,
              c.identitySource)) {
        if (!in.currentIdentity.empty()) {
            c.identityToken = in.currentIdentity;
            c.identitySource = Source::Current;
            c.sources.push_back({"identityToken", "current"});
        } else if (in.priorInstall || !c.fileError.empty()) {
            // Never invent an identity for a server that ran before, or from a file we could not read.
            c.identityToken.clear();
            c.sources.push_back({"identityToken", "not set"});
        } else {
            c.identityToken = in.newIdentity ? in.newIdentity() : NewIdentity();
            c.identitySource = Source::Generated;
            c.sources.push_back({"identityToken", "generated"});
        }
    }
    if (!pick("name", {"TAKARO_SERVER_NAME", "TAKARO_NAME"}, {"name", "serverName"}, "name", "", c.serverName,
              c.nameSource)) {
        if (c.identitySource == Source::Current && !in.currentName.empty()) {
            c.serverName = in.currentName;
            c.nameSource = Source::Current;
        } else if (c.identitySource == Source::Generated) {
            c.serverName = std::string(kDefaultServerName) + " (" + c.identityToken.substr(0, 8) + ")";
            c.nameSource = Source::Generated;
        } else {
            c.serverName = kDefaultServerName;
        }
        c.sources.push_back({"name", SourceName(c.nameSource)});
    }

    std::string from;
    std::string ca = envValue({"TAKARO_CA_FILE"}, from);
    if (ca.empty()) ca = FileString(file, {"caFile"});
    c.caFile = ResolvePath(in.savedDir, ca);
    c.stateDir = StateDirFor(in.savedDir, env, in.fileText);

    c.reconnectBaseMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_BASE_MS")), 2000, 100, 600000,
                                      "TAKARO_RECONNECT_BASE_MS", c.warnings);
    c.reconnectMaxMs = ParseUnsigned(Trim(env("TAKARO_RECONNECT_MAX_MS")), 60000, 100, 3600000,
                                     "TAKARO_RECONNECT_MAX_MS", c.warnings);
    if (c.reconnectMaxMs < c.reconnectBaseMs) c.reconnectMaxMs = c.reconnectBaseMs;
    c.actionTimeoutMs = ParseUnsigned(Trim(env("TAKARO_ACTION_TIMEOUT_MS")), 15000, 1000, 600000,
                                      "TAKARO_ACTION_TIMEOUT_MS", c.warnings);
    c.actionWorkers = ParseUnsigned(Trim(env("TAKARO_ACTION_WORKERS")), 2, 1, 8, "TAKARO_ACTION_WORKERS", c.warnings);

    if (Trim(env("TAKARO_CONAN_NATIVE_DISABLE")) == "1") {
        c.inert = true;
        c.disabledReason = "disabled by TAKARO_CONAN_NATIVE_DISABLE=1";
    } else if (!c.fileError.empty()) {
        c.hold = Hold::FileError;
        c.disabledReason = "config error: " + c.fileError;
    } else if (c.identityToken.empty()) {
        c.hold = Hold::NoIdentity;
        c.disabledReason = "not configured: identityToken is empty in " + c.configFile +
                           ", but this server ran the connector before (" + c.stateDir +
                           " exists); put its old identityToken back (or TAKARO_IDENTITY_TOKEN)";
    } else if (c.registrationToken.empty()) {
        c.hold = Hold::NoToken;
        c.disabledReason = "not configured: set registrationToken in " + c.configFile +
                           " (or TAKARO_REGISTRATION_TOKEN)";
    } else if (Lower(c.url.substr(0, 6)) != "wss://") {
        c.hold = Hold::BadUrl;
        c.disabledReason = "url must start with wss:// (no plaintext connection to Takaro)";
    } else {
        c.enabled = true;
    }
    return c;
}

Config LoadConfig(const std::string& savedDir, const EnvFn& env, const std::string& fileText, bool fileExists) {
    ConfigInput in;
    in.savedDir = savedDir;
    in.env = env;
    in.fileText = fileText;
    in.fileExists = fileExists;
    return ResolveConfig(in);
}

std::string RenderSaved(const Config& c, const std::string& oldSavedText, bool withRegistration) {
    std::string ignored;
    JsonValue old = ParseObject(oldSavedText, ignored);
    auto keep = [](Source s) { return s != Source::Env && s != Source::Default; };
    auto value = [&](const char* key, const std::string& inUse, Source src, bool write) {
        return write && keep(src) ? inUse : FileString(old, {key});
    };
    return ObjBuilder()
               .S("_comment",
                  "Written by the Takaro connector: the settings it last connected with, kept outside takaro.json "
                  "so a replaced takaro.json keeps this server's token and identity. Edit takaro.json instead; "
                  "a value set there wins over this file.")
               .S("url", value("url", c.url, c.urlSource, true))
               .S("identityToken", value("identityToken", c.identityToken, c.identitySource, true))
               .S("registrationToken",
                  value("registrationToken", c.registrationToken, c.registrationSource, withRegistration))
               .S("name", value("name", c.serverName, c.nameSource, true))
               .Done() +
           "\n";
}

// ------------------------------------------------------------------------------------------------ JSON edit

namespace {
size_t SkipWs(const std::string& t, size_t i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
    return i;
}
// Index just past the string literal starting at t[i] == '"', or npos.
size_t EndOfString(const std::string& t, size_t i) {
    for (i++; i < t.size(); i++) {
        if (t[i] == '\\') i++;
        else if (t[i] == '"') return i + 1;
    }
    return std::string::npos;
}
// Index just past the JSON value starting at t[i], or npos (strings, nested containers, scalars).
size_t EndOfValue(const std::string& t, size_t i) {
    if (i >= t.size()) return std::string::npos;
    if (t[i] == '"') return EndOfString(t, i);
    if (t[i] == '{' || t[i] == '[') {
        int depth = 0;
        for (; i < t.size(); i++) {
            if (t[i] == '"') {
                i = EndOfString(t, i);
                if (i == std::string::npos) return i;
                i--;
            } else if (t[i] == '{' || t[i] == '[') {
                depth++;
            } else if (t[i] == '}' || t[i] == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return std::string::npos;
    }
    while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ']' && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' &&
           t[i] != '\n')
        i++;
    return i;
}
}  // namespace

std::optional<std::string> SetJsonString(const std::string& text, const std::string& key, const std::string& value) {
    JsonValue parsed;
    if (!ParseJson(text, parsed) || parsed.type != JsonValue::Object) return std::nullopt;
    const JsonValue* existing = parsed.get(key);
    if (existing && existing->type != JsonValue::String) return std::nullopt;
    const std::string literal = JsonStr(value);
    std::string out;
    size_t i = SkipWs(text, 0);
    if (i < text.size() && text.compare(i, 3, "\xEF\xBB\xBF") == 0) i = SkipWs(text, i + 3);
    if (i >= text.size() || text[i] != '{') return std::nullopt;
    size_t lastValueEnd = std::string::npos;
    std::string memberIndent = "  ";
    for (i = SkipWs(text, i + 1); i < text.size() && text[i] == '"';) {
        size_t lineStart = text.rfind('\n', i);
        if (lineStart != std::string::npos) memberIndent = text.substr(lineStart + 1, i - lineStart - 1);
        size_t keyEnd = EndOfString(text, i);
        if (keyEnd == std::string::npos) return std::nullopt;
        JsonValue k;
        if (!ParseJson(text.substr(i, keyEnd - i), k)) return std::nullopt;
        size_t colon = SkipWs(text, keyEnd);
        if (colon >= text.size() || text[colon] != ':') return std::nullopt;
        size_t valueStart = SkipWs(text, colon + 1);
        size_t valueEnd = EndOfValue(text, valueStart);
        if (valueEnd == std::string::npos) return std::nullopt;
        if (k.str == key) {
            out = text.substr(0, valueStart) + literal + text.substr(valueEnd);
            break;
        }
        lastValueEnd = valueEnd;
        i = SkipWs(text, valueEnd);
        if (i < text.size() && text[i] == ',') i = SkipWs(text, i + 1);
    }
    if (out.empty()) {
        if (memberIndent.find_first_not_of(" \t") != std::string::npos) memberIndent = "  ";
        const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        if (lastValueEnd == std::string::npos) {
            size_t open = text.find('{');
            out = text.substr(0, open + 1) + nl + memberIndent + JsonStr(key) + ": " + literal + nl +
                  text.substr(SkipWs(text, open + 1));
        } else {
            out = text.substr(0, lastValueEnd) + "," + nl + memberIndent + JsonStr(key) + ": " + literal +
                  text.substr(lastValueEnd);
        }
    }
    JsonValue check;
    if (!ParseJson(out, check) || check.type != JsonValue::Object) return std::nullopt;
    const JsonValue* got = check.get(key);
    if (!got || got->type != JsonValue::String || got->str != value) return std::nullopt;
    return out;
}

std::string NewIdentity() {
    unsigned char b[16] = {};
    if (!RandomBytes(b, sizeof b)) {
        // Never expected; the clock still makes a collision within one domain unlikely.
        uint64_t seed = NowNs() ^ ((uint64_t)(uintptr_t)&b << 16);
        for (auto& x : b) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            x = (unsigned char)(seed >> 56);
        }
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    char out[37];
    snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
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
