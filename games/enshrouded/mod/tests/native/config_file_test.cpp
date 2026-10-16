// takaro\plugin.json, the one config file (ConfigWatcher on real files): created with two placeholders
// when missing, token and server name filled in while running, wrong then fixed token, emptied token,
// half-saved files, an older file with "name"/"url"/"token", and environment precedence.
#include "native/bridge.h"
#include "native/config_file.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "testlib.h"

#include <sys/stat.h>

#include <map>

using namespace native;

namespace {


EnvFn Env(std::map<std::string, std::string> m) {
    return [m](const char* k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
}

void Write(const std::string& path, const std::string& text) {
    std::string err;
    CHECK_MSG(EnsureDirectory(DirName(path), err) && AtomicWriteFile(path, text, err), err);
}

std::string Read(const std::string& path) {
    std::string text, err;
    bool exists = false;
    ReadWholeFile(path, text, exists, err);
    return exists ? text : "<missing>";
}

SettingsFile Parsed(const std::string& path) {
    std::string text = Read(path);
    return ParseSettingsFile(text != "<missing>", text);
}

struct Install {
    std::string dir, path;
    std::map<std::string, std::string> env;
    int64_t now = 1000;
    std::unique_ptr<ConfigWatcher> w;

    explicit Install(const std::string& tag) {
        dir = t::TempDir(tag);
        path = dir + "/takaro/plugin.json";
    }
    const LiveResolution& Start() {
        ConfigWatcher::Options o;
        o.path = path;
        o.env = Env(env);
        w.reset(new ConfigWatcher(o));
        return w->Start(now);
    }
    // Two reads settleMs apart: what a save while running needs before it applies.
    bool Settle() {
        now += 5000;
        bool a = w->Poll(now);
        now += 1000;
        bool b = w->Poll(now);
        return a || b;
    }
    void SetKey(const char* key, const std::string& value) { Write(path, SetJsonKey(Parsed(path), key, value)); }
};

void FreshInstall() {
    t::Group("config-fresh-install");
    Install in("cfg-fresh");
    LiveResolution r = in.Start();  // only dbghelp.dll was copied: no plugin.json yet
    CHECK_EQ(Read(in.path), std::string("{\n  \"registrationToken\": \"paste-your-token-here\",\n  \"identityToken\": \"name-your-server\"\n}\n"));
    CHECK(r.registration == Source::Default && r.identity == Source::Default);  // placeholders count as not set
    CHECK(r.s.registrationToken.empty() && r.s.identityToken.empty() && r.s.serverName.empty());
    CHECK_EQ(r.s.url, std::string(kDefaultUrl));
    CHECK_EQ(ConnectBlocker(r.s), std::string("no registration token"));
    CHECK(r.s.protocolV1);  // protocol 1 is offered without a "protocol" key

    // The token is pasted and saved while the server runs: still waiting for the server name.
    in.SetKey("registrationToken", "reg-AAAA");
    in.now += 5000;
    CHECK(!in.w->Poll(in.now));  // first sight of the new text: wait for it to settle
    in.now += 1000;
    CHECK(in.w->Poll(in.now));
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-AAAA"));
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string("no identityToken"));

    // The server name: it is the identity Takaro keeps the game server under, and its name.
    in.SetKey("identityToken", "Limon World");
    CHECK(in.Settle());
    CHECK_EQ(in.w->Active().s.identityToken, std::string("Limon World"));
    CHECK_EQ(in.w->Active().s.serverName, std::string("Limon World"));
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string());
    CHECK(!in.Settle());  // the same text again is no change
    CHECK_EQ(Read(in.path).find("\"name\""), std::string::npos);  // nothing written into the operator's file
    CHECK_EQ(Read(in.path).find("\"url\""), std::string::npos);

    // Wrong token, then the right one: each save is a change the caller reconnects on.
    in.SetKey("registrationToken", "reg-WRONG");
    CHECK(in.Settle());
    in.SetKey("registrationToken", "reg-AAAA");
    CHECK(in.Settle());
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-AAAA"));

