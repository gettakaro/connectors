// takaro\plugin.json while the server runs (ConfigWatcher on real files): fresh install, token pasted
// while running, upgrade that replaced plugin.json, token precedence, half-saved files, identity
// generation and preservation, legacy installs and environment precedence.
#include "native/bridge.h"
#include "native/config_file.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "testlib.h"

#include <map>

using namespace native;

namespace {

const char* kShipped = R"({
  "registrationToken": "",
  "identityToken": "",
  "name": "",
  "url": "wss://connect.takaro.io/",
  "token": ""
}
)";

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
    std::string dir, user, saved;
    std::map<std::string, std::string> env;
    bool legacy = false;
    int ids = 0;
    int64_t now = 1000;
    std::unique_ptr<ConfigWatcher> w;

    explicit Install(const std::string& tag) {
        dir = t::TempDir(tag);
        user = dir + "/takaro/plugin.json";
        saved = dir + "/takaro/connector-state/" + kSavedFileName;
    }
    const LiveResolution& Start() {
        ConfigWatcher::Options o;
        o.userPath = user;
        o.savedPath = saved;
        o.legacyInstall = legacy;
        o.hostName = "Limon World";
        o.env = Env(env);
        o.newIdentity = [this] { return std::string(++ids == 1 ? "3f2a9c1e-0000-4000-8000-000000000001" : "4b4b4b4b-0000-4000-8000-000000000002"); };
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
};

void FreshInstall() {
    t::Group("config-fresh-install");
    Install in("cfg-fresh");
    Write(in.user, kShipped);
    LiveResolution r = in.Start();
    CHECK(r.identity == Source::Generated);
    CHECK_EQ(r.s.identityToken, std::string("3f2a9c1e-0000-4000-8000-000000000001"));
    CHECK_EQ(r.s.serverName, std::string("Limon World (3f2a9c1e)"));  // unique per install: Takaro refuses a taken name
    CHECK_EQ(ConnectBlocker(r.s), std::string("no registration token"));
    SettingsFile f = Parsed(in.user);
    CHECK_EQ(f.String("identityToken"), r.s.identityToken);  // pinned where the operator sees it
    CHECK_EQ(f.String("name"), r.s.serverName);
    CHECK_EQ(f.String("url"), std::string(kDefaultUrl));  // the operator's other keys stay
    CHECK(f.obj.get("token") != nullptr);
    SettingsFile s = Parsed(in.saved);
    CHECK_EQ(s.String("identityToken"), r.s.identityToken);
    CHECK(!s.HasValue("registrationToken"));

    // The token is pasted and saved while the server runs.
    SettingsFile edit = Parsed(in.user);
    Write(in.user, SetJsonKeys(edit, {{"registrationToken", "reg-AAAA"}}));
    in.now += 5000;
    CHECK(!in.w->Poll(in.now));  // first sight of the new text: wait for it to settle
    in.now += 1000;
    CHECK(in.w->Poll(in.now));
    CHECK(in.w->Active().registration == Source::File);
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-AAAA"));
    CHECK_EQ(in.w->Active().s.identityToken, std::string("3f2a9c1e-0000-4000-8000-000000000001"));  // unchanged
    CHECK_EQ(ConnectBlocker(in.w->Active().s), std::string());
    CHECK(!Parsed(in.saved).HasValue("registrationToken"));  // not before Takaro accepted it
    in.w->MarkAccepted(in.w->Active().s);
    CHECK(!in.w->Poll(in.now + 1));
    CHECK_EQ(Parsed(in.saved).String("registrationToken"), std::string("reg-AAAA"));
    CHECK_EQ(in.ids, 1);

    // A restart keeps everything (identity now from plugin.json).
    LiveResolution r2 = in.Start();
    CHECK(r2.identity == Source::File && r2.name == Source::File);
    CHECK_EQ(r2.s.identityToken, std::string("3f2a9c1e-0000-4000-8000-000000000001"));
    CHECK_EQ(in.ids, 1);

    // An upgrade that copied the shipped plugin.json over it: the saved copy brings everything back.
    Write(in.user, kShipped);
    LiveResolution r3 = in.Start();
    CHECK(r3.identity == Source::Saved && r3.registration == Source::Saved && r3.name == Source::Saved);
    CHECK_EQ(r3.s.identityToken, std::string("3f2a9c1e-0000-4000-8000-000000000001"));
    CHECK_EQ(r3.s.registrationToken, std::string("reg-AAAA"));
    CHECK_EQ(r3.s.serverName, std::string("Limon World (3f2a9c1e)"));
    CHECK_EQ(in.ids, 1);  // never a second identity
    SettingsFile f3 = Parsed(in.user);
    CHECK_EQ(f3.String("identityToken"), r3.s.identityToken);  // written back
    CHECK(!f3.HasValue("registrationToken"));                    // the token is never copied into plugin.json

    // A token set in plugin.json wins over the saved one.
    Write(in.user, SetJsonKeys(Parsed(in.user), {{"registrationToken", "reg-BBBB"}}));
    CHECK(in.Settle());
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-BBBB"));
    CHECK(in.w->Active().registration == Source::File);
}

