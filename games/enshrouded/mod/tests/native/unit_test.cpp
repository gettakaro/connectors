// Unit tests for the native connector building blocks.
#include "native/config.h"
#include "native/fileio.h"
#include "native/game_profile.h"
#include "native/json_util.h"
#include "native/logtail.h"
#include "native/mapping.h"
#include "native/persistence.h"
#include "native/transport.h"
#include "testlib.h"

#include <cstdlib>
#include <map>
#include <regex>

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

// POST /give `quality` and console `give ... [level]` share ParseItemLevel (common.cpp).
void ItemLevelTests() {
    t::Group("give-item-level");
    uint8_t lvl = 99;
    auto body = [](const char* json) { return t::J(json); };
    // absent, null and empty mean the item's default level (0)
    JsonValue b = body(R"({"gameId":"1","code":"Torch","amount":1})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 0);
    b = body(R"({"quality":null})");
    lvl = 99;
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 0);
    b = body(R"({"quality":""})");
    lvl = 99;
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 0);
    b = body(R"({"quality":"  "})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 0);
    // a string (what the native adapter forwards) or a number, 1-255
    b = body(R"({"quality":"5"})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 5);
    b = body(R"({"quality":" 7 "})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 7);
    b = body(R"({"quality":4})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 4);
    b = body(R"({"quality":1})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 1);
    b = body(R"({"quality":"255"})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 255);
    b = body(R"({"quality":3.0})");
    CHECK(ParseItemLevel(b.get("quality"), lvl) && lvl == 3);
    // invalid: the same rule the console applies (integer 1-255)
    for (const char* bad : {R"({"quality":"legendary"})", R"({"quality":"0"})", R"({"quality":0})", R"({"quality":256})",
                            R"({"quality":"256"})", R"({"quality":-1})", R"({"quality":"2.5"})", R"({"quality":2.5})",
                            R"({"quality":"5x"})", R"({"quality":"nan"})", R"({"quality":true})", R"({"quality":[5]})",
                            R"({"quality":{"level":5}})"}) {
        b = body(bad);
        CHECK_MSG(!ParseItemLevel(b.get("quality"), lvl), bad);
    }
    // console tokens
    CHECK(ParseItemLevel(std::string("12"), lvl) && lvl == 12);
    CHECK(!ParseItemLevel(std::string("0"), lvl));
    CHECK(!ParseItemLevel(std::string("300"), lvl));
    CHECK(!ParseItemLevel(std::string("abc"), lvl));
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
    CHECK(c.enabled);  // the connection settings come from ConfigWatcher; only the kill switch disables
    c = LoadNativeConfig("Z:\\srv", Env({}), R"({"token":"x","caFile":"takaro\\ca.pem"})");
    CHECK_EQ(c.caFile, std::string("Z:\\srv\\takaro\\ca.pem"));  // relative CA path resolves against the exe dir
    c = LoadNativeConfig("Z:\\srv", Env({{"TAKARO_CA_FILE", "Z:\\certs\\ca.pem"}}), "\xEF\xBB\xBF{\"caFile\":\"x.pem\"}");
    CHECK_EQ(c.caFile, std::string("Z:\\certs\\ca.pem"));  // env wins; a BOM is no parse error
    CHECK(c.warnings.empty());
    LiveResolution live;
    live.s = {"wss://fake:8443/", "env-reg", "env-id", "My Server"};
    live.url = live.registration = live.identity = live.name = Source::Env;
    ApplyLive(c, live);
    CHECK_EQ(c.identityToken, std::string("env-id"));
    CHECK_EQ(c.serverName, std::string("My Server"));
    std::string summary = ConfigSummaryJson(c);
    CHECK(summary.find("env-id") == std::string::npos && summary.find("env-reg") == std::string::npos);  // no secrets
    CHECK(summary.find("\"registrationToken\":\"environment\"") != std::string::npos);
    c = LoadNativeConfig("Z:\\srv", Env({{"TAKARO_NATIVE_DISABLE", "1"}}), "");
    CHECK(!c.enabled && c.disabledReason.find("TAKARO_NATIVE_DISABLE") != std::string::npos);
    c = LoadNativeConfig("Z:\\srv",
                         Env({{"ENSHROUDED_LOG_EVENTS", "bogus"}, {"TAKARO_RECONNECT_BASE_MS", "5"},
                              {"ENSHROUDED_LOG_TAIL", "never"}, {"TAKARO_LEGACY_HTTP", "1"}}),
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

void ProtocolV1Tests() {
    t::Group("protocol-v1");
    // error codes: arguments, missing things, the game, the connector
    auto code = [](auto thrower) {
        try {
            thrower();
        } catch (const std::exception& e) {
            return ProtocolErrorCode(e);
        }
        return std::string("none");
    };
    CHECK_EQ(code([] { Fail("sendMessage requires 'message'"); }), std::string("invalid_args"));
    CHECK_EQ(code([] { FailCode("not_found", "x"); }), std::string("not_found"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::PluginHttp, "x", 404); }), std::string("not_found"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::PluginHttp, "x", 400); }), std::string("invalid_args"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::PluginHttp, "x", 500); }), std::string("game_error"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::PluginUnimplemented, "x"); }), std::string("unsupported"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::PluginUnreachable, "x"); }), std::string("game_error"));
    CHECK_EQ(code([] { throw NativeError(ErrorKind::Plain, "x"); }), std::string("game_error"));
    CHECK_EQ(code([] { throw std::runtime_error("x"); }), std::string("internal"));

    CHECK(t::JsonTextEq(CreateErrorResponseV1("r1", "not_found", "Player 1 is not online"),
                        R"({"type":"response","requestId":"r1","error":{"code":"not_found","message":"Player 1 is not online"}})"));
    CHECK(t::JsonTextEq(CreateResponseV1("r1", JNull()), R"({"type":"response","requestId":"r1","payload":null})"));
    CHECK(t::JsonTextEq(CreateResponse("r1", JNull()), R"({"type":"response","requestId":"r1","payload":{}})"));
    std::string v0 = CreateGameEvent("chat-message", t::J(R"({"msg":"hi"})"));
    CHECK(t::JsonTextEq(GameEventFrameV1(v0, "s-1", 7),
                        R"({"type":"gameEvent","payload":{"type":"chat-message","data":{"msg":"hi"},"streamId":"s-1","seq":7}})"));
    CHECK_EQ(GameEventFrameV1("not json", "s", 1), std::string(""));
    CHECK_EQ(GameEventFrameV1(R"({"type":"gameEvent","payload":{"type":"x"}})", "s", 1), std::string(""));
    const std::string who = R"("game":"enshrouded","connector":{"name":"takaro-enshrouded","version":")" +
                            std::string(kConnectorVersion) + R"("})";
    CHECK(t::JsonTextEq(CreateIdentify("i", "r", "n"),
                        R"({"type":"identify","payload":{"identityToken":"i","registrationToken":"r","name":"n",)" + who + "}}"));
    CHECK(t::JsonTextEq(CreateIdentify("i", "r", "n", "[]"),
                        R"({"type":"identify","payload":{"identityToken":"i","registrationToken":"r","name":"n",)" + who +
                            R"(,"protocolVersions":[0,1],"capabilities":[]}})"));

    CHECK(ValidStreamId("4f0e9a1c-6c2b-4d3a-9f8e-7b6a5c4d3e2f") && ValidStreamId("a_B-1"));
    CHECK(!ValidStreamId("") && !ValidStreamId("a b") && !ValidStreamId(std::string(129, 'a')));
    OutboxState o, back;
    o.streamId = "stream-1";
    std::string err;
    CHECK(ParseOutbox(OutboxJson(o), back, err) && back.streamId == "stream-1");
    o.streamId = "";
    CHECK(ParseOutbox(OutboxJson(o), back, err) && back.streamId.empty());

    // protocol=0 turns the protocol 1 identify off; anything else keeps it on (with a warning)
    CHECK(LoadNativeConfig("Z:\\srv", Env({}), "").protocolV1);
    CHECK(!LoadNativeConfig("Z:\\srv", Env({{"TAKARO_PROTOCOL", "0"}}), "").protocolV1);
    CHECK(!LoadNativeConfig("Z:\\srv", Env({}), R"({"protocol":"0"})").protocolV1);
    CHECK(!LoadNativeConfig("Z:\\srv", Env({}), R"({"protocol":0})").protocolV1);  // a number, as in the created file
    NativeConfig one = LoadNativeConfig("Z:\\srv", Env({}), R"({"protocol":1})");
    CHECK(one.protocolV1 && one.warnings.empty());
    NativeConfig c = LoadNativeConfig("Z:\\srv", Env({{"TAKARO_PROTOCOL", "2"}}), "");
    CHECK(c.protocolV1 && !c.warnings.empty());
}

