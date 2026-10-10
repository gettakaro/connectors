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
        case Source::Saved: return "saved copy";
        case Source::Current: return "current";
        case Source::Legacy: return "legacy default";
        case Source::Generated: return "generated";
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

std::string ShortId(const std::string& identity) {
    std::string out;
    for (char c : identity)
        if (isalnum((unsigned char)c) && out.size() < 8) out += c;
    return out;
}

bool FromOperatorOrLegacy(Source s) { return s == Source::Env || s == Source::File || s == Source::Legacy; }
bool Stored(Source s) { return s != Source::Env && s != Source::Default; }

std::string PrettyObject(const JsonValue& obj) {
    if (obj.obj.empty()) return "{}\n";
    std::string out = "{\n";
    for (size_t i = 0; i < obj.obj.size(); i++)
        out += "  " + JsonStr(obj.obj[i].first) + ": " + JsonDump(obj.obj[i].second) + (i + 1 < obj.obj.size() ? ",\n" : "\n");
    return out + "}\n";
}

}  // namespace

LiveResolution ResolveLive(const ResolveInputs& in, const EnvFn& env, const std::function<std::string()>& newIdentity) {
    static const SettingsFile none;
    const SettingsFile& user = in.user ? *in.user : none;
    const SettingsFile& saved = in.saved ? *in.saved : none;
    LiveResolution r;
    std::string v;

    if (!(v = EnvValue(env, {"TAKARO_WS_URL", "TAKARO_URL"})).empty()) r.s.url = v, r.url = Source::Env;
    else if (!(v = user.String("url")).empty() && v != kDefaultUrl) r.s.url = v, r.url = Source::File;
    else if (!(v = saved.String("url")).empty()) r.s.url = v, r.url = Source::Saved;
    else r.s.url = kDefaultUrl, r.url = Source::Default;

    if (!(v = EnvValue(env, {"TAKARO_REGISTRATION_TOKEN"})).empty()) r.s.registrationToken = v, r.registration = Source::Env;
    else if (!(v = user.String("registrationToken")).empty()) r.s.registrationToken = v, r.registration = Source::File;
    else if (!(v = saved.String("registrationToken")).empty()) r.s.registrationToken = v, r.registration = Source::Saved;

    if (!(v = EnvValue(env, {"TAKARO_IDENTITY_TOKEN"})).empty()) r.s.identityToken = v, r.identity = Source::Env;
    else if (!(v = user.String("identityToken")).empty()) r.s.identityToken = v, r.identity = Source::File;
    else if (!(v = saved.String("identityToken")).empty()) r.s.identityToken = v, r.identity = Source::Saved;
    else if (!in.currentIdentity.empty()) r.s.identityToken = in.currentIdentity, r.identity = Source::Current;
    else if (in.legacyInstall) r.s.identityToken = kLegacyIdentity, r.identity = Source::Legacy;
    else r.s.identityToken = newIdentity(), r.identity = Source::Generated;

    if (!(v = EnvValue(env, {"TAKARO_SERVER_NAME", "TAKARO_NAME"})).empty()) r.s.serverName = v, r.name = Source::Env;
    else if (!(v = user.String("name")).empty()) r.s.serverName = v, r.name = Source::File;
    else if (!(v = saved.String("name")).empty()) r.s.serverName = v, r.name = Source::Saved;
    else if (FromOperatorOrLegacy(r.identity)) r.s.serverName = kLegacyServerName, r.name = Source::Default;
    else if (!in.currentName.empty() && r.identity == Source::Current) r.s.serverName = in.currentName, r.name = Source::Current;
    else {
        std::string base = Trim(in.hostName).empty() ? "Enshrouded" : Trim(in.hostName);
        r.s.serverName = base + " (" + ShortId(r.s.identityToken) + ")";
        r.name = Source::Generated;
    }
    return r;
}

