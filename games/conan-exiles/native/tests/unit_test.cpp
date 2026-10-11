// Host unit tests for everything that does not need a game process or a socket: JSON and the
// Takaro protocol shapes, config (env first, takaro.json, saved copy, identity, live reload, the
// JSON key edit, error redaction), the durable
// outbox, the frame queues and heartbeat, the pins scanner and its refusal rules, the Conan
// adapter (argument shapes, refusal, pending actions), the coverage registry, text and the
// ChatRpcData layout, and the ELF / /proc/self/maps helpers.
#include "common.h"
#include "gtstats.h"
#include "conan/adapter.h"
#include "conan/coverage.h"
#include "conan/text.h"
#include "elfscan.h"
#include "pins/pins.h"
#include "takaro/config.h"
#include "takaro/config_watch.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/outbox.h"
#include "takaro/protocol.h"
#include "takaro/transport.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace conan;

static int g_failed = 0, g_ran = 0;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        g_ran++;                                                        \
        if (!(cond)) {                                                  \
            g_failed++;                                                 \
            printf("FAIL %s:%d: %s\n     ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)
#define EQ(a, b) CHECK((a) == (b), "got '%s' want '%s'", std::string(a).c_str(), std::string(b).c_str())

static JsonValue J(const std::string& text) {
    JsonValue v;
    if (!JsonParse(text, v)) printf("bad test json: %s\n", text.c_str());
    return v;
}

static void TestProtocol() {
    using namespace takaro;
    EQ(JsonDump(NormalizeArgs(nullptr)), "{}");
    JsonValue arr = J("[]"), nul = J("null"), str = J("\"{\\\"message\\\":\\\"hi\\\"}\""), blank = J("\"  \"");
    JsonValue obj = J("{\"a\":1}"), bad = J("\"{oops\""), strArr = J("\"[1,2]\"");
    EQ(JsonDump(NormalizeArgs(&arr)), "{}");
    EQ(JsonDump(NormalizeArgs(&nul)), "{}");
    EQ(JsonDump(NormalizeArgs(&str)), "{\"message\":\"hi\"}");
    EQ(JsonDump(NormalizeArgs(&blank)), "{}");
    EQ(JsonDump(NormalizeArgs(&obj)), "{\"a\":1}");
    EQ(JsonDump(NormalizeArgs(&bad)), "{}");
    EQ(JsonDump(NormalizeArgs(&strArr)), "{}");

    EQ(PlayerId(J("{\"gameId\":\"7656\"}")), "7656");
    EQ(PlayerId(J("{\"gameId\":null,\"player\":{\"gameId\":\"p1\"}}")), "p1");
    EQ(PlayerId(J("{\"playerRef\":{\"steamId\":\"s1\"}}")), "s1");
    EQ(PlayerId(J("{\"player\":null}")), "");
    EQ(StripSteamPrefix("steam:76561198000000001"), "76561198000000001");
    EQ(StripSteamPrefix("Platform:Steam:76561198000000001"), "76561198000000001");
    EQ(StripSteamPrefix("eos:abc"), "eos:abc");

    JsonValue v;
    CHECK(JsonParse(CreateIdentify("i", "r", "n"), v), "identify json");
    EQ(JsonDump(v), "{\"type\":\"identify\",\"payload\":{\"identityToken\":\"i\",\"registrationToken\":\"r\",\"name\":\"n\"}}");
    EQ(CreateResponse("r1", JsonValue()), "{\"type\":\"response\",\"requestId\":\"r1\",\"payload\":{}}");
    EQ(CreateErrorResponse("r2", "bad \"x\""), "{\"type\":\"response\",\"requestId\":\"r2\",\"error\":\"bad \\\"x\\\"\"}");
    EQ(CreateGameEvent("log", J("{\"msg\":\"m\"}")), "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"log\",\"data\":{\"msg\":\"m\"}}}");
    CHECK(IsAction("getMapTile") && IsAction("shutdown") && !IsAction("rcon"), "actions");
    CHECK(IsEventType("entity-killed") && !IsEventType("player-joined"), "events");

    int64_t ms = 0;
    CHECK(ParseIsoMs("2026-10-03T10:00:00.250Z", ms) && ms == 1791021600250LL, "iso %lld", (long long)ms);
    CHECK(ParseIsoMs("2026-10-03T12:00:00+02:00", ms) && ms == 1791021600000LL, "offset");
    CHECK(!ParseIsoMs("2026-10-03T10:00:00", ms), "no zone refused");
    EQ(FormatIsoMs(1791021600250LL), "2026-10-03T10:00:00.250Z");
}

static void TestConfig() {
    using namespace takaro;
    std::map<std::string, std::string> envs;
    EnvFn env = [&](const char* n) { return envs.count(n) ? envs[n] : std::string(); };
    const std::string saved = "/srv/conan/ConanSandbox/Saved";
    EQ(ConfigFilePath(saved, env), "/srv/conan/ConanSandbox/Saved/Config/Takaro/takaro.json");
    int generated = 0;
    auto gen = [&] {
        generated++;
        return std::string("0123abcd-0000-4000-8000-00000000000") + std::to_string(generated);
    };
    auto resolve = [&](const std::string& file, bool exists, const std::string& savedText = "", bool prior = false,
                       const std::string& current = "") {
        ConfigInput in;
        in.savedDir = saved;
        in.env = env;
        in.fileText = file;
        in.fileExists = exists;
        in.savedText = savedText;
        in.savedExists = !savedText.empty();
        in.priorInstall = prior;
        in.currentIdentity = current;
        in.newIdentity = gen;
        return ResolveConfig(in);
    };

    // Fresh install with the shipped file: holds for the token, generates an identity and a unique name.
    Config c = resolve(kConfigTemplate, true);
    CHECK(!c.enabled && !c.inert && c.hold == Hold::NoToken, "%s", c.disabledReason.c_str());
    CHECK(c.disabledReason.find("registrationToken") != std::string::npos, "%s", c.disabledReason.c_str());
    CHECK(c.identitySource == Source::Generated && c.identityToken.size() == 36, "generated identity");
    EQ(c.serverName, "Conan Exiles (0123abcd)");
    CHECK(c.nameSource == Source::Generated, "generated name");
    EQ(c.url, kDefaultUrl);
    // No file at all behaves the same.
    c = resolve("", false);
    CHECK(c.hold == Hold::NoToken && c.identitySource == Source::Generated, "no file");
    // Token pasted: connects.
    c = resolve("{\"registrationToken\":\"reg-5678\",\"identityToken\":\"\"}", true);
    CHECK(c.enabled && c.registrationSource == Source::File, "%s", c.disabledReason.c_str());
    // The 3.2.0 example placeholder is no token.
    c = resolve("{\"registrationToken\":\"paste-your-registration-token-here\",\"identityToken\":\"my-conan-server\"}",
                true);
    CHECK(c.hold == Hold::NoToken && !c.warnings.empty(), "placeholder token holds");
    // ...and an install on the old example identity keeps it (never a new UUID), with its old name.
    EQ(c.identityToken, "my-conan-server");
    CHECK(c.identitySource == Source::File, "example identity kept");
    EQ(c.serverName, "Conan Exiles");
    // Existing install: everything from the file.
    c = resolve("{\"identityToken\":\"file-id-1234\",\"registrationToken\":\"file-reg-5678\",\"serverName\":\"F\","
                "\"caFile\":\"ca.pem\",\"_comment\":\"x\"}",
                true, "", true);
    CHECK(c.enabled, "%s", c.disabledReason.c_str());
    EQ(c.identityToken, "file-id-1234");
    EQ(c.serverName, "F");
    EQ(c.caFile, saved + "/ca.pem");
    EQ(c.stateDir, saved + "/Takaro/state");
    // Upgrade where the shipped file replaced takaro.json: the saved copy restores token, identity, url, name.
    const std::string savedCopy =
        "{\"url\":\"wss://eu.example.test/\",\"identityToken\":\"saved-id-1\",\"registrationToken\":\"saved-reg-1\","
        "\"name\":\"Saved Name\"}";
    c = resolve(kConfigTemplate, true, savedCopy, false);
    CHECK(c.enabled && c.identitySource == Source::Saved && c.registrationSource == Source::Saved, "saved restores");
    EQ(c.identityToken, "saved-id-1");
    EQ(c.registrationToken, "saved-reg-1");
    EQ(c.url, "wss://eu.example.test/");  // the shipped default URL does not undo a saved one
    EQ(c.serverName, "Saved Name");
    // A token typed into the file wins over the saved one.
    c = resolve("{\"registrationToken\":\"new-reg-2\"}", true, savedCopy);
    EQ(c.registrationToken, "new-reg-2");
    EQ(c.identityToken, "saved-id-1");
    // Prior install (state dir from an older connector) with no identity anywhere: hold, never generate.
    generated = 0;
    c = resolve("{\"registrationToken\":\"reg-1\"}", true, "", true);
    CHECK(c.hold == Hold::NoIdentity && c.identityToken.empty() && generated == 0, "%s", c.disabledReason.c_str());
    // While running, the identity in use is kept when the file loses it.
    c = resolve("{\"registrationToken\":\"reg-1\"}", true, "", true, "running-id");
    CHECK(c.enabled && c.identitySource == Source::Current, "current identity kept");
    EQ(c.identityToken, "running-id");
    // Half-saved / wrong types: hold with the reason, no identity invented.
    generated = 0;
    c = resolve("{\"identityToken\": oops", true);
    CHECK(c.hold == Hold::FileError && c.fileError.find("not valid JSON") != std::string::npos && generated == 0,
          "%s", c.fileError.c_str());
    c = resolve("[1]", true);
    CHECK(c.hold == Hold::FileError, "non-object file holds");
    c = resolve("{\"registrationToken\":5}", true);
    CHECK(c.hold == Hold::FileError && c.fileError.find("must be a string") != std::string::npos, "%s",
          c.fileError.c_str());
    c = resolve("{\"registrationToken\":\"r-1234\",\"identityToken\":\"i-1\",\"bogus\":\"1\"}", true);
    CHECK(c.enabled && c.warnings.size() == 1, "unknown key warns");
    // Environment wins over everything, and is never written to the saved copy.
    envs["TAKARO_IDENTITY_TOKEN"] = "env-id-9999";
    envs["TAKARO_WS_URL"] = "wss://example.test/ws";
    c = resolve("{\"identityToken\":\"file-id-1234\",\"registrationToken\":\"file-reg-5678\"}", true, savedCopy);
    CHECK(c.enabled && c.identitySource == Source::Env, "env+file");
    EQ(c.identityToken, "env-id-9999");
    EQ(c.registrationToken, "file-reg-5678");
    EQ(c.url, "wss://example.test/ws");
    std::string summary = ConfigSummaryJson(c);
    CHECK(summary.find("env-id-9999") == std::string::npos && summary.find("file-reg-5678") == std::string::npos,
          "summary leaks a token: %s", summary.c_str());
    CHECK(summary.find("\"identityToken\":\"env TAKARO_IDENTITY_TOKEN\"") != std::string::npos, "%s", summary.c_str());
    EQ(c.Redact("error: token env-id-9999 and file-reg-5678 rejected"), "error: token [redacted] and [redacted] rejected");
    std::string render = RenderSaved(c, savedCopy, true);
    CHECK(render.find("env-id-9999") == std::string::npos && render.find("saved-id-1") != std::string::npos &&
              render.find("file-reg-5678") != std::string::npos && render.find("example.test/ws") == std::string::npos,
          "env values stay out of the saved copy: %s", render.c_str());
    render = RenderSaved(c, savedCopy, false);
    CHECK(render.find("saved-reg-1") != std::string::npos && render.find("file-reg-5678") == std::string::npos,
          "an unproven token is not saved: %s", render.c_str());
    envs["TAKARO_WS_URL"] = "ws://plain/";
    c = resolve("{\"registrationToken\":\"r-1234\"}", true);
    CHECK(c.hold == Hold::BadUrl && c.disabledReason.find("wss://") != std::string::npos, "plaintext refused");
    envs["TAKARO_WS_URL"] = "";
    envs["TAKARO_CONAN_NATIVE_DISABLE"] = "1";
    c = resolve("{\"registrationToken\":\"r-1234\"}", true);
    CHECK(c.inert && !c.enabled && c.disabledReason.find("DISABLE") != std::string::npos, "kill switch");
    envs.clear();
    envs["TAKARO_CONAN_CONFIG"] = "/etc/takaro/conan.json";
    EQ(ConfigFilePath(saved, env), "/etc/takaro/conan.json");
    envs["TAKARO_RECONNECT_BASE_MS"] = "abc";
    c = LoadConfig(saved, env, "", false);
    CHECK(c.reconnectBaseMs == 2000 && !c.warnings.empty(), "bad number warns and defaults");
    envs.clear();
    EQ(StateDirFor(saved, env, "{\"stateDir\":\"/var/lib/takaro\"}"), "/var/lib/takaro");

    // The shipped takaro.json is the compiled-in template.
    std::string shipped;
    bool exists = false;
    std::string err;
    CHECK(ReadWholeFile("takaro.json", shipped, exists, err) && exists, "native/takaro.json readable");
    EQ(shipped, kConfigTemplate);

    std::string a = NewIdentity(), b = NewIdentity();
    CHECK(a.size() == 36 && a[14] == '4' && a != b, "uuid v4 %s", a.c_str());
}

static void TestSetJsonString() {
    using namespace takaro;
    auto set = [](const std::string& t, const std::string& k, const std::string& v) {
        auto r = SetJsonString(t, k, v);
        return r ? *r : std::string("<none>");
    };
    EQ(set("{\n  \"a\": \"1\",\n  \"identityToken\": \"\",\n  \"b\": [1, {\"identityToken\": 2}]\n}\n", "identityToken",
           "x-1"),
       "{\n  \"a\": \"1\",\n  \"identityToken\": \"x-1\",\n  \"b\": [1, {\"identityToken\": 2}]\n}\n");
    EQ(set("{\r\n  \"a\": \"1\"\r\n}\r\n", "name", "N \"q\""), "{\r\n  \"a\": \"1\",\r\n  \"name\": \"N \\\"q\\\"\"\r\n}\r\n");
    EQ(set("{}", "name", "n"), "{\n  \"name\": \"n\"\n}");
    EQ(set("{\"b\":{\"name\":\"inner\"},\"x\":\"\\\"}\"}", "name", "n"),
       "{\"b\":{\"name\":\"inner\"},\"x\":\"\\\"}\",\n  \"name\": \"n\"}");
    EQ(set(kConfigTemplate, "identityToken", "abc"),
       std::string(kConfigTemplate).replace(std::string(kConfigTemplate).find("\"identityToken\": \"\"") + 17, 2,
                                            "\"abc\""));
    EQ(set("{\"name\": 5}", "name", "n"), "<none>");
    EQ(set("{\"name\": ", "name", "n"), "<none>");
    EQ(set("[]", "name", "n"), "<none>");
}

static void TestConfigWatcher() {
    using namespace takaro;
    char tmpl[] = "/tmp/conan-watch-XXXXXX";
    std::string root = mkdtemp(tmpl);
    std::string savedDir = root + "/Saved";
    std::map<std::string, std::string> envs;
    EnvFn env = [&](const char* n) { return envs.count(n) ? envs[n] : std::string(); };
    std::string err, text;
    bool exists = false;
    auto read = [&](const std::string& p) {
        text.clear();
        ReadWholeFile(p, text, exists, err);
        return exists ? text : std::string("<missing>");
    };
    ConfigWatcher::Options o;
    o.savedDir = savedDir;
    o.env = env;
    o.newIdentity = [] { return std::string("feedc0de-1111-4111-8111-111111111111"); };
    o.pollMs = 10;
    o.settleMs = 5;

    // Fresh: no file -> created from the template with the generated identity and name; held.
    ConfigWatcher w(o);
    Config c = w.Load();
    CHECK(c.hold == Hold::NoToken, "%s", c.disabledReason.c_str());
    const std::string file = savedDir + "/Config/Takaro/takaro.json";
    EQ(w.FilePath(), file);
    std::string f = read(file);
    CHECK(f.find("\"identityToken\": \"feedc0de-1111-4111-8111-111111111111\"") != std::string::npos &&
              f.find("\"name\": \"Conan Exiles (feedc0de)\"") != std::string::npos &&
              f.find("\"registrationToken\": \"\"") != std::string::npos,
          "file created with identity: %s", f.c_str());
    struct stat st {};
    CHECK(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "created 0600: %o", st.st_mode & 0777);
    std::string savedCopy = read(savedDir + "/Takaro/state/saved-settings.json");
    CHECK(savedCopy.find("feedc0de-1111") != std::string::npos && savedCopy.find("\"registrationToken\":\"\"") !=
                                                                         std::string::npos,
          "saved copy has the identity, no token: %s", savedCopy.c_str());

    // Token pasted while running: applied after the settle read, not before.
    chmod(file.c_str(), 0640);
    std::string withToken = *SetJsonString(f, "registrationToken", "reg-AAAA-1111");
    CHECK(ReplaceUserFile(file, withToken, 0600, err), "%s", err.c_str());
    Config next;
    CHECK(!w.Poll(100, next), "first sight of a change waits for the settle read");
    CHECK(w.Poll(105, next) && next.enabled && next.registrationToken == "reg-AAAA-1111", "token applied");
    CHECK(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0640, "mode kept on rewrite: %o", st.st_mode & 0777);
    CHECK(read(savedDir + "/Takaro/state/saved-settings.json").find("reg-AAAA") == std::string::npos,
          "token not saved before Takaro accepts it");
    w.Identified(next);
    CHECK(read(savedDir + "/Takaro/state/saved-settings.json").find("reg-AAAA-1111") != std::string::npos,
          "token saved after identify");
    // Unchanged text: no reload. Half-saved text: ignored, settings kept.
    CHECK(!w.Poll(200, next) && !w.Poll(300, next), "unchanged");
    CHECK(ReplaceUserFile(file, "{\"registrationToken\": \"reg-B", 0600, err), "half");
    CHECK(!w.Poll(400, next) && !w.Poll(405, next), "half-saved ignored");
    // The template copied over takaro.json (a mistaken upgrade): token and identity come back from the
    // saved copy, identity is written into the file, the token is not; no reconnect (same settings).
    CHECK(ReplaceUserFile(file, kConfigTemplate, 0600, err), "replace");
    CHECK(!w.Poll(500, next), "settle");
    CHECK(!w.Poll(505, next), "same settings from the saved copy: no reconnect");
    f = read(file);
    CHECK(f.find("feedc0de-1111") != std::string::npos && f.find("reg-AAAA") == std::string::npos,
          "identity written back, token not: %s", f.c_str());

    // A restart of that install: identity from the file, token from the saved copy.
    ConfigWatcher w2(o);
    c = w2.Load();
    CHECK(c.enabled && c.identitySource == Source::File && c.registrationSource == Source::Saved, "%s",
          ConfigSummaryJson(c).c_str());

    // Prior install (state dir present, no saved copy) whose file lost its identity: held, never generated.
    unlink((savedDir + "/Takaro/state/saved-settings.json").c_str());
    CHECK(ReplaceUserFile(file, "{\"registrationToken\": \"reg-C\"}", 0600, err), "w");
    ConfigWatcher w3(o);
    c = w3.Load();
    CHECK(c.hold == Hold::NoIdentity, "%s", c.disabledReason.c_str());
    CHECK(read(file).find("feedc0de") == std::string::npos, "no identity invented for a prior install");

    // Environment-only install: no file created, no saved copy.
    std::string root2 = root + "/envonly";
    o.savedDir = root2 + "/Saved";
    envs["TAKARO_IDENTITY_TOKEN"] = "env-id";
    envs["TAKARO_REGISTRATION_TOKEN"] = "env-reg";
    ConfigWatcher w4(o);
    c = w4.Load();
    CHECK(c.enabled && read(o.savedDir + "/Config/Takaro/takaro.json") == "<missing>" &&
              read(o.savedDir + "/Takaro/state/saved-settings.json") == "<missing>",
          "env-only install writes nothing");
    w4.Identified(c);
    CHECK(read(o.savedDir + "/Takaro/state/saved-settings.json") == "<missing>", "still nothing after identify");

    // Identify errors: name/message/status only; JWTs and our tokens scrubbed.
    JsonValue e;
    JsonParse("{\"name\":\"BadRequestError\",\"message\":\"Invalid token env-reg\",\"http\":400,"
              "\"meta\":{\"request\":{\"headers\":{\"x-takaro-token\":\"eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.abc\"}}}}",
              e);
    bool taken = true;
    EQ(DescribeTakaroError(&e, c, &taken), "BadRequestError: Invalid token [redacted] (HTTP 400)");
    CHECK(!taken, "not a name conflict");
    JsonParse("{\"message\":\"Request failed with status code 409\"}", e);
    DescribeTakaroError(&e, c, &taken);
    CHECK(taken, "409 is a name conflict");
    JsonParse("\"bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.sig\"", e);
    EQ(DescribeTakaroError(&e, c), "bearer [redacted]");
}

static void TestOutbox() {
    using namespace takaro;
    char tmpl[] = "/tmp/conan-outbox-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    {
        Store s(dir + "/state");
        std::string notes = s.Load();
        CHECK(notes.find("created") != std::string::npos && s.OutboxUsable(), "%s", notes.c_str());
        for (int i = 1; i <= 3; i++)
            CHECK(s.Admit("chat-message", CreateGameEvent("chat-message", J("{\"msg\":\"m" + std::to_string(i) + "\"}"))) ==
                      (uint64_t)i,
                  "ids");
        CHECK(s.SaveOutbox(), "save");
        CHECK(s.ConfirmThrough(1) == 1 && s.Outbox().pending.size() == 2, "confirm 1");
        CHECK(s.ConfirmThrough(1) == 0, "confirm is idempotent");
        CHECK(s.SaveOutbox(), "save");
    }
    {
        Store s(dir + "/state");
        std::string notes = s.Load();
        CHECK(s.Outbox().pending.size() == 2 && s.Outbox().pending.front().id == 2 && s.Outbox().nextId == 4,
              "reload keeps the unconfirmed tail: %s", notes.c_str());
        CHECK(s.Outbox().confirmedTotal == 1, "confirmed total persisted");
    }
    {
        std::string err;
        AtomicWriteFile(dir + "/state/event-outbox.json", "{\"version\":1,\"nextId\":", err);
        Store s(dir + "/state");
        s.Load();
        CHECK(!s.OutboxUsable() && s.Errors().count("outbox"), "corrupt outbox is fenced");
        CHECK(!s.SaveOutbox(), "fenced outbox is never overwritten");
        std::string text;
        bool exists;
        ReadWholeFile(dir + "/state/event-outbox.json", text, exists, err);
        EQ(text, "{\"version\":1,\"nextId\":");
    }
    {
        Store s(dir + "/full");
        s.Load();
        for (size_t i = 0; i < Store::kMaxPendingEvents; i++) s.Admit(i == 10 ? "log" : "chat-message", "{}");
        s.Admit("player-death", "{}");
        CHECK(s.Outbox().losses == 1 && s.Outbox().pending.size() == Store::kMaxPendingEvents, "bounded");
        bool logLeft = false;
        for (auto& e : s.Outbox().pending) logLeft = logLeft || e.type == "log";
        CHECK(!logLeft, "the log event is dropped first");
    }
    std::string text = OutboxJson(OutboxState());
    OutboxState o;
    std::string err;
    CHECK(ParseOutbox(text, o, err), "%s", err.c_str());
    CHECK(!ParseOutbox("{\"version\":2,\"nextId\":1,\"pending\":[]}", o, err), "version");
    CHECK(!ParseOutbox("{\"version\":1,\"nextId\":3,\"pending\":[{\"id\":2,\"type\":\"log\",\"frame\":\"{}\"},"
                       "{\"id\":1,\"type\":\"log\",\"frame\":\"{}\"}]}",
                       o, err),
          "order");
}

static void TestQueuesAndHeartbeat() {
    using namespace takaro;
    FrameQueues q;
    auto f = [](FrameKind k, const std::string& t) {
        OutFrame o;
        o.kind = k;
        o.text = std::make_shared<const std::string>(t);
        return o;
    };
    q.Push(f(FrameKind::Event, "e"));
    q.Push(f(FrameKind::Response, "r"));
    q.Push(f(FrameKind::Control, "c"));
    q.Push(f(FrameKind::CriticalResponse, "x"));
    std::string order;
    OutFrame out;
    while (q.Pop(out)) order += *out.text;
    EQ(order, "cxre");
    CHECK(q.Push(f(FrameKind::Response, std::string(kMaxOutboundFrame + 1, 'a'))) == QueueStatus::TooLarge, "too large");
    for (int i = 0; i < 64; i++) q.Push(f(FrameKind::Control, "c"));
    CHECK(q.Push(f(FrameKind::Control, "c")) == QueueStatus::Full, "control bound");

    Heartbeat::Params p;
    Heartbeat h(p);
    h.Reset(0);
    CHECK(!h.PingDue(100, true), "nothing to confirm yet");
    h.OnEventWritten(3);
    CHECK(!h.PingDue(500, true) && h.PingDue(1000, true), "eager ping 1 s after a burst");
    CHECK(h.NextPingId() == 1, "first ping id");
    h.OnPingSent(1000);  // ping 1 covers events <= 3
    h.OnEventWritten(5);
    h.OnPingSent(2000);  // ping 2 covers <= 5
    h.OnEventWritten(9);
    h.OnPingSent(3000);  // ping 3 covers <= 9
    CHECK(h.OnPongId(7) == 0 && h.Outstanding() == 3, "unknown pong confirms nothing");
    CHECK(h.OnPongId(2) == 5 && h.Outstanding() == 1, "pong 2 confirms through 5 and retires ping 1");
    CHECK(h.OnPongId(1) == 0, "an already retired ping confirms nothing");
    CHECK(h.Stalled(3000 + p.intervalMs) == false, "one outstanding is not stalled");
    h.OnPingSent(4000);
    CHECK(h.Stalled(4000 + p.intervalMs) && !h.Stalled(4000 + p.intervalMs - 1), "two unanswered for an interval");
    CHECK(h.OnPongId(4) == 9 && h.Confirmed() == 9, "pong 4 confirms through 9");
    h.OnInbound(4000);
    CHECK(!h.Dead(4000 + p.idleMs) && h.Dead(4001 + p.idleMs), "idle watchdog");
}

static void TestPins() {
    std::vector<int> pat;
    CHECK(pins::ParsePattern("48 8b ?? 05", pat) && pat.size() == 4 && pat[2] == -1 && pat[1] == 0x8b, "parse");
    CHECK(!pins::ParsePattern("?? 48", pat), "leading wildcard refused");
    CHECK(!pins::ParsePattern("4", pat) && !pins::ParsePattern("zz", pat) && !pins::ParsePattern("", pat), "bad");
    CHECK(!pins::ParsePattern("488B", pat), "tokens need spaces");

    // A synthetic image holding the three linux signatures at known places.
    std::vector<uint8_t> img(0x4000, 0x90);
    const uintptr_t base = 0x400000;
    auto put = [&](size_t off, const std::string& pattern, std::map<size_t, uint32_t> fields) {
        std::vector<int> p;
        pins::ParsePattern(pattern, p);
        for (size_t i = 0; i < p.size(); i++) img[off + i] = p[i] < 0 ? 0xEE : (uint8_t)p[i];
        for (auto& kv : fields) memcpy(&img[off + kv.first], &kv.second, 4);
    };
    const auto& sigs = pins::SignaturesFor("linux");
    CHECK(sigs.size() == 3, "3 linux signatures");
    put(0x100, sigs[0].pattern, {{43, (uint32_t)(0x700000 - (base + 0x100 + 43 + 4))}});
    put(0x2000, sigs[2].pattern, {{9, 0x6a0000}});
    std::vector<pins::Region> regions = {{img.data(), img.size(), base}};
    pins::Result r = pins::Resolve("linux", "unknown-build", regions, false);
    CHECK(!r.ok && r.reason.find("unsupported server build (unknown-build)") != std::string::npos, "%s", r.reason.c_str());
    CHECK(r.anchors.processEvent == base + 0x100 && r.anchors.objObjects == 0x700000 && r.anchors.nameBlocks == 0x6a0000,
          "anchors %llx %llx %llx", (unsigned long long)r.anchors.processEvent,
          (unsigned long long)r.anchors.objObjects, (unsigned long long)r.anchors.nameBlocks);
    r = pins::Resolve("linux", "unknown-build", regions, true);
    CHECK(r.ok, "allowUnpinned accepts a clean scan of an unpinned build");
    r = pins::Resolve("linux", pins::PinnedBuilds()[0].buildId, regions, false);
    CHECK(!r.ok && r.reason.find("disagrees") != std::string::npos, "pinned build with other addresses refused: %s",
          r.reason.c_str());
    put(0x3000, sigs[2].pattern, {{9, 0x6a0000}});  // a second match
    r = pins::Resolve("linux", "unknown-build", regions, true);
    CHECK(!r.ok && r.reason.find("more than one match") != std::string::npos, "%s", r.reason.c_str());
    std::vector<pins::Region> none;
    r = pins::Resolve("linux", "x", none, true);
    CHECK(!r.ok && r.reason.find("signature scan failed (processEvent: no match)") != std::string::npos, "%s",
          r.reason.c_str());
    r = pins::Resolve("linux", "x", none, false);
    CHECK(!r.ok && r.reason.find("unsupported server build (x)") == 0 && r.reason.find("no match") != std::string::npos,
          "%s", r.reason.c_str());
    r = pins::Resolve("freebsd", "x", regions, true);
    CHECK(!r.ok && r.reason.find("no signatures for platform freebsd") != std::string::npos, "%s", r.reason.c_str());

    // Windows: ASLR relocates the image, so the pinned anchors are RVAs. Three code sites of the pinned
    // build, placed at their real RVAs under two different bases, each with a rip32 capture.
    const auto& wsigs = pins::SignaturesFor("windows");
    CHECK(wsigs.size() == 3, "3 windows signatures");
    const pins::BuildPin* wpin = nullptr;
    for (auto& b : pins::PinnedBuilds())
        if (std::string(b.platform) == "windows" && std::string(b.build) == "25639945") wpin = &b;
    CHECK(wpin && std::string(wpin->buildId) == "a0e8d0c4-b619000", "windows build pinned by PE identity");
    for (uintptr_t wbase : {(uintptr_t)0x140000000ull, (uintptr_t)0x7ff7b2f10000ull}) {
        std::vector<std::vector<uint8_t>> bufs(3, std::vector<uint8_t>(0x100, 0xCC));
        const uintptr_t siteRva[3] = {0x16408d0, 0x15494c9, 0x145ff52};
        const uintptr_t target[3] = {0, 0xa92cdc0, 0xa85e010};
        std::vector<pins::Region> wregions;
        for (int i = 0; i < 3; i++) {
            std::vector<int> p;
            pins::ParsePattern(wsigs[i].pattern, p);
            for (size_t k = 0; k < p.size(); k++) bufs[i][0x40 + k] = p[k] < 0 ? 0xEE : (uint8_t)p[k];
            uintptr_t site = wbase + siteRva[i];
            if (wsigs[i].capture == pins::Capture::Rip32) {
                int32_t disp = (int32_t)((wbase + target[i]) - (site + wsigs[i].offset + 4));
                memcpy(&bufs[i][0x40 + wsigs[i].offset], &disp, 4);
            }
            wregions.push_back({bufs[i].data(), bufs[i].size(), site - 0x40});
        }
        r = pins::Resolve("windows", "a0e8d0c4-b619000", wregions, false, wbase);
        CHECK(r.ok && r.build == "25639945", "windows pinned build verified at base %llx: %s",
              (unsigned long long)wbase, r.reason.c_str());
        CHECK(r.anchors.processEvent == wbase + 0x16408d0 && r.anchors.objObjects == wbase + 0xa92cdc0 &&
                  r.anchors.nameBlocks == wbase + 0xa85e010,
              "windows anchors are absolute in the relocated image");
        r = pins::Resolve("windows", "a0e8d0c4-b619000", wregions, false, 0);
        CHECK(!r.ok && r.reason.find("disagrees") != std::string::npos, "unrelocated comparison refused");
        r = pins::Resolve("windows", "deadbeef-1000", wregions, false, wbase);
        CHECK(!r.ok && r.reason.find("unsupported server build (deadbeef-1000)") == 0, "%s", r.reason.c_str());
    }
    // A pattern straddling the end of a region must not read past it.
    std::vector<uint8_t> tail(10, 0x89);
    std::vector<pins::Region> small = {{tail.data(), tail.size(), 0x1000}};
    std::vector<int> p2;
    pins::ParsePattern(sigs[2].pattern, p2);
    CHECK(pins::FindMatches(small, p2, 2).empty(), "short region");
}

static void TestMaps() {
    std::string maps =
        "00400000-03ac8000 r--p 00000000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "03ac8000-0c106000 r-xp 036c7000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "0c10d000-0c228000 rw-p 0bf0b000 08:01 123 /srv/conan/ConanSandboxServer-Linux-Shipping\n"
        "7f0000000000-7f0000001000 r-xp 00000000 08:01 9 /usr/lib/libc.so.6\n"
        "7f0000002000-7f0000003000 r-xp 00000000 00:00 0 \n";
    auto r = linuxplat::ExecutableRegions("/srv/conan/ConanSandboxServer-Linux-Shipping", maps);
    CHECK(r.size() == 1 && r[0].address == 0x3ac8000 && r[0].size == 0x0c106000 - 0x3ac8000, "one text mapping");
    auto own = linuxplat::ExecutableRegions("/proc/self/exe");
    CHECK(own.empty(), "the path must be the real one, not /proc/self/exe");
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    own = linuxplat::ExecutableRegions(std::string(buf, n > 0 ? (size_t)n : 0));
    CHECK(!own.empty(), "this test binary has an executable mapping");
}

static void TestAdapter() {
    ChatRequest req;
    std::string err;
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"opts\":{\"recipient\":{\"gameId\":\"76561198000000001\"},"
                             "\"senderNameOverride\":\" Bob \"}}"),
                           req, err),
          "%s", err.c_str());
    EQ(req.recipient, "76561198000000001");
    EQ(req.sender, "Bob");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"player\":{\"platformId\":\"steam:76561198000000002\"}}"), req, err),
          "nested");
    EQ(req.recipient, "76561198000000002");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"recipient\":\"werwerwer\"}"), req, err), "string recipient");
    EQ(req.recipient, "werwerwer");
    EQ(req.sender, "Takaro");
    CHECK(ParseSendMessage(J("{\"message\":\"hi\",\"opts\":null,\"gameId\":null}"), req, err) && req.recipient.empty(),
          "explicit nulls mean global");
    CHECK(!ParseSendMessage(J("{\"message\":\"   \"}"), req, err), "blank message");
    CHECK(!ParseSendMessage(J("{}"), req, err), "missing message");

    AdapterOptions ok;
    ok.ready = true;
    int sent = 0;
    ok.chat = [&](const ChatRequest&) {
        ChatOutcome o;
        o.success = true;
        sent++;
        return o;
    };
    Adapter a(ok);
    std::vector<takaro::GameEvent> evs;
    a.DrainEvents(evs);
    CHECK(evs.empty(), "a ready adapter sends no notice");
    auto r = a.Execute("testReachability", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload) == "{\"connectable\":true,\"reason\":null}", "reachable");
    r = a.Execute("sendMessage", J("{\"message\":\"x\"}"));
    CHECK(r.ok && sent == 1 && r.payload.type == JsonValue::Null, "sendMessage ok answers {}");
    r = a.Execute("giveItem", J("{}"));  // no game backend in this adapter (savedDir unset): structured error
    CHECK(!r.ok && r.error.find("giveItem") == 0, "%s", r.error.c_str());
    r = a.Execute("getMapInfo", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload).find("\"enabled\":false") != std::string::npos, "map info");
    r = a.Execute("bogus", J("{}"));
    CHECK(!r.ok && r.error == "unknown action 'bogus'", "unknown");

    AdapterOptions no;
    no.refusal = "unsupported server build (abc)";
    Adapter b(no);
    b.DrainEvents(evs);
    CHECK(evs.size() == 1 && evs[0].type == "log" &&
              takaro::JsonDump(evs[0].data).find("CRITICAL: unsupported server build (abc)") != std::string::npos,
          "one critical notice");
    evs.clear();
    b.DrainEvents(evs);
    CHECK(evs.empty(), "only one");
    r = b.Execute("testReachability", J("{}"));
    CHECK(r.ok && takaro::JsonDump(r.payload).find("\"connectable\":false") != std::string::npos, "not reachable");
    for (int i = 0; i < takaro::kActionCount; i++) {
        std::string act = takaro::kActions[i];
        if (act == "testReachability") continue;
        r = b.Execute(act, J("{\"message\":\"x\"}"));
        CHECK(!r.ok && r.error.find("refused: unsupported server build") != std::string::npos, "%s: %s", act.c_str(),
              r.error.c_str());
    }
}

