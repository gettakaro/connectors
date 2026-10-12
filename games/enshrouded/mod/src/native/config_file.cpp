#include "native/config_file.h"

#include "native/fileio.h"
#include "native/json_util.h"

#include <cctype>
#include <cstdio>

namespace native {

const char* SourceName(Source s) {
    switch (s) {
        case Source::Env: return "environment";
        case Source::File: return "plugin.json";
        case Source::Default: return "default";
    }
    return "default";
}

std::string SettingsFile::String(const char* key) const {
    if (!ok) return "";
    const JsonValue* v = obj.get(key);
    return v && v->type == JsonValue::String ? Trim(v->str) : "";
}

SettingsFile ParseSettingsFile(bool found, const std::string& text) {
    SettingsFile f;
    f.found = found;
    if (!found) return f;
    std::string body = text.rfind("\xEF\xBB\xBF", 0) == 0 ? text.substr(3) : text;  // Notepad's UTF-8 BOM
    JsonValue v;
    if (ParseJson(body, v) && v.type == JsonValue::Object) {
        f.ok = true;
        f.obj = std::move(v);
    }
    return f;
}

namespace {

std::string EnvValue(const EnvFn& env, std::initializer_list<const char*> names) {
    for (auto* n : names) {
        std::string v = Trim(env(n));
        if (!v.empty()) return v;
    }
    return "";
}

std::string PrettyObject(const JsonValue& obj) {
    if (obj.obj.empty()) return "{}\n";
    std::string out = "{\n";
    for (size_t i = 0; i < obj.obj.size(); i++)
        out += "  " + JsonStr(obj.obj[i].first) + ": " + JsonDump(obj.obj[i].second) + (i + 1 < obj.obj.size() ? ",\n" : "\n");
    return out + "}\n";
}

}  // namespace

std::string DefaultServerName(const std::string& hostName, const std::string& identity) {
    std::string id8;
    for (char c : identity)
        if (isalnum((unsigned char)c) && id8.size() < 8) id8 += c;
    std::string base = Trim(hostName).empty() ? "Enshrouded" : Trim(hostName);
    return base + " (" + id8 + ")";
}

LiveResolution ResolveLive(const SettingsFile& file, const std::string& hostName, const EnvFn& env) {
    LiveResolution r;
    auto pick = [&](std::initializer_list<const char*> envNames, const char* key, std::string& out, Source& src) {
        if (!(out = EnvValue(env, envNames)).empty()) src = Source::Env;
        else if (!(out = file.String(key)).empty()) src = Source::File;
        else src = Source::Default;
    };
    pick({"TAKARO_WS_URL", "TAKARO_URL"}, "url", r.s.url, r.url);
    if (r.s.url.empty()) r.s.url = kDefaultUrl;
    pick({"TAKARO_REGISTRATION_TOKEN"}, "registrationToken", r.s.registrationToken, r.registration);
    pick({"TAKARO_IDENTITY_TOKEN"}, "identityToken", r.s.identityToken, r.identity);
    pick({"TAKARO_SERVER_NAME", "TAKARO_NAME"}, "name", r.s.serverName, r.name);
    if (r.s.serverName.empty() && !r.s.identityToken.empty())
        r.s.serverName = DefaultServerName(hostName, r.s.identityToken);
    return r;
}

std::string ConnectBlocker(const LiveSettings& s) {
    if (s.registrationToken.empty()) return "no registration token";
    if (s.identityToken.empty()) return "no identityToken";
    if (s.url.compare(0, 6, "wss://") != 0) return "url must start with wss:// (no plaintext connection to Takaro)";
    return "";
}

std::string SetJsonKey(const SettingsFile& file, const std::string& key, const std::string& value) {
    JsonValue obj = file.ok ? file.obj : JObj();
    Put(obj, key, JStr(value));
    return PrettyObject(obj);
}

std::string FreshPluginJson(const std::string& identity, const std::string& name) {
    JsonValue obj = JObj();
    Put(obj, "registrationToken", JStr(""));
    Put(obj, "identityToken", JStr(identity));
    Put(obj, "name", JStr(name));
    Put(obj, "url", JStr(kDefaultUrl));
    Put(obj, "token", JStr(""));
    return PrettyObject(obj);
}

std::string UuidFromBytes(const unsigned char in[16]) {
    unsigned char b[16];
    for (int i = 0; i < 16; i++) b[i] = in[i];
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    char out[37];
    snprintf(out, sizeof out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
}

// ------------------------------------------------------------------------------------------------ watcher

ConfigWatcher::ConfigWatcher(Options o) : o_(std::move(o)) {}

void ConfigWatcher::Log(const std::string& line) const {
    if (o_.log) o_.log(line);
}

std::vector<std::string> ConfigWatcher::TakeConsoleWarnings() {
    std::vector<std::string> out;
    out.swap(consoleWarnings_);
    return out;
}

const LiveResolution& ConfigWatcher::Start(int64_t nowMs) {
    nextPoll_ = nowMs + o_.pollMs;
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(o_.path, text, exists, err)) {
        consoleWarnings_.push_back("cannot read " + o_.path + " (" + err + ").");
        Apply(false, "");
        return active_;
    }
    if (!exists) {
        std::string id = o_.newIdentity();
        std::string fresh = FreshPluginJson(id, DefaultServerName(o_.hostName, id));
        if (EnsureDirectory(DirName(o_.path), err) && AtomicWriteFile(o_.path, fresh, err)) {
            Log("config: created " + o_.path);
            exists = true;
            text = fresh;
        } else {
            consoleWarnings_.push_back("could not create " + o_.path + " (" + err + ").");
        }
    }
    if (exists && !ParseSettingsFile(true, text).ok) {
        consoleWarnings_.push_back(o_.path + " is not valid JSON, so it is ignored. Fix it and save it; "
                                   "the connector reads it again within a few seconds.");
        Apply(false, "");
        appliedFound_ = true;
        appliedText_ = text;
        return active_;
    }
    Apply(exists, text);
    return active_;
}

void ConfigWatcher::Apply(bool found, const std::string& text) {
    appliedFound_ = found;
    appliedText_ = text;
    SettingsFile file = ParseSettingsFile(found, text);
    active_ = ResolveLive(file, o_.hostName, o_.env);
    if (!file.ok || active_.identity == Source::Env || file.HasValue("identityToken")) return;
    std::string next = SetJsonKey(file, "identityToken", o_.newIdentity()), err;
    if (RewriteKeepingAccess(o_.path, text, next, err)) {
        Log("config: wrote a generated identityToken into " + o_.path);
        appliedText_ = next;
        writeError_.clear();
        active_ = ResolveLive(ParseSettingsFile(true, next), o_.hostName, o_.env);
        return;
    }
    appliedFound_ = false;  // not applied for good: the next poll reads the file and tries again
    appliedText_.clear();
    if (writeError_ != err)
        consoleWarnings_.push_back("could not write a generated identityToken into " + o_.path + " (" + err +
                                   "). Set \"identityToken\" there to any unique value, for example a UUID.");
    writeError_ = err;
}

bool ConfigWatcher::Poll(int64_t nowMs) {
    if (nowMs < nextPoll_) return false;
    nextPoll_ = nowMs + o_.pollMs;
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(o_.path, text, exists, err)) return false;  // unreadable right now: try again later
    if (exists == appliedFound_ && text == appliedText_) {
        haveCandidate_ = false;
        return false;
    }
    if (!exists) {
        // Removed (or mid-replace by an editor): keep what runs; a file that comes back is a change.
        appliedFound_ = false;
        appliedText_.clear();
        haveCandidate_ = false;
        return false;
    }
    if (!haveCandidate_ || candidateText_ != text) {
        haveCandidate_ = true;
        candidateText_ = text;
        nextPoll_ = nowMs + o_.settleMs;
        return false;
    }
    haveCandidate_ = false;
    if (!ParseSettingsFile(true, text).ok) {
        appliedFound_ = true;
        appliedText_ = text;
        consoleWarnings_.push_back(o_.path + " is not valid JSON; keeping the current settings until it is "
                                   "fixed and saved.");
        return false;
    }
    LiveSettings before = active_.s;
    Apply(true, text);
    if (active_.s == before) return false;
    Log(std::string("config: ") + o_.path + " changed (identity from " + SourceName(active_.identity) +
        ", registration token from " + SourceName(active_.registration) + ", url from " + SourceName(active_.url) +
        ", name from " + SourceName(active_.name) + ")");
    return true;
}

}  // namespace native