void HalfSavedAndSettle() {
    t::Group("config-half-saved");
    Install in("cfg-half");
    Write(in.user, kShipped);
    in.Start();
    LiveSettings before = in.w->Active().s;
    in.w->TakeConsoleWarnings();
    Write(in.user, "{\n  \"registrationToken\": \"reg-HALF");
    CHECK(!in.Settle());
    CHECK(in.w->Active().s == before);  // keeps what runs
    auto warn = in.w->TakeConsoleWarnings();
    CHECK_EQ(warn.size(), (size_t)1);
    CHECK(!warn.empty() && warn[0].find("not valid JSON") != std::string::npos);
    CHECK(!in.Settle());
    CHECK(in.w->TakeConsoleWarnings().empty());  // reported once per text
    // A save that changes again before it settles: only the final text applies.
    Write(in.user, SetJsonKeys(Parsed(in.user).ok ? Parsed(in.user) : ParseSettingsFile(true, kShipped),
                               {{"registrationToken", "reg-ONE"}}));
    in.now += 5000;
    CHECK(!in.w->Poll(in.now));
    Write(in.user, SetJsonKeys(ParseSettingsFile(true, kShipped), {{"registrationToken", "reg-TWO"}}));
    in.now += 1000;
    CHECK(!in.w->Poll(in.now));
    in.now += 1000;
    CHECK(in.w->Poll(in.now));
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-TWO"));
    CHECK_EQ(in.w->Active().s.identityToken, before.identityToken);  // the identity from this run survives
    // A removed file keeps the running settings.
    RemoveFileIfExists(in.user);
    CHECK(!in.Settle());
    CHECK_EQ(in.w->Active().s.registrationToken, std::string("reg-TWO"));
}

void ExistingInstalls() {
    t::Group("config-existing-install");
    {
        // An install from before the saved copy, plugin.json from the old example: nothing changes.
        Install in("cfg-old");
        in.legacy = true;
        Write(in.user, R"({"registrationToken":"reg-OLD","identityToken":"my-enshrouded-server","name":"My Enshrouded Server","token":"t"})");
        LiveResolution r = in.Start();
        CHECK_EQ(r.s.identityToken, std::string("my-enshrouded-server"));
        CHECK_EQ(r.s.serverName, std::string("My Enshrouded Server"));
        CHECK_EQ(r.s.registrationToken, std::string("reg-OLD"));
        CHECK_EQ(in.ids, 0);
        CHECK_EQ(Parsed(in.saved).String("identityToken"), std::string("my-enshrouded-server"));
        in.w->MarkAccepted(r.s);
        in.w->Poll(in.now);
        CHECK_EQ(Parsed(in.saved).String("registrationToken"), std::string("reg-OLD"));
    }
    {
        // Identity set, no name: the name the connector always used.
        Install in("cfg-noname");
        Write(in.user, R"({"registrationToken":"r","identityToken":"own-id"})");
        LiveResolution r = in.Start();
        CHECK_EQ(r.s.serverName, std::string(kLegacyServerName));
        CHECK(!Parsed(in.user).HasValue("name"));  // a default is not written into the operator's file
    }
    {
        // An old install whose plugin.json was replaced by the shipped one before any saved copy existed.
        Install in("cfg-legacy-clobbered");
        in.legacy = true;
        Write(in.user, kShipped);
        LiveResolution r = in.Start();
        CHECK(r.identity == Source::Legacy);
        CHECK_EQ(r.s.identityToken, std::string(kLegacyIdentity));
        CHECK_EQ(r.s.serverName, std::string(kLegacyServerName));
        CHECK_EQ(in.ids, 0);
    }
    {
        // Only dbghelp.dll copied: plugin.json is laid down for the banner to point at.
        Install in("cfg-nofile");
        LiveResolution r = in.Start();
        CHECK(r.identity == Source::Generated);
        SettingsFile f = Parsed(in.user);
        CHECK(f.ok && f.String("identityToken") == r.s.identityToken && f.obj.get("registrationToken"));
    }
}

