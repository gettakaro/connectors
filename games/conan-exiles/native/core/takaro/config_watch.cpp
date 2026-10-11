#include "takaro/config_watch.h"

#include "common.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <cctype>

namespace takaro {

namespace {

const char kRule[] = "*************************************************************************";

bool Persisted(Source s) { return s == Source::Saved || s == Source::Current || s == Source::Generated; }

// Long runs of token characters that start like a JWT ("eyJ").
std::string ScrubJwt(std::string text) {
    for (size_t at = 0; (at = text.find("eyJ", at)) != std::string::npos;) {
        size_t end = at;
        while (end < text.size() && (isalnum((unsigned char)text[end]) || text[end] == '-' || text[end] == '_' ||
                                     text[end] == '.'))
            end++;
        if (end - at >= 20) {
            text.replace(at, end - at, "[redacted]");
            at += 10;
        } else {
            at = end;
        }
    }
    return text;
}

}  // namespace

// Every write starts with a newline: the engine writes its own lines in pieces, and a line of ours
// must never be glued to the end of a half-written engine line.
void ConsoleBanner(const std::vector<std::string>& lines) {
    std::string out = "\n" + std::string(kRule) + "\n";
    for (const auto& l : lines) {
        NativeLog("console: %s", l.c_str());
        out += "  " + l + "\n";
    }
    out += std::string(kRule) + "\n";
    ConsoleWrite(out);
}

void ConsoleLine(const std::string& line) {
    NativeLog("console: %s", line.c_str());
    ConsoleWrite("\nTakaro: " + line + "\n");
}

std::string DescribeTakaroError(const JsonValue* error, const Config& config, bool* nameTaken) {
    std::string name, message;
    double status = 0;
    if (error && error->type == JsonValue::String) {
        message = error->str;
    } else if (error && error->type == JsonValue::Object) {
        name = Str(error->get("name")).value_or("");
        message = Str(error->get("message")).value_or("");
        for (const char* k : {"http", "status", "statusCode", "httpCode", "code"}) {
            auto n = Num(error->get(k));
            if (n && *n >= 100 && *n < 600) {
                status = *n;
                break;
            }
        }
    }
    std::string text = name.empty() ? message : message.empty() ? name : name + ": " + message;
    if (text.empty()) text = "no reason given";
    if (status) text += " (HTTP " + std::to_string((int)status) + ")";
    text = ScrubJwt(config.Redact(text));
    if (text.size() > 300) text = text.substr(0, 300) + "...";
    if (nameTaken) {
        std::string l = Lower(text);
        *nameTaken = status == 409 || l.find("409") != std::string::npos || l.find("conflict") != std::string::npos ||
                     l.find("already exists") != std::string::npos || l.find("unique") != std::string::npos;
    }
    return text;
}

ConfigWatcher::ConfigWatcher(Options o) : o_(std::move(o)) {
    if (!o_.newIdentity) o_.newIdentity = NewIdentity;
}

Config ConfigWatcher::Load() {
    path_ = ConfigFilePath(o_.savedDir, o_.env);
    std::string text, readError;
    bool exists = false;
    if (!ReadWholeFile(path_, text, exists, readError)) exists = false;
    const std::string stateDir = StateDirFor(o_.savedDir, o_.env, exists ? text : "");
    priorInstall_ = StatFile(stateDir).exists;
    savedPath_ = JoinPath(stateDir, kSavedFileName);
    std::string savedText, ignored;
    bool savedExists = false;
    ReadWholeFile(savedPath_, savedText, savedExists, ignored);

    const bool envTokens = !Trim(o_.env("TAKARO_REGISTRATION_TOKEN")).empty() &&
                           !Trim(o_.env("TAKARO_IDENTITY_TOKEN")).empty();
    if (!exists && readError.empty() && !envTokens && Trim(o_.env("TAKARO_CONAN_NATIVE_DISABLE")) != "1") {
        std::string err;
        if (EnsureDirectory(DirName(path_), err) && ReplaceUserFile(path_, kConfigTemplate, 0600, err)) {
            text = kConfigTemplate;
            exists = true;
            createdFile_ = true;
            NativeLog("config: created %s from the shipped template", path_.c_str());
        } else {
            NativeLog("config: could not create %s: %s", path_.c_str(), err.c_str());
        }
    }

    ConfigInput in;
    in.savedDir = o_.savedDir;
    in.env = o_.env;
    in.fileText = text;
    in.fileExists = exists;
    in.readError = readError;
    in.savedText = savedText;
    in.savedExists = savedExists;
    in.priorInstall = priorInstall_ && !savedExists;
    in.newIdentity = o_.newIdentity;
    active_ = ResolveConfig(in);
    if (exists) appliedText_ = text;
    if (!readError.empty()) lastReadError_ = readError;
    NativeLog("config: %s (%s); identity from %s, registration token from %s, url from %s, name from %s",
              path_.c_str(), !readError.empty() ? "unreadable" : exists ? "found" : "missing",
              SourceName(active_.identitySource), SourceName(active_.registrationSource),
              SourceName(active_.urlSource), SourceName(active_.nameSource));
    if (!active_.inert) Persist(active_, false);
    return active_;
}

void ConfigWatcher::Persist(const Config& c, bool withRegistration) {
    // 1. takaro.json: fill in an identity (and the name that goes with it) the file leaves empty.
    if (appliedText_ && c.fileError.empty()) {
        JsonValue file;
        ParseJson(*appliedText_, file);
        auto empty = [&](const char* a, const char* b) {
            for (const char* k : {a, b}) {
                const JsonValue* v = k ? file.get(k) : nullptr;
                if (v && v->type == JsonValue::String && !Trim(v->str).empty()) return false;
            }
            return true;
        };
        std::optional<std::string> next = *appliedText_;
        bool changed = false;
        if (Persisted(c.identitySource) && empty("identityToken", nullptr)) {
            next = SetJsonString(*next, "identityToken", c.identityToken);
            changed = true;
        }
        if (next && Persisted(c.nameSource) && empty("name", "serverName")) {
            next = SetJsonString(*next, "name", c.serverName);
            changed = true;
        }
        if (changed) {
            std::string err;
            if (next && ReplaceUserFile(path_, *next, 0600, err)) {
                appliedText_ = *next;
                NativeLog("config: wrote the server identity (%s) into %s", SourceName(c.identitySource),
                          path_.c_str());
            } else {
                NativeLog("config: could not write the server identity into %s (%s); the saved copy keeps it",
                          path_.c_str(), next ? err.c_str() : "the file is not a plain JSON object");
            }
        }
    }
    // 2. The saved copy. An install configured only through the environment gets none.
    std::string old, ignored;
    bool savedExists = false;
    ReadWholeFile(savedPath_, old, savedExists, ignored);
    auto ours = [](Source s) { return s != Source::Env && s != Source::Default; };
    if (!savedExists && !ours(c.urlSource) && !ours(c.identitySource) && !ours(c.nameSource) &&
        !(withRegistration && ours(c.registrationSource)))
        return;
    if (c.identityToken.empty() && !savedExists) return;
    std::string text = RenderSaved(c, old, withRegistration);
    if (savedExists && old == text) return;
    std::string err;
    if (!EnsureDirectory(DirName(savedPath_), err) || !ReplaceUserFile(savedPath_, text, 0600, err))
        NativeLog("config: could not write %s: %s", savedPath_.c_str(), err.c_str());
}

bool ConfigWatcher::Poll(int64_t nowMs, Config& out) {
    if (nowMs < nextPoll_) return false;
    nextPoll_ = nowMs + o_.pollMs;
    std::string text, readError;
    bool exists = false;
    if (!ReadWholeFile(path_, text, exists, readError)) {
        if (readError != lastReadError_) NativeLog("config: %s; keeping the current settings", readError.c_str());
        lastReadError_ = readError;
        candidateText_.reset();
        return false;
    }
    lastReadError_.clear();
    if (!exists) {  // removed: keep what runs
        appliedText_.reset();
        candidateText_.reset();
        return false;
    }
    if (appliedText_ && text == *appliedText_) {
        candidateText_.reset();
        return false;
    }
    if (!candidateText_ || *candidateText_ != text) {
        candidateText_ = text;
        nextPoll_ = nowMs + o_.settleMs;
        return false;
    }
    candidateText_.reset();
    appliedText_ = text;

    std::string savedText, ignored;
    bool savedExists = false;
    ReadWholeFile(savedPath_, savedText, savedExists, ignored);
    ConfigInput in;
    in.savedDir = o_.savedDir;
    in.env = o_.env;
    in.fileText = text;
    in.fileExists = true;
    in.savedText = savedText;
    in.savedExists = savedExists;
    in.currentIdentity = active_.identityToken;
    in.currentName = active_.serverName;
    in.priorInstall = priorInstall_ && !savedExists;
    in.newIdentity = o_.newIdentity;
    Config next = ResolveConfig(in);
    if (!next.fileError.empty()) {
        ignoredSaves_++;
        ConsoleLine("WARNING: " + next.fileError + "; keeping the current settings until the file is fixed and saved");
        return false;
    }
    // Read at startup only.
    next.caFile = active_.caFile;
    next.stateDir = active_.stateDir;
    next.reconnectBaseMs = active_.reconnectBaseMs;
    next.reconnectMaxMs = active_.reconnectMaxMs;
    next.actionTimeoutMs = active_.actionTimeoutMs;
    next.actionWorkers = active_.actionWorkers;
    next.inert = active_.inert;
    for (auto& w : next.warnings) NativeLog("config warning: %s", w.c_str());
    Persist(next, false);
    const bool changed =
        next.Connection() != active_.Connection() || next.enabled != active_.enabled || next.hold != active_.hold;
    active_ = next;
    if (!changed) return false;
    reloads_++;
    out = next;
    return true;
}

void ConfigWatcher::Identified(const Config& used) {
    if (used.Connection() != active_.Connection()) return;  // an answer for settings that no longer run
    Persist(active_, true);
}

void ConfigWatcher::Banner(const std::string& key, const std::vector<std::string>& lines) {
    if (key == problem_) return;
    problem_ = key;
    ConsoleBanner(lines);
}

void ConfigWatcher::HoldBanner(const Config& c, bool force) {
    if (force) problem_.clear();
    switch (c.hold) {
        case Hold::NoToken:
            Banner("no-token", {"registrationToken not set, the server is not connected to Takaro.",
                                "Paste the registration token from Takaro into " + path_,
                                "and save it. The connector connects within a few seconds, no restart needed."});
            break;
        case Hold::NoIdentity:
            Banner("no-identity", {"identityToken is empty, the server is not connected to Takaro.",
                                   "This server ran the Takaro connector before: put its old identityToken back",
                                   "into " + path_ + " and save it (a new value registers",
                                   "a new Takaro game server). No restart needed."});
            break;
        case Hold::FileError:
            Banner("file-error", {"The Takaro config cannot be used, the server is not connected to Takaro.",
                                  c.fileError + ".",
                                  "Fix " + path_,
                                  "and save it. The connector connects within a few seconds, no restart needed."});
            break;
        case Hold::BadUrl:
            Banner("bad-url", {"The Takaro url must start with wss://, the server is not connected to Takaro.",
                               "Fix url in " + path_,
                               "and save it. The connector connects within a few seconds, no restart needed."});
            break;
        case Hold::None:
            break;
    }
}

void ConfigWatcher::Announce(const Config& c, int64_t nowMs) {
    problem_.clear();
    live_ = false;
    if (c.hold != Hold::None) {
        HoldBanner(c, true);
        reminderGap_ = 60000;
        nextReminder_ = nowMs + reminderGap_;
        return;
    }
    nextReminder_ = 0;
    ConsoleLine("connecting to " + c.Redact(c.url) + " as \"" + c.serverName + "\"");
}

void ConfigWatcher::Remind(const Config& c, int64_t nowMs) {
    if (c.hold == Hold::None || !nextReminder_ || nowMs < nextReminder_) return;
    HoldBanner(c, true);
    reminderGap_ = 15 * 60000;
    nextReminder_ = nowMs + reminderGap_;
}

void ConfigWatcher::Refused(const Config& c, const std::string& why, bool nameTaken) {
    live_ = false;
    if (nameTaken) {
        Banner("name:" + why,
               {"Takaro refused the server name \"" + c.serverName + "\": " + why + ".",
                "Another game server in this Takaro domain has that name. Set a different name in",
                path_ + " and save it. The connector reconnects within a few seconds, no restart needed."});
    } else if (c.registrationSource == Source::Env) {
        Banner("refused:" + why,
               {"Takaro refused this server: " + why + ".",
                "Check the TAKARO_REGISTRATION_TOKEN environment variable; it wins over " + path_,
                "and a changed environment variable needs a server restart."});
    } else {
        Banner("refused:" + why,
               {"Takaro refused this server: " + why + ".", "Check registrationToken in " + path_,
                "and save it. The connector reconnects within a few seconds, no restart needed."});
    }
}

void ConfigWatcher::Connected(const Config& c) {
    if (live_) return;
    live_ = true;
    problem_.clear();
    ConsoleLine("connected to Takaro as \"" + c.serverName + "\"; the server shows as reachable in the dashboard");
}

std::string ConfigWatcher::HealthJson() const {
    return ObjBuilder()
        .S("file", path_)
        .B("fileFound", appliedText_.has_value())
        .B("fileCreated", createdFile_)
        .S("savedCopy", savedPath_)
        .B("priorInstall", priorInstall_)
        .S("identitySource", SourceName(active_.identitySource))
        .S("registrationSource", SourceName(active_.registrationSource))
        .S("urlSource", SourceName(active_.urlSource))
        .S("nameSource", SourceName(active_.nameSource))
        .N("reloads", (double)reloads_)
        .N("ignoredSaves", (double)ignoredSaves_)
        .Done();
}

}  // namespace takaro
