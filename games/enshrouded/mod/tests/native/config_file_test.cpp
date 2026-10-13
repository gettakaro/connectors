// takaro\plugin.json, the one config file (ConfigWatcher on real files): created with a UUID when
// missing, token pasted while running, wrong then fixed token, emptied token, half-saved files, an
// empty identity written back without touching the file's owner or mode, and environment precedence.
#include "native/bridge.h"
#include "native/config_file.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "testlib.h"

#include <sys/stat.h>

#include <map>

using namespace native;

namespace {

const char* kId1 = "3f2a9c1e-0000-4000-8000-000000000001";
const char* kId2 = "4b4b4b4b-0000-4000-8000-000000000002";

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
    int ids = 0;
    int64_t now = 1000;
    std::unique_ptr<ConfigWatcher> w;

    explicit Install(const std::string& tag) {
        dir = t::TempDir(tag);
        path = dir + "/takaro/plugin.json";
    }
    const LiveResolution& Start() {
        ConfigWatcher::Options o;
        o.path = path;
        o.hostName = "Limon World";
        o.env = Env(env);
        o.newIdentity = [this] { return std::string(++ids == 1 ? kId1 : kId2); };
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
    CHECK(r.identity == Source::File);
    CHECK_EQ(r.s.identityToken, std::string(kId1));
    CHECK_EQ(r.s.serverName, std::string("Limon World (3f2a9c1e)"));  // unique per install: Takaro refuses a taken name
    CHECK_EQ(ConnectBlocker(r.s), std::string("no registration token"));
    SettingsFile f = Parsed(in.path);
    CHECK(f.ok);
    CHECK_EQ(f.String("identityToken"), std::string(kId1));
    CHECK_EQ(f.String("name"), std::string("Limon World (3f2a9c1e)"));
    CHECK_EQ(f.String("url"), std::string(kDefaultUrl));
    CHECK(f.obj.get("registrationToken") && f.obj.get("token"));  // every key, ready to fill in

    // The token is pasted and saved while the server runs.
    in.SetKey("registrationToken", "reg-AAAA");
    in.now += 5000;
    CHECK(!in.w->Poll(in.now));  // first sight of the new text: wait for it to settle
    in.now += 1000;
    CHECK(in.w->Poll(in.now));
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-AAAA"));
    CHECK_EQ(in.w->Active().s.identityToken, std::string(kId1));
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string());
    CHECK(!in.Settle());  // the same text again is no change

    // Wrong token, then the right one: each save is a change the caller reconnects on.
    in.SetKey("registrationToken", "reg-WRONG");
    CHECK(in.Settle());
    in.SetKey("registrationToken", "reg-AAAA");
    CHECK(in.Settle());
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-AAAA"));

    // Emptying the token disconnects.
    in.SetKey("registrationToken", "");
    CHECK(in.Settle());
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string("no registration token"));

    // A restart (or an upgrade, which never touches plugin.json) keeps the identity.
    in.SetKey("registrationToken", "reg-AAAA");
    LiveResolution r2 = in.Start();
    CHECK_EQ(r2.s.identityToken, std::string(kId1));
    CHECK_EQ(r2.s.registrationToken, std::string("reg-AAAA"));
    CHECK_EQ(in.ids, 1);  // never a second identity
}

void IdentityWrittenBack() {
    t::Group("config-identity-written-back");
    Install in("cfg-writeback");
    Write(in.path, "{\n  \"registrationToken\": \"reg-X\",\n  \"identityToken\": \"\",\n  \"token\": \"diag\"\n}\n");
    CHECK(chmod(in.path.c_str(), 0600) == 0);
    struct stat before = {};
    stat(in.path.c_str(), &before);
    LiveResolution r = in.Start();
    CHECK_EQ(r.s.identityToken, std::string(kId1));
    CHECK_EQ(r.s.serverName, std::string("Limon World (3f2a9c1e)"));
    CHECK_EQ(ConnectBlocker(r.s), std::string());
    SettingsFile f = Parsed(in.path);
    CHECK_EQ(f.String("identityToken"), std::string(kId1));
    CHECK_EQ(f.String("registrationToken"), std::string("reg-X"));  // the operator's keys stay
    CHECK_EQ(f.String("token"), std::string("diag"));
    CHECK(!f.obj.get("name"));  // only the identity is written
    struct stat after = {};
    stat(in.path.c_str(), &after);
    CHECK_EQ((int)(after.st_mode & 07777), 0600);  // the operator's mode and owner survive
    CHECK(after.st_ino == before.st_ino && after.st_uid == before.st_uid);
    std::string err, cur = Read(in.path);
    CHECK(!RewriteKeepingAccess(in.path, cur + " ", "{}", err));  // the operator saved meanwhile: untouched
    CHECK_EQ(Read(in.path), cur);
    CHECK(!in.Settle());  // its own write is not a change

    // An identity emptied while running gets a new one, written back too.
    in.SetKey("identityToken", "");
    CHECK(in.Settle());
    CHECK_EQ(in.w->Active().s.identityToken, std::string(kId2));
    CHECK_EQ(Parsed(in.path).String("identityToken"), std::string(kId2));

    // A file that cannot be created: no identity, so no connection, and the console says why.
    Install bad("cfg-nowrite");
    Write(bad.dir + "/takaro", "a file where the folder should be");
    bad.env = {{"TAKARO_REGISTRATION_TOKEN", "env-reg"}};
    LiveResolution rb = bad.Start();
    CHECK_EQ(ConnectBlocker(rb.s), std::string("no identityToken"));
    auto warn = bad.w->TakeConsoleWarnings();
    CHECK(!warn.empty() && warn[0].find("could not create") != std::string::npos);
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
        CHECK_EQ(in.ids, 0);  // the environment's identity: nothing generated or written
        CHECK(!Parsed(in.path).HasValue("identityToken"));
    }
    {
        Install in("cfg-url");
        Write(in.path, R"({"registrationToken":"r","identityToken":"i","url":"ws://own.example/"})");
        LiveResolution r = in.Start();
        CHECK(ConnectBlocker(r.s).find("wss://") != std::string::npos);  // never a plaintext connection
        CHECK_EQ(r.s.serverName, std::string("Limon World (i)"));
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
    IdentityWrittenBack();
    HalfSavedAndSettle();
    EnvironmentPrecedence();
    IdentifyErrors();
}