void EnvironmentPrecedence() {
    t::Group("config-env");
    {
        // A Docker install configured by environment: env wins, no file is created, no copy kept.
        Install in("cfg-env");
        in.env = {{"TAKARO_IDENTITY_TOKEN", "env-id"}, {"TAKARO_REGISTRATION_TOKEN", " env-reg "},
                  {"TAKARO_WS_URL", "wss://fake:8443/"}, {"TAKARO_SERVER_NAME", "Env Name"}};
        LiveResolution r = in.Start();
        CHECK(r.identity == Source::Env && r.registration == Source::Env && r.url == Source::Env && r.name == Source::Env);
        CHECK_EQ(r.s.registrationToken, std::string("env-reg"));
        CHECK_EQ(Read(in.user), std::string("<missing>"));
        CHECK_EQ(Read(in.saved), std::string("<missing>"));
        in.w->MarkAccepted(r.s);
        in.w->Poll(in.now);
        CHECK_EQ(Read(in.saved), std::string("<missing>"));
    }
    {
        Install in("cfg-env-over-file");
        in.env = {{"TAKARO_REGISTRATION_TOKEN", "env-reg"}};
        Write(in.user, R"({"registrationToken":"file-reg","identityToken":"file-id"})");
        LiveResolution r = in.Start();
        CHECK_EQ(r.s.registrationToken, std::string("env-reg"));
        CHECK_EQ(r.s.identityToken, std::string("file-id"));
        in.w->MarkAccepted(r.s);
        in.w->Poll(in.now);
        CHECK(!Parsed(in.saved).HasValue("registrationToken"));  // an env token is never stored
        CHECK_EQ(Parsed(in.saved).String("identityToken"), std::string("file-id"));
    }
    {
        // Env token only, nothing else anywhere: the generated identity is kept in the saved copy.
        Install in("cfg-env-reg-only");
        in.env = {{"TAKARO_REGISTRATION_TOKEN", "env-reg"}};
        LiveResolution r = in.Start();
        CHECK(r.identity == Source::Generated);
        CHECK_EQ(Read(in.user), std::string("<missing>"));
        CHECK_EQ(Parsed(in.saved).String("identityToken"), r.s.identityToken);
        LiveResolution r2 = in.Start();
        CHECK(r2.identity == Source::Saved && r2.s.identityToken == r.s.identityToken);
        CHECK_EQ(in.ids, 1);
    }
    {
        // A custom URL survives a shipped plugin.json (default URL); ws:// never connects.
        Install in("cfg-url");
        Write(in.user, R"({"registrationToken":"r","identityToken":"i","url":"wss://own.example/"})");
        in.Start();
        Write(in.user, R"({"registrationToken":"r","identityToken":"i","url":"wss://connect.takaro.io/"})");
        in.Settle();
        CHECK_EQ(in.w->Active().s.url, std::string("wss://own.example/"));
        CHECK(in.w->Active().url == Source::Saved);
        LiveSettings plain = in.w->Active().s;
        plain.url = "ws://own.example/";
        CHECK(ConnectBlocker(plain).find("wss://") != std::string::npos);
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
    HalfSavedAndSettle();
    ExistingInstalls();
    EnvironmentPrecedence();
    IdentifyErrors();
}
