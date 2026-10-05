// Unit tests for the native connector building blocks.
#include "native/config.h"
#include "native/fileio.h"
#include "native/json_util.h"
#include "native/logtail.h"
#include "native/mapping.h"
#include "native/persistence.h"
#include "native/transport.h"
#include "testlib.h"

#include <map>

using namespace native;

namespace {

EnvFn Env(std::map<std::string, std::string> m) {
    return [m](const char* k) {
        auto it = m.find(k);
        return it == m.end() ? std::string() : it->second;
    };
}

void JsonUtilTests() {
    t::Group("json-util");
    CHECK_EQ(NumText(1), std::string("1"));
    CHECK_EQ(NumText(1.5), std::string("1.5"));
    CHECK_EQ(NumText(0.1), std::string("0.1"));
    CHECK_EQ(NumText(-0.25), std::string("-0.25"));
    CHECK_EQ(NumText(1e21), std::string("1e+21"));
    CHECK_EQ(NumText(1e-7), std::string("1e-7"));
    CHECK_EQ(NumText(123456789012345680000.0), std::string("123456789012345680000"));
    CHECK_EQ(NumText(0.000001), std::string("0.000001"));
    CHECK_EQ(NumText(10.5), std::string("10.5"));
    CHECK_EQ(NumText(1e3), std::string("1000"));
    JsonValue v = t::J(R"({"a":76561198000005875,"b":" x ","c":"","d":"12.5","e":null,"f":true})");
    CHECK_EQ(Str(v.get("a")).value_or("?"), std::string("76561198000005875"));  // 64-bit id kept exact
    CHECK_EQ(Str(v.get("b")).value_or("?"), std::string("x"));
    CHECK(!Str(v.get("c")));
    CHECK(!Str(v.get("e")));
    CHECK(!Str(v.get("f")));
    CHECK(Num(v.get("d")) && *Num(v.get("d")) == 12.5);
    CHECK(!Num(v.get("b")));
    CHECK(JsonDepthExceeds(std::string(65, '[') + std::string(65, ']'), 64));
    CHECK(!JsonDepthExceeds(std::string(64, '[') + std::string(64, ']'), 64));
    CHECK(!JsonDepthExceeds("\"[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[\"", 64));
    JsonValue deep;
    CHECK(!ParseJson(std::string(65, '[') + std::string(65, ']'), deep));  // parser refuses depth > 64
    CHECK(ParseJson(std::string(64, '[') + std::string(64, ']'), deep));
    CHECK_EQ(EncodeUriComponent("a b/c?d=é"), std::string("a%20b%2Fc%3Fd%3D%C3%A9"));
}

void IsoTests() {
    t::Group("iso-dates");
    int64_t ms = 0;
    CHECK(ParseIsoMs("2030-01-01T00:00:00.000Z", ms) && ms == 1893456000000LL);
    CHECK_EQ(FormatIsoMs(1893456000000LL), std::string("2030-01-01T00:00:00.000Z"));
    CHECK(ParseIsoMs("2026-09-29T12:34:56Z", ms) && FormatIsoMs(ms) == "2026-09-29T12:34:56.000Z");
    CHECK(ParseIsoMs("2026-09-29T14:34:56.5+02:00", ms) && FormatIsoMs(ms) == "2026-09-29T12:34:56.500Z");
    CHECK(ParseIsoMs("2026-09-29", ms) && FormatIsoMs(ms) == "2026-09-29T00:00:00.000Z");
    CHECK(ParseIsoMs("1893456000000", ms) && ms == 1893456000000LL);
    CHECK(!ParseIsoMs("2026-09-29T12:34:56", ms));  // no zone: refused
    CHECK(!ParseIsoMs("tomorrow", ms));
    CHECK(!ParseIsoMs("2026-13-01T00:00:00Z", ms));
    CHECK_EQ(FormatIsoMs(0), std::string("1970-01-01T00:00:00.000Z"));
}

void HeartbeatTests() {
    t::Group("heartbeat");
    Heartbeat::Params p;
    p.intervalMs = 5000;
    p.idleMs = 20000;
    p.eagerMs = 1000;
    Heartbeat hb(p);
    hb.Reset(1000);
    CHECK(!hb.PingDue(1100, true));
    // ping BEFORE the event is written: its pong must not confirm the event
    hb.OnPingSent(6000);
    hb.OnEventWritten(7);
    CHECK_EQ(hb.OnPong(), (uint64_t)0);
    // a later ping covers it
    CHECK(!hb.PingDue(6500, true));   // eager ping waits for the 1 s gap
    CHECK(hb.PingDue(7100, true));    // eager: written 7 > covered 0, 1.1 s since the last ping
    CHECK(!hb.PingDue(7100, false));  // not while events are still queued
    hb.OnPingSent(6300);
    hb.OnEventWritten(9);  // written after the second ping
    CHECK_EQ(hb.OnPong(), (uint64_t)7);
    CHECK_EQ(hb.Confirmed(), (uint64_t)7);
    hb.OnPingSent(6600);
    CHECK_EQ(hb.OnPong(), (uint64_t)9);
    CHECK_EQ(hb.OnPong(), (uint64_t)0);  // unsolicited
    // a lost pong only under-confirms
    hb.OnEventWritten(12);
    hb.OnPingSent(7000);  // pong lost
    hb.OnEventWritten(15);
    hb.OnPingSent(7300);
    CHECK_EQ(hb.OnPong(), (uint64_t)12);  // the late pong pops the older entry: confirms 12, not 15
    CHECK_EQ(hb.OnPong(), (uint64_t)15);
    // idle detection
    hb.OnInbound(8000);
    CHECK(!hb.Dead(27000));
    CHECK(hb.Dead(28001));
    // outstanding cap
    Heartbeat h2(p);
    h2.Reset(0);
    for (int i = 0; i < 8; i++) h2.OnPingSent(i * 6000);
    CHECK(!h2.PingDue(100000, true));
}

void QueueTests() {
    t::Group("frame-queues");
    FrameQueues q;
    auto f = [](FrameKind k, const std::string& s, uint64_t id = 0) {
        return OutFrame{k, std::make_shared<const std::string>(s), 1, id};
    };
    CHECK(q.Push(f(FrameKind::Event, "e1", 1)) == QueueStatus::Accepted);
    CHECK(q.Push(f(FrameKind::Response, "r1")) == QueueStatus::Accepted);
    CHECK(q.Push(f(FrameKind::Control, "c1")) == QueueStatus::Accepted);
    CHECK(q.Push(f(FrameKind::CriticalResponse, "x1")) == QueueStatus::Accepted);
    CHECK(q.Push(f(FrameKind::Event, "e2", 2)) == QueueStatus::Accepted);
    std::string order;
    OutFrame o;
    while (q.Pop(o)) order += *o.text + " ";
    CHECK_EQ(order, std::string("c1 x1 r1 e1 e2 "));
    for (size_t i = 0; i < FrameQueues::LimitFor(FrameKind::Control).frames; i++) q.Push(f(FrameKind::Control, "c"));
    CHECK(q.Push(f(FrameKind::Control, "c")) == QueueStatus::Full);
    CHECK(q.Push(f(FrameKind::Response, "r")) == QueueStatus::Accepted);  // other kinds unaffected
    CHECK(q.Push(f(FrameKind::Response, std::string(kMaxOutboundFrame + 1, 'x'))) == QueueStatus::TooLarge);
    q.Clear();
    CHECK_EQ(q.TotalFrames(), (size_t)0);
}

void ConfigTests() {
    t::Group("config");
    NativeConfig c = LoadNativeConfig("Z:\\srv", Env({}), "");
    CHECK(!c.enabled);
    CHECK(c.disabledReason.find("identityToken and registrationToken") != std::string::npos);
    c = LoadNativeConfig("Z:\\srv", Env({}),
                         R"({"token":"x","identityToken":"file-id","registrationToken":"file-reg","name":"My Server","caFile":"takaro\\ca.pem"})");
    CHECK(c.enabled);
    CHECK_EQ(c.identityToken, std::string("file-id"));
    CHECK_EQ(c.serverName, std::string("My Server"));
    CHECK_EQ(c.url, std::string("wss://connect.takaro.io/"));
    CHECK_EQ(c.caFile, std::string("Z:\\srv\\takaro\\ca.pem"));  // relative CA path resolves against the exe dir
    c = LoadNativeConfig("Z:\\srv",
                         Env({{"TAKARO_IDENTITY_TOKEN", "env-id"}, {"TAKARO_REGISTRATION_TOKEN", " env-reg "},
                              {"TAKARO_WS_URL", "wss://fake:8443/"}, {"TAKARO_CA_FILE", "Z:\\certs\\ca.pem"}}),
                         R"({"identityToken":"file-id","registrationToken":"file-reg"})");
    CHECK_EQ(c.identityToken, std::string("env-id"));  // env wins
    CHECK_EQ(c.registrationToken, std::string("env-reg"));
    CHECK_EQ(c.url, std::string("wss://fake:8443/"));
    CHECK_EQ(c.caFile, std::string("Z:\\certs\\ca.pem"));
    CHECK_EQ(c.serverName, std::string("Takaro Dev Enshrouded"));
    std::string summary = ConfigSummaryJson(c);
    CHECK(summary.find("env-id") == std::string::npos && summary.find("env-reg") == std::string::npos);  // no secrets
    c = LoadNativeConfig("Z:\\srv", Env({{"TAKARO_IDENTITY_TOKEN", "a"}, {"TAKARO_REGISTRATION_TOKEN", "b"}, {"TAKARO_NATIVE_DISABLE", "1"}}), "");
    CHECK(!c.enabled && c.disabledReason.find("TAKARO_NATIVE_DISABLE") != std::string::npos);
    c = LoadNativeConfig("Z:\\srv", Env({{"TAKARO_IDENTITY_TOKEN", "a"}, {"TAKARO_REGISTRATION_TOKEN", "b"}, {"TAKARO_WS_URL", "ws://plain/"}}), "");
    CHECK(!c.enabled && c.disabledReason.find("wss://") != std::string::npos);
    c = LoadNativeConfig("Z:\\srv",
                         Env({{"TAKARO_IDENTITY_TOKEN", "a"}, {"TAKARO_REGISTRATION_TOKEN", "b"}, {"ENSHROUDED_LOG_EVENTS", "bogus"},
                              {"TAKARO_RECONNECT_BASE_MS", "5"}, {"ENSHROUDED_LOG_TAIL", "never"}, {"TAKARO_LEGACY_HTTP", "1"}}),
                         "not json");
    CHECK(c.enabled);
    CHECK(c.logEvents == LogEventsMode::Filtered && c.logTail == LogTailMode::Never && c.legacyHttp);
    CHECK_EQ(c.reconnectBaseMs, 2000u);
    CHECK_EQ(c.warnings.size(), (size_t)3);
    CHECK_EQ(c.logFile, std::string("Z:\\srv\\logs\\enshrouded_server.log"));
}

void PersistenceTests() {
    t::Group("persistence");
    std::string dir = t::TempDir("persist");
    auto env = Env({});
    StatePaths paths = ResolveStatePaths(dir, env);
    CHECK_EQ(paths.dir, dir + "/takaro/connector-state");
    {
        Store s(paths);
        LoadReport rep = s.Load();
        CHECK(rep.outboxCreated);
        CHECK(s.OutboxUsable());
        s.Outbox().scan = {"boot1", 5};
        for (int i = 0; i < 3; i++) {
            PendingEvent e;
            e.id = s.Outbox().nextId++;
            e.source = {"boot1", (uint64_t)i + 1};
            e.type = i == 1 ? "chat-message" : "log";
            e.frame = "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"" + e.type + "\",\"data\":{\"msg\":\"m" + std::to_string(i) + "\"}}}";
            s.Admit(e);
        }
        CHECK(s.SaveOutbox());
        CHECK_EQ(s.ConfirmThrough(1), (size_t)1);
        CHECK(s.SaveOutbox());
    }
    {
        Store s(paths);
        s.Load();
        CHECK_EQ(s.Outbox().pending.size(), (size_t)2);
        CHECK_EQ(s.Outbox().scan.seq, (uint64_t)5);
        CHECK_EQ(s.Outbox().confirmed.seq, (uint64_t)1);
        CHECK_EQ(s.Outbox().nextId, (uint64_t)4);
        CHECK(!t::ReadFile(paths.outbox).empty());
        CHECK(t::ReadFile(paths.outbox + ".tmp").empty());  // replaced atomically, no tmp left behind
    }
    // overflow drops log events first
    {
        Store s(ResolveStatePaths(t::TempDir("persist-overflow"), env));
        s.Load();
        for (size_t i = 0; i < Store::kMaxPendingEvents + 3; i++) {
            PendingEvent e;
            e.id = s.Outbox().nextId++;
            e.type = i < 3 ? "player-connected" : "log";
            e.frame = "{}";
            s.Admit(e);
        }
        CHECK_EQ(s.Outbox().pending.size(), Store::kMaxPendingEvents);
        CHECK_EQ(s.Outbox().losses, (uint64_t)3);
        CHECK_EQ(s.Outbox().pending.front().type, std::string("player-connected"));
    }
    // corrupt files are errors: fenced, never overwritten, never read as empty
    {
        std::string d2 = t::TempDir("persist-corrupt");
        StatePaths p2 = ResolveStatePaths(d2, env);
        std::string err;
        EnsureDirectory(p2.dir, err);
        t::WriteFile(p2.outbox, "{\"version\":1,\"scan\":");
        t::WriteFile(p2.timedBans, "[{\"gameId\":\"1\"}]");
        t::WriteFile(p2.online, "{oops");
        Store s(p2);
        s.Load();
        CHECK(!s.OutboxUsable());
        CHECK(s.Fenced("timedBans") && s.Fenced("online"));
        CHECK(!s.BanStoreError().empty());
        CHECK(!s.SaveOutbox());
        CHECK(!s.SaveTimedBans());
        CHECK_EQ(t::ReadFile(p2.outbox), std::string("{\"version\":1,\"scan\":"));
        CHECK_EQ(t::ReadFile(p2.timedBans), std::string("[{\"gameId\":\"1\"}]"));
        CHECK_EQ(s.Errors().size(), (size_t)3);
    }
    // legacy sidecar import: cursor once, online file in the sidecar's own format
    {
        std::string d3 = t::TempDir("persist-legacy");
        std::string err;
        EnsureDirectory(d3 + "/sidecar", err);
        t::WriteFile(d3 + "/sidecar/event-cursor.json", "{\"seq\":42,\"bootId\":\"oldboot\"}");
        t::WriteFile(d3 + "/sidecar/online-players.json",
                     "[{\"gameId\":\"76561198000005875\",\"name\":\"Limon\",\"steamId\":\"76561198000005875\",\"platformId\":\"steam:76561198000005875\"},{\"bad\":1}]");
        StatePaths p3 = ResolveStatePaths(d3, Env({{"TAKARO_CURSOR_FILE", "sidecar/event-cursor.json"},
                                                  {"TAKARO_ONLINE_FILE", "sidecar/online-players.json"}}));
        Store s(p3);
        LoadReport rep = s.Load();
        CHECK(rep.legacyCursorImported);
        CHECK_EQ(s.Outbox().scan.bootId, std::string("oldboot"));
        CHECK_EQ(s.Outbox().scan.seq, (uint64_t)42);
        CHECK_EQ(s.Online().size(), (size_t)1);
        Store again(p3);
        CHECK(!again.Load().legacyCursorImported);  // only once: the outbox exists now
    }
    // known players are bounded and refreshed
    {
        Store s(ResolveStatePaths(t::TempDir("persist-known"), env));
        s.Load();
        CHECK(s.Remember(t::J(R"({"gameId":"1","name":"A"})"), 10));
        CHECK(!s.Remember(t::J(R"({"gameId":"1","name":"A"})"), 15));  // only lastSeen moved: nothing to write
        CHECK(s.Remember(t::J(R"({"gameId":"1","name":"A2"})"), 20));
        CHECK_EQ(s.Known().size(), (size_t)1);
        CHECK_EQ(s.Known()[0].get("name")->str, std::string("A2"));
    }
}

void TailerTests() {
    t::Group("log-tailer");
    std::string dir = t::TempDir("tail");
    std::string file = dir + "/enshrouded_server.log";
    const char* SID = "76561198000005875";
    std::string join = std::string("[I 00:03:52,650] [online] Added peer 0(1) (steamid:") + SID + ")\n" +
                       "[I 00:04:24,062] [server] Machine '1': Player '0(0)' logged in\n"
                       "[I 00:04:24,063] [server] Player 'Limon' logged in with Permissions:\n"
                       "[I 00:04:24,063] \t - CanKickBan\n";
    std::string leave = "[I 00:07:46,566] [server] Remove Player 'Limon'\n[I 00:07:46,704] [online] Removed peer 0(1)\n";
    t::WriteFile(file, join);  // old session must not replay
    LogTailer tail(file);
    tail.Start();
    CHECK(tail.Poll().empty());
    std::vector<std::string> types;
    auto append = [&](const std::string& s) {
        FILE* f = fopen(file.c_str(), "ab");
        fwrite(s.data(), 1, s.size(), f);
        fclose(f);
    };
    append(join.substr(0, 60));
    for (auto& e : tail.Poll()) types.push_back(e.type);
    append(join.substr(60));
    for (auto& e : tail.Poll()) types.push_back(e.type);
    CHECK_EQ(types.size(), (size_t)1);
    // rotation: the server replaces the file on restart
    remove(file.c_str());
    t::WriteFile(file, "[I 00:00:00,001] new boot\n");
    for (auto& e : tail.Poll()) types.push_back(e.type);
    append(join + leave);
    for (auto& e : tail.Poll()) types.push_back(e.type);
    std::string all;
    for (auto& s : types) all += s + " ";
    CHECK_EQ(all, std::string("player-connected player-connected player-disconnected "));
}

// Takaro matches players on the raw steamId and never parses platformId: only a real SteamID64
// may go out as steamId, on every path that builds a player (events, getPlayers, listBans).
void IdentityTests() {
    t::Group("player-identity");
    const std::string sid = "76561198000005875";
    JsonValue p = t::J(R"({"gameId":"76561198000005875","name":"Limon","steamId":"76561198000005875"})");
    JsonValue m = MapPlayer(&p);
    CHECK_EQ(Str(m.get("gameId")).value_or("?"), sid);
    CHECK_EQ(Str(m.get("steamId")).value_or("?"), sid);
    CHECK_EQ(Str(m.get("platformId")).value_or("?"), "steam:" + sid);
    CHECK(!m.get("epicOnlineServicesId"));
    CHECK(!m.get("xboxLiveId"));
    // steamId derived from a SteamID64 gameId or a steam: platformId when the plugin omits it
    JsonValue g = t::J(R"({"gameId":"76561198000005875","name":"Limon"})");
    CHECK_EQ(Str(MapPlayer(&g).get("steamId")).value_or("?"), sid);
    JsonValue pl = t::J(R"({"platformId":"steam:76561198000005875","name":"Limon"})");
    JsonValue mpl = MapPlayer(&pl);
    CHECK_EQ(Str(mpl.get("steamId")).value_or("?"), sid);
    CHECK_EQ(Str(mpl.get("gameId")).value_or("?"), sid);
    // the plugin's ban list carries the account hash in steamId when the SteamID is not cached
    JsonValue hash = t::J(R"({"gameId":"1234567890123456789","name":"Ghost","steamId":"1234567890123456789"})");
    JsonValue mh = MapPlayer(&hash);
    CHECK_EQ(Str(mh.get("gameId")).value_or("?"), std::string("1234567890123456789"));
    CHECK(!mh.get("steamId"));
    CHECK(!mh.get("platformId"));
    JsonValue shortSteam = t::J(R"({"gameId":"x","platformId":"steam:67","steamId":"0"})");
    JsonValue ms = MapPlayer(&shortSteam);
    CHECK(!ms.get("steamId"));
    CHECK(!ms.get("platformId"));
    JsonValue ban = t::J(R"({"player":{"gameId":"76561198000005875","steamId":"76561198000005875","name":"Limon"},"reason":"x"})");
    JsonValue mb = MapBan(&ban);
    CHECK_EQ(Str(mb.get("player")->get("steamId")).value_or("?"), sid);
}

}  // namespace

void RunUnitTests() {
    IdentityTests();
    JsonUtilTests();
    IsoTests();
    HeartbeatTests();
    QueueTests();
    ConfigTests();
    PersistenceTests();
    TailerTests();
}