std::string Base64Decode(const std::string& in) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        size_t v = alphabet.find(c);
        if (v == std::string::npos) return "<invalid base64>";
        acc = acc << 6 | (uint32_t)v;
        if ((bits += 6) >= 8) out += (char)((acc >> (bits -= 8)) & 0xff);
    }
    return out;
}

uint32_t Be32(const std::string& s, size_t at) {
    return (uint32_t)(unsigned char)s[at] << 24 | (uint32_t)(unsigned char)s[at + 1] << 16 |
           (uint32_t)(unsigned char)s[at + 2] << 8 | (unsigned char)s[at + 3];
}

void GameProfileTests() {
    t::Group("game-profile");
    // release-please rewrites TAKARO_PLUGIN_VERSION and version.txt together; run.sh hands version.txt in.
    const char* versionTxt = getenv("ENSHROUDED_VERSION_TXT");
    CHECK_MSG(versionTxt && *versionTxt, "ENSHROUDED_VERSION_TXT not set (run tests/run.sh)");
    if (versionTxt) CHECK_EQ(std::string(kConnectorVersion), Trim(versionTxt));
    CHECK(std::regex_match(std::string(kConnectorVersion),
                           std::regex("^(0|[1-9]\\d*)\\.(0|[1-9]\\d*)\\.(0|[1-9]\\d*)(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")));
    for (const char* slug : {kGameSlug, kConnectorName})
        CHECK_MSG(std::regex_match(std::string(slug), std::regex("^[a-z0-9][a-z0-9-]*$")), slug);

    CHECK_EQ(Sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(Sha256Hex(std::string(1000, 'a')), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));

    // the shipped icon: a 256 x 256 PNG of at most 128 KiB; revision = first 12 hex of its sha256
    const std::string& png = GameIconPng();
    CHECK(png.size() > 33 && png.size() <= 131072);
    CHECK(png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && png.compare(12, 4, "IHDR") == 0);
    CHECK_EQ(Be32(png, 16), 256u);
    CHECK_EQ(Be32(png, 20), 256u);
    CHECK_EQ(GameIconRevision(), Sha256Hex(png).substr(0, 12));

    JsonValue cap = t::J(GameProfileCapability(" 1269579 "));
    CHECK_EQ(Str(cap.get("name")).value_or(""), std::string("game.profile"));
    const JsonValue& p = AsRecord(cap.get("parameters"));
    CHECK_EQ(Str(p.get("name")).value_or(""), std::string("Enshrouded"));
    CHECK_EQ(Str(p.get("version")).value_or(""), std::string("1269579"));
    const JsonValue& icon = AsRecord(p.get("icon"));
    CHECK_EQ(Str(icon.get("revision")).value_or(""), GameIconRevision());
    CHECK_EQ(Num(icon.get("size")).value_or(-1), (double)png.size());
    // unknown, too long or non-printable builds are left out, never sent as-is
    for (const std::string& bad : {std::string(""), std::string("   "), std::string(33, '1'), std::string("12\x01" "3")})
        CHECK_MSG(!AsRecord(t::J(GameProfileCapability(bad)).get("parameters")).get("version"), bad);
    CHECK(AsRecord(t::J(GameProfileCapability(std::string(32, '1'))).get("parameters")).get("version"));

    JsonValue icon64 = GameIconPayload();
    CHECK_EQ(Str(icon64.get("revision")).value_or(""), GameIconRevision());
    std::string bytes = Base64Decode(Str(icon64.get("data")).value_or(""));
    CHECK_EQ(bytes.size(), png.size());
    CHECK(bytes == png);

    // identify names the game and the connector on both protocol versions
    for (const std::string& caps : {std::string(""), std::string("[]")}) {
        JsonValue id = t::J(CreateIdentify("i", "r", "Server", caps));
        const JsonValue& pl = AsRecord(id.get("payload"));
        CHECK_EQ(Str(pl.get("game")).value_or(""), std::string("enshrouded"));
        CHECK_EQ(Str(AsRecord(pl.get("connector")).get("name")).value_or(""), std::string("takaro-enshrouded"));
        CHECK_EQ(Str(AsRecord(pl.get("connector")).get("version")).value_or(""), std::string(kConnectorVersion));
    }
}

}  // namespace

void RunUnitTests() {
    GameProfileTests();
    ProtocolV1Tests();
    IdentityTests();
    JsonUtilTests();
    ItemLevelTests();
    IsoTests();
    HeartbeatTests();
    QueueTests();
    ConfigTests();
    PersistenceTests();
    TailerTests();
}