std::string ConnectBlocker(const LiveSettings& s) {
    if (s.registrationToken.empty()) return "no registration token";
    if (s.url.compare(0, 6, "wss://") != 0) return "url must start with wss:// (no plaintext connection to Takaro)";
    return "";
}

std::string SetJsonKeys(const SettingsFile& file, const std::vector<std::pair<std::string, std::string>>& keys) {
    JsonValue obj = file.ok ? file.obj : JObj();
    for (auto& kv : keys) Put(obj, kv.first, JStr(kv.second));
    return PrettyObject(obj);
}

std::string FreshPluginJson(const LiveSettings& s) {
    JsonValue obj = JObj();
    Put(obj, "registrationToken", JStr(""));
    Put(obj, "identityToken", JStr(s.identityToken));
    Put(obj, "name", JStr(s.serverName));
    Put(obj, "url", JStr(kDefaultUrl));
    Put(obj, "token", JStr(""));
    return PrettyObject(obj);
}

std::string RenderSaved(const LiveResolution& r, const SettingsFile& oldSaved, bool withRegistration) {
    JsonValue obj = JObj();
    auto put = [&](const char* key, const std::string& inUse, bool write) {
        std::string v = write ? inUse : oldSaved.String(key);
        if (!v.empty()) Put(obj, key, JStr(v));
    };
    put("url", r.s.url, Stored(r.url));
    put("identityToken", r.s.identityToken, Stored(r.identity));
    put("name", r.s.serverName, Stored(r.name));
    put("registrationToken", r.s.registrationToken, withRegistration && Stored(r.registration));
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

SettingsFile ConfigWatcher::ReadSaved() const {
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(o_.savedPath, text, exists, err)) return SettingsFile();
    SettingsFile f = ParseSettingsFile(exists, text);
    if (exists && !f.ok) Log("config: " + o_.savedPath + " is not a JSON object; ignoring it");
    return f;
}

LiveResolution ConfigWatcher::Resolve(const SettingsFile& user, const SettingsFile& saved) const {
    ResolveInputs in;
    in.user = &user;
    in.saved = &saved;
    in.legacyInstall = o_.legacyInstall;
    in.currentIdentity = active_.s.identityToken;
    in.currentName = active_.name == Source::Generated || active_.name == Source::Current ? active_.s.serverName : "";
    in.hostName = o_.hostName;
    return ResolveLive(in, o_.env, o_.newIdentity);
}

const LiveResolution& ConfigWatcher::Start(int64_t nowMs) {
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(o_.userPath, text, exists, err)) {
        Log("config: " + err + "; using the environment and the saved copy");
        exists = false;
        text.clear();
    }
    SettingsFile user = ParseSettingsFile(exists, text);
    if (exists && !user.ok)
        consoleWarnings_.push_back(o_.userPath + " is not valid JSON, so it is ignored. Fix it and save it; "
                                   "the connector reads it again within a few seconds.");
    active_ = Resolve(user, ReadSaved());
    appliedUser_ = user;
    appliedFound_ = exists;
    appliedText_ = text;
    nextPoll_ = nowMs + o_.pollMs;
    Persist(user);
    return active_;
}

void ConfigWatcher::MarkAccepted(const LiveSettings& accepted) {
    std::lock_guard<std::mutex> g(acceptedMu_);
    acceptedPending_ = true;
    accepted_ = accepted;
}

std::vector<std::string> ConfigWatcher::TakeConsoleWarnings() {
    std::vector<std::string> out;
    out.swap(consoleWarnings_);
    return out;
}