static void TestCoverage() {
    for (int i = 0; i < takaro::kActionCount; i++)
        CHECK(ActionCoverage(takaro::kActions[i]) != nullptr, "action %s covered", takaro::kActions[i]);
    for (int i = 0; i < takaro::kEventTypeCount; i++)
        CHECK(EventCoverage(takaro::kEventTypes[i]) != nullptr, "event %s covered", takaro::kEventTypes[i]);
    CHECK(std::string(ActionCoverage("sendMessage")->implementation) == "native", "chat native");
    CHECK(std::string(ActionCoverage("getMapInfo")->status) == "schema-fallback", "map fallback");
    CHECK(std::string(ActionCoverage("getMapTile")->status) == "unsupported", "tile unsupported");
    for (auto* a : {"getPlayers", "getPlayer", "getPlayerLocation", "getPlayerInventory", "listItems", "listEntities",
                    "listLocations", "giveItem", "teleportPlayer", "executeConsoleCommand", "kickPlayer", "banPlayer",
                    "unbanPlayer", "listBans", "shutdown"})
        CHECK(std::string(ActionCoverage(a)->implementation) == "native", "%s native", a);
    JsonValue v;
    CHECK(JsonParse(RegistryJson(), v), "registry json");
}

static void TestRecipient() {
    CHECK(IsSteam64("76561198000000001"), "steam64");
    CHECK(!IsSteam64("7656119800000000"), "short");
    CHECK(!IsSteam64("86561198000000001"), "prefix");
    CHECK(!IsSteam64("7656119800000000a"), "digits");
    EQ(NormalizeRecipient("steam:76561198000000001"), "76561198000000001");
    EQ(NormalizeRecipient(" 76561198000000001 "), "76561198000000001");
    EQ(NormalizeRecipient("some:name"), "some:name");
    EQ(NormalizeRecipient(""), "");
    CHECK(EqualsIgnoreCase("WerWer", "werwer") && !EqualsIgnoreCase("werwer", "werwe"), "ignore case");
}