    // A saved protocol alone is a change (the caller reconnects with it); a number works as well as text.
    std::string text = Read(in.path);
    Write(in.path, text.replace(text.rfind('}'), 1, ",\n  \"protocol\": 0\n}"));
    CHECK(in.Settle());
    CHECK(!in.w->Active().s.protocolV1);
    CHECK_EQ(SourceName(in.w->Active().protocol), std::string("plugin.json"));
    in.SetKey("protocol", "1");
    CHECK(in.Settle());
    CHECK(in.w->Active().s.protocolV1);

    // Emptying the token disconnects.
    in.SetKey("registrationToken", "");
    CHECK(in.Settle());
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string("no registration token"));

    // A restart (or an upgrade, which never touches plugin.json) keeps everything.
    in.SetKey("registrationToken", "reg-AAAA");
    LiveResolution r2 = in.Start();
    CHECK_EQ(r2.s.identityToken, std::string("Limon World"));
    CHECK_EQ(r2.s.registrationToken, std::string("reg-AAAA"));

    // A file that cannot be created: no identity, so no connection, and the console says why.
    Install bad("cfg-nowrite");
    Write(bad.dir + "/takaro", "a file where the folder should be");
    bad.env = {{"TAKARO_REGISTRATION_TOKEN", "env-reg"}};
    LiveResolution rb = bad.Start();
    CHECK_EQ(ConnectBlocker(rb.s), std::string("no identityToken"));
    auto warn = bad.w->TakeConsoleWarnings();
    CHECK(!warn.empty() && warn[0].find("could not create") != std::string::npos);
}

void OlderFile() {
    t::Group("config-older-file");
    // A plugin.json from an earlier version, with name, url and token: read as it is, never rewritten.
    Install in("cfg-older");
    std::string text = "{\n  \"registrationToken\": \"reg-X\",\n  \"identityToken\": \"Enshrouded\",\n  \"name\": \"Enshrouded\",\n"
                       "  \"url\": \"wss://connect.takaro.io/\",\n  \"token\": \"\"\n}\n";
    Write(in.path, text);
    CHECK(chmod(in.path.c_str(), 0600) == 0);
    LiveResolution r = in.Start();
    CHECK_EQ(r.s.identityToken, std::string("Enshrouded"));
    CHECK_EQ(r.s.serverName, std::string("Enshrouded"));
    CHECK(r.name == Source::File);
    CHECK_EQ(ConnectBlocker(r.s), std::string());
    CHECK_EQ(Read(in.path), text);  // untouched
    struct stat after = {};
    stat(in.path.c_str(), &after);
    CHECK_EQ((int)(after.st_mode & 07777), 0600);
    // An older file with an empty identity: not connected until the server name is set.
    Install empty("cfg-older-empty");
    Write(empty.path, "{\n  \"registrationToken\": \"reg-X\",\n  \"identityToken\": \"\",\n  \"token\": \"diag\"\n}\n");
    LiveResolution re = empty.Start();
    CHECK_EQ(ConnectBlocker(re.s), std::string("no identityToken"));
    CHECK_EQ(Parsed(empty.path).String("identityToken"), std::string());  // nothing generated or written
    // RewriteKeepingAccess (used elsewhere) still leaves a file the operator saved meanwhile alone.
    std::string err, cur = Read(in.path);
    CHECK(!RewriteKeepingAccess(in.path, cur + " ", "{}", err));
    CHECK_EQ(Read(in.path), cur);
}

void HalfSavedAndSettle() {
    t::Group("config-half-saved");
    Install in("cfg-half");
    in.Start();
    LiveSettings before = in.w->Active().s;
    in.w->TakeConsoleWarnings();
    std::string good = Read(in.path);
    Write(in.path, "{\n  \"registrationToken\": \"reg-HALF");
    CHECK(!in.Settle());
    CHECK(in.w->Active().s == before);  // keeps what runs
    auto warn = in.w->TakeConsoleWarnings();
    CHECK_EQ(warn.size(), (size_t)1);
    CHECK(!warn.empty() && warn[0].find("not valid JSON") != std::string::npos);
    CHECK(!in.Settle());
    CHECK(in.w->TakeConsoleWarnings().empty());  // reported once per text
    // A save that changes again before it settles: only the final text applies.
    Write(in.path, SetJsonKey(ParseSettingsFile(true, good), "registrationToken", "reg-ONE"));
    in.now += 5000;
    CHECK(!in.w->Poll(in.now));
    Write(in.path, SetJsonKey(ParseSettingsFile(true, good), "registrationToken", "reg-TWO"));
    in.now += 1000;
    CHECK(!in.w->Poll(in.now));
    in.now += 1000;
    CHECK(in.w->Poll(in.now));
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-TWO"));
    CHECK_EQ(in.w->Active().s.identityToken, before.identityToken);
    // A removed file keeps the running settings.
    RemoveFileIfExists(in.path);
    CHECK(!in.Settle());
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-TWO"));
}