bool ConfigWatcher::Poll(int64_t nowMs) {
    {
        LiveSettings acc;
        bool pending = false;
        {
            std::lock_guard<std::mutex> g(acceptedMu_);
            pending = acceptedPending_;
            acceptedPending_ = false;
            acc = accepted_;
        }
        if (pending && acc == active_.s && acceptedRegistration_ != acc.registrationToken) {
            acceptedRegistration_ = acc.registrationToken;
            Persist(appliedUser_);
        }
    }
    if (nowMs < nextPoll_) return false;
    nextPoll_ = nowMs + o_.pollMs;
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(o_.userPath, text, exists, err)) return false;  // unreadable right now: try again later
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
    if (!haveCandidate_ || candidateFound_ != exists || candidateText_ != text) {
        haveCandidate_ = true;
        candidateFound_ = exists;
        candidateText_ = text;
        nextPoll_ = nowMs + o_.settleMs;
        return false;
    }
    haveCandidate_ = false;
    appliedFound_ = exists;
    appliedText_ = text;
    SettingsFile user = ParseSettingsFile(exists, text);
    if (!user.ok) {
        consoleWarnings_.push_back(o_.userPath + " is not valid JSON; keeping the current settings until it is "
                                   "fixed and saved.");
        return false;
    }
    appliedUser_ = user;
    LiveSettings before = active_.s;
    active_ = Resolve(user, ReadSaved());
    Persist(user);
    if (active_.s == before) return false;
    Log(std::string("config: ") + o_.userPath + " changed (identity from " + SourceName(active_.identity) +
        ", registration token from " + SourceName(active_.registration) + ", url from " + SourceName(active_.url) +
        ", name from " + SourceName(active_.name) + ")");
    return true;
}

void ConfigWatcher::Persist(const SettingsFile& user) {
    std::string err;
    // 1. Pin the identity (and a generated name) in plugin.json, where the operator sees it.
    if (user.found && user.ok) {
        std::vector<std::pair<std::string, std::string>> keys;
        Source id = active_.identity, nm = active_.name;
        if (!user.HasValue("identityToken") && id != Source::Env)
            keys.push_back({"identityToken", active_.s.identityToken});
        if (!user.HasValue("name") && (nm == Source::Saved || nm == Source::Current || nm == Source::Generated))
            keys.push_back({"name", active_.s.serverName});
        if (!keys.empty()) {
            std::string next = SetJsonKeys(user, keys);
            if (AtomicWriteFile(o_.userPath, next, err)) {
                appliedText_ = next;
                appliedFound_ = true;
                appliedUser_ = ParseSettingsFile(true, next);
                Log(std::string("config: wrote the server identity (") + SourceName(id) + ") into " + o_.userPath);
            } else {
                Log("config: could not write the server identity into " + o_.userPath + " (" + err +
                    "); the saved copy keeps it");
            }
        }
    } else if (!user.found && active_.identity != Source::Env && active_.registration != Source::Env &&
               active_.url != Source::Env && active_.name != Source::Env) {
        // Only dbghelp.dll was copied: lay down the file the console banner points at.
        std::string next = FreshPluginJson(active_.s);
        if (EnsureDirectory(DirName(o_.userPath), err) && AtomicWriteFile(o_.userPath, next, err)) {
            appliedText_ = next;
            appliedFound_ = true;
            appliedUser_ = ParseSettingsFile(true, next);
            Log("config: created " + o_.userPath);
        } else {
            Log("config: could not create " + o_.userPath + " (" + err + ")");
        }
    }
    // 2. The saved copy, outside what an upgrade replaces.
    SettingsFile saved = ReadSaved();
    bool withRegistration = !acceptedRegistration_.empty() && acceptedRegistration_ == active_.s.registrationToken;
    if (!saved.found && !Stored(active_.url) && !Stored(active_.identity) && !Stored(active_.name) &&
        !(withRegistration && Stored(active_.registration)))
        return;  // an environment-only install keeps no copy
    std::string text = RenderSaved(active_, saved, withRegistration);
    std::string old;
    bool exists = false;
    if (ReadWholeFile(o_.savedPath, old, exists, err) && exists && old == text) return;
    if (!EnsureDirectory(DirName(o_.savedPath), err) || !AtomicWriteFile(o_.savedPath, text, err))
        Log("config: could not write " + o_.savedPath + " (" + err + ")");
}

}  // namespace native