static void TestText() {
    CHECK(ChatText("hello") == u"hello", "ascii");
    CHECK(ChatText("h\xc3\xa9llo") == u"h\u00e9llo", "2-byte");
    CHECK(ChatText("\xe2\x82\xac") == u"\u20ac", "3-byte");
    CHECK(ChatText("\xf0\x9f\x98\x80") == u"\U0001F600", "4-byte -> surrogate pair");
    CHECK(ChatText("a\xffz") == u"a\uFFFDz", "invalid byte");
    CHECK(ChatText("a\xc3") == u"a\uFFFD", "truncated sequence");
    CHECK(ChatText("\xc0\xaf") == u"\uFFFD\uFFFD", "overlong");
    CHECK(ChatText("\xed\xa0\x80") == u"\uFFFD\uFFFD\uFFFD", "encoded surrogate");
    CHECK(ChatText("a\nb\tc\rd") == u"a b c d", "whitespace");
    CHECK(ChatText(std::string("a\x01" "b\x7f" "c")) == u"abc", "control chars dropped");
    CHECK(ChatText(std::string(5000, 'x')).size() == kMaxMessageChars, "capped");
    CHECK(ChatText("abc\xf0\x9f\x98\x80", 4) == u"abc", "no half surrogate at the cap");
    EQ(Utf16To8(u"h\u00e9\u20ac\U0001F600"), "h\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
    EQ(Utf16To8(std::u16string(1, (char16_t)0xD800)), "\xef\xbf\xbd");
}

static void TestChatRpc() {
    CHECK(FileTimeTicks(0, 0) == 116444736000000000ULL, "unix epoch");
    CHECK(FileTimeTicks(1, 500) == 116444736000000000ULL + 10000000ULL + 5, "1s + 500ns");
    // The spike captured a real player message at 0x01dd529b80c4e16f on 2026-10-02 (UTC).
    uint64_t real = 0x01dd529b80c4e16fULL;
    int64_t sec = (int64_t)((real - FileTimeTicks(0, 0)) / 10000000ULL);
    CHECK(sec >= 1790899200 && sec < 1790985600, "captured timestamp is 2026-10-02: %lld", (long long)sec);

    std::u16string user = u"Takaro", channel = u"Global", msg = u"hello";
    alignas(16) uint8_t buf[ChatRpc::kSize];
    memset(buf, 0xAB, sizeof buf);
    PackChatRpc(buf, 0x1122334455667788ULL, user, channel, msg);
    uint64_t ts;
    memcpy(&ts, buf, 8);
    CHECK(ts == 0x1122334455667788ULL, "timestamp");
    for (size_t i = 8; i < ChatRpc::kUserName; i++) CHECK(buf[i] == 0, "ids zeroed at %zu", i);
    struct { size_t off; const std::u16string* s; } fields[] = {
        {ChatRpc::kUserName, &user}, {ChatRpc::kChannel, &channel}, {ChatRpc::kMessage, &msg}};
    for (auto& f : fields) {
        const char16_t* p;
        int32_t num, max;
        memcpy(&p, buf + f.off, 8);
        memcpy(&num, buf + f.off + 8, 4);
        memcpy(&max, buf + f.off + 12, 4);
        CHECK(p == f.s->c_str(), "FString data at %zx", f.off);
        CHECK(num == (int32_t)f.s->size() + 1 && max == num, "FString Num/Max include NUL at %zx", f.off);
        CHECK(p[num - 1] == 0, "NUL terminated");
    }
    CHECK(buf[ChatRpc::kGenerated] == 0, "generated false");
    for (size_t i = ChatRpc::kGenerated + 1; i < ChatRpc::kSize; i++) CHECK(buf[i] == 0, "tail zeroed");
}

static void TestBuildId() {
    std::string id = linuxplat::ReadElfBuildId("/proc/self/exe");
    CHECK(id.size() == 40 && id.find_first_not_of("0123456789abcdef") == std::string::npos, "own build-id '%s'",
          id.c_str());
    CHECK(linuxplat::ReadElfBuildId("/nonexistent").empty(), "missing file");
    CHECK(linuxplat::ReadElfBuildId("/proc/self/status").empty(), "not ELF");
}

static void TestGtStats() {
    using namespace GtStats;
    ResetForTests();
    // No sample yet: both windows null, the sampler not started.
    JsonValue v;
    CHECK(JsonParse(HealthJson(), v), "gt health json");
    CHECK(v.get("window") && v.get("window")->type == JsonValue::Null, "no window before samples");
    CHECK(v.get("sinceStart") && v.get("sinceStart")->type == JsonValue::Null, "no totals before samples");

    // Pure window math.
    Snap a, b;
    a.monoNs = 1000000000ULL;
    a.frames = 1000;
    a.detourCalls = 1024 * 10;
    a.detourSamples = 10;
    a.detourSampleNs = 10 * 50;  // 50 ns per call
    a.drainNs = 100000;
    b = a;
    b.monoNs = a.monoNs + 10000000000ULL;  // 10 s
    b.frames = a.frames + 300;             // 30 ticks/s
    b.detourCalls = a.detourCalls + 1024 * 100;
    b.detourSamples = a.detourSamples + 100;
    b.detourSampleNs = a.detourSampleNs + 100 * 100;  // 100 ns per call in this window
    b.drainNs = a.drainNs + 600000;                   // 600 us
    b.drainJobs = 12;
    b.hookNs = 300000;  // 300 us
    b.hookCalls = 7;
    b.maxDrainNs = 250000;
    Window w = Compute(a, b);
    CHECK(w.frames == 300, "frames %lld", (long long)w.frames);
    CHECK(w.seconds > 9.99 && w.seconds < 10.01, "seconds %f", w.seconds);
    CHECK(w.detourCalls == 102400, "calls");
    CHECK(w.hotPathUs > 10239 && w.hotPathUs < 10241, "hot path %f", w.hotPathUs);  // 102400 x 100 ns
    CHECK(w.drainUs > 599.9 && w.drainUs < 600.1, "drain %f", w.drainUs);
    CHECK(w.hookUs > 299.9 && w.hookUs < 300.1, "hook %f", w.hookUs);
    CHECK(w.totalUs > 11139 && w.totalUs < 11141, "total %f", w.totalUs);
    CHECK(w.usPerTick > 37.13 && w.usPerTick < 37.14, "us per tick %f", w.usPerTick);
    CHECK(w.ticksPerSecond > 29.99 && w.ticksPerSecond < 30.01, "tps %f", w.ticksPerSecond);
    CHECK(w.maxDrainUs > 249.9 && w.maxDrainUs < 250.1, "max drain %f", w.maxDrainUs);
    // Unknown frame count: usPerTick unknown, written as null.
    Snap c = b;
    c.frames = -1;
    Window wu = Compute(a, c);
    CHECK(wu.frames == -1 && wu.usPerTick < 0, "unknown frames");
    CHECK(JsonParse(WindowJson(wu), v) && v.get("usPerTick")->type == JsonValue::Null &&
              v.get("frames")->type == JsonValue::Null,
          "null usPerTick json");
    // Since start (zero baseline): frames = the engine's own count.
    Window ws = Compute(Snap(), b);
    CHECK(ws.frames == 1300 && ws.usPerTick > 0, "since start frames %lld", (long long)ws.frames);

    // The counters through RecordSample: two samples make a window with exactly the added cost.
    AddDetourCalls(2048);
    AddDetourSample(80);
    AddDetourSample(120);  // 100 ns per call
    AddDrain(40000, 2);
    AddDrain(10000, 1);
    AddHook(5000);
    RecordSample(500, 1000000000ULL);
    AddDetourCalls(1024);
    AddDetourSample(100);
    AddDrain(20000, 1);
    AddHook(1000);
    AddHook(2000);
    RecordSample(530, 11000000000ULL);
    CHECK(JsonParse(HealthJson(), v), "gt health json 2");
    const JsonValue* win = v.get("window");
    CHECK(win && win->type == JsonValue::Object, "window object");
    if (win && win->type == JsonValue::Object) {
        CHECK(win->get("frames")->num == 30, "window frames");
        CHECK(win->get("drainUs")->num == 20, "window drain %f", win->get("drainUs")->num);
        CHECK(win->get("drainJobs")->num == 1, "window jobs");
        CHECK(win->get("hookUs")->num == 3, "window hook");
        CHECK(win->get("hookCalls")->num == 2, "window hook calls");
        CHECK(win->get("maxDrainUs")->num == 20, "window max drain (reset per window)");
        CHECK(win->get("detourCalls")->num == 1024, "window calls");
        // 1024 calls x 100 ns = 102.4 us, + 20 + 3 = 125.4 us over 30 ticks
        CHECK(win->get("usPerTick")->num > 4.17 && win->get("usPerTick")->num < 4.19, "window us/tick %f",
              win->get("usPerTick")->num);
    }
    const JsonValue* tot = v.get("sinceStart");
    CHECK(tot && tot->type == JsonValue::Object && tot->get("frames")->num == 530 &&
              tot->get("drainJobs")->num == 4 && tot->get("maxDrainUs")->num == 40,
          "since start totals");
    // Since start counts from the library load (here: the first sample's own clock), not from boot.
    CHECK(tot && tot->get("seconds") && tot->get("seconds")->num > 9.99 && tot->get("seconds")->num < 10.01,
          "since start seconds %f", tot && tot->get("seconds") ? tot->get("seconds")->num : -1.0);
    CHECK(v.get("maxDrainUs")->num == 40 && v.get("samples")->num == 2, "overall max and samples");
    ResetForTests();
}

int main() {
    TestProtocol();
    TestConfig();
    TestSetJsonString();
    TestConfigWatcher();
    TestOutbox();
    TestQueuesAndHeartbeat();
    TestPins();
    TestMaps();
    TestAdapter();
    TestCoverage();
    TestRecipient();
    TestText();
    TestChatRpc();
    TestBuildId();
    TestGtStats();
    printf("unit tests: %d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