void EnvironmentPrecedence() {
    t::Group("config-env");
    {
        // A Docker install configured by environment: env wins per field and is never written.
        Install in("cfg-env");
        in.env = {{"TAKARO_IDENTITY_TOKEN", "env-id"}, {"TAKARO_REGISTRATION_TOKEN", " env-reg "},
                  {"TAKARO_WS_URL", "wss://fake:8443/"}, {"TAKARO_SERVER_NAME", "Env Name"}};
        LiveResolution r = in.Start();
        CHECK(r.identity == Source::Env && r.registration == Source::Env && r.url == Source::Env && r.name == Source::Env);
        CHECK_EQ(r.s.registrationToken, std::string("env-reg"));
        CHECK_EQ(r.s.identityToken, std::string("env-id"));
        std::string text = Read(in.path);
        CHECK(text.find("env-") == std::string::npos);  // the created file carries none of it
    }
    {
        Install in("cfg-env-over-file");
        in.env = {{"TAKARO_REGISTRATION_TOKEN", "env-reg"}, {"TAKARO_IDENTITY_TOKEN", "env-id"}};
        Write(in.path, R"({"registrationToken":"file-reg","identityToken":"","url":"wss://own.example/"})");
        LiveResolution r = in.Start();
        CHECK_EQ(r.s.registrationToken, std::string("env-reg"));
        CHECK_EQ(r.s.identityToken, std::string("env-id"));
        CHECK_EQ(r.s.url, std::string("wss://own.example/"));  // fields the environment leaves unset
        CHECK(r.url == Source::File);
        CHECK(!Parsed(in.path).HasValue("identityToken"));  // the environment's identity is never written
    }
    {
        Install in("cfg-url");
        Write(in.path, R"({"registrationToken":"r","identityToken":"i","url":"ws://own.example/"})");
        LiveResolution r = in.Start();
        CHECK(ConnectBlocker(r.s).find("wss://") != std::string::npos);  // never a plaintext connection
        CHECK_EQ(r.s.serverName, std::string("i"));  // the identity is the name
    }
}

void IdentifyErrors() {
    t::Group("config-identify-errors");
    int status = 0;
    JsonValue e = t::J(R"({"name":"AxiosError","message":"Request failed with status code 409","config":{"headers":{"x-takaro-token":"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4eHh4eHh4eHh4eHh4eHgifQ.c2lnbmF0dXJlc2lnbmF0dXJl"}},"status":409})");
    std::string s = IdentifyErrorSummary(&e, status);
    CHECK_EQ(status, 409);
    CHECK_EQ(s, std::string("AxiosError: Request failed with status code 409 (HTTP 409)"));
    CHECK(s.find("eyJ") == std::string::npos);
    JsonValue str = t::J(R"("Request failed with status code 409")");
    IdentifyErrorSummary(&str, status);
    CHECK_EQ(status, 409);
    JsonValue jwt = t::J(R"({"message":"token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4eHh4eHh4eHh4eHh4eHgifQ.c2lnbmF0dXJl rejected","http":400})");
    s = IdentifyErrorSummary(&jwt, status);
    CHECK_EQ(status, 400);
    CHECK_MSG(s.find("eyJ") == std::string::npos && s.find("<redacted>") != std::string::npos, s);
}

}  // namespace

void RunConfigFileTests() {
    FreshInstall();
    OlderFile();
    HalfSavedAndSettle();
    EnvironmentPrecedence();
    IdentifyErrors();
}
