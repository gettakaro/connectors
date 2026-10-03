// Host tests for lane L2c (events): the Takaro event payloads and their whitelist, death cause and
// weapon naming, the log tail (line splitting, redaction, rate limit, rotation) and the hook
// dispatch table (bloom + open addressing, Before/After order, veto).
#include "common.h"
#include "conan/events_payload.h"
#include "conan/hook_dispatch.h"
#include "conan/logtail.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace conan;
using namespace conan::events;

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

static std::string D(const JsonValue& v) { return takaro::JsonDump(v); }
static JsonValue J(const std::string& text) {
    JsonValue v;
    if (!JsonParse(text, v)) printf("bad test json: %s\n", text.c_str());
    return v;
}

static PlayerId Me() {
    PlayerId p;
    p.steam64 = "76561198000735875";
    p.name = "Limon#67642";
    p.ip = "192.168.129.15";
    return p;
}

static void TestPayloads() {
    PlayerId me = Me();
    EQ(D(ConnectedPayload(me)),
       "{\"player\":{\"gameId\":\"76561198000735875\",\"name\":\"Limon#67642\",\"steamId\":\"76561198000735875\","
       "\"platformId\":\"steam:76561198000735875\",\"ip\":\"192.168.129.15\"}}");
    PlayerId bad;
    bad.steam64 = "154";
    CHECK(ConnectedPayload(bad).type == JsonValue::Null, "a non-Steam64 id must not produce an event");
    PlayerId noName = me;
    noName.name = "  ";
    noName.ip = "";
    EQ(D(PlayerJson(noName)), "{\"gameId\":\"76561198000735875\",\"name\":\"76561198000735875\",\"steamId\":"
                              "\"76561198000735875\",\"platformId\":\"steam:76561198000735875\"}");

    EQ(MapChannel("Global"), "global");
    EQ(MapChannel("Local"), "global");
    EQ(MapChannel("Clan"), "team");
    EQ(MapChannel("whatever"), "global");
    JsonValue chat = ChatPayload(me, "Local", "+s4 back to global");
    EQ(takaro::JsString(chat.get("channel")), "global");
    EQ(takaro::JsString(chat.get("msg")), "+s4 back to global");
    CHECK(ChatPayload(me, "Global", "   ").type == JsonValue::Null, "empty chat is not an event");

    Position pos;
    pos.has = true;
    pos.x = 101746.7249;
    pos.y = 319894.28;
    pos.z = -21582.481;
    EQ(D(DeathPayload(me, nullptr, pos, "Limon#67642 died (fall damage)")),
       "{\"player\":{\"gameId\":\"76561198000735875\",\"name\":\"Limon#67642\",\"steamId\":\"76561198000735875\","
       "\"platformId\":\"steam:76561198000735875\",\"ip\":\"192.168.129.15\"},\"position\":{\"x\":101746.72,"
       "\"y\":319894.28,\"z\":-21582.48},\"msg\":\"Limon#67642 died (fall damage)\"}");
    PlayerId other = me;
    other.steam64 = "76561198000000001";
    other.name = "Other";
    JsonValue pvp = DeathPayload(me, &other, Position(), "x");
    CHECK(pvp.get("attacker") != nullptr && pvp.get("position") == nullptr, "%s", D(pvp).c_str());
    JsonValue self = DeathPayload(me, &me, Position(), "");
    CHECK(self.get("attacker") == nullptr && self.get("msg") == nullptr, "self is never the attacker: %s", D(self).c_str());

    EQ(D(KilledPayload(me, "Rabbit", "Unarmed")),
       "{\"player\":{\"gameId\":\"76561198000735875\",\"name\":\"Limon#67642\",\"steamId\":\"76561198000735875\","
       "\"platformId\":\"steam:76561198000735875\",\"ip\":\"192.168.129.15\"},\"entity\":\"Rabbit\",\"weapon\":\"Unarmed\"}");
    EQ(takaro::JsString(KilledPayload(me, "Imp", "").get("weapon")), "unknown");
    CHECK(KilledPayload(me, " ", "x").type == JsonValue::Null, "entity is required");
    EQ(D(LogPayload("LogNet: Join succeeded: Limon#67642")), "{\"msg\":\"LogNet: Join succeeded: Limon#67642\"}");
}

static void TestSanitize() {
    JsonValue in = J("{\"player\":{\"gameId\":\"1\",\"name\":\"n\",\"characterName\":\"werwerwer\",\"rconId\":3},"
                     "\"entity\":\"Rabbit\",\"entityCode\":\"Wildlife_Rabbit\",\"weapon\":\"Unarmed\",\"msg\":\"m\"}");
    EQ(D(Sanitize("entity-killed", in)),
       "{\"player\":{\"gameId\":\"1\",\"name\":\"n\"},\"entity\":\"Rabbit\",\"weapon\":\"Unarmed\",\"msg\":\"m\"}");
    JsonValue death = J("{\"player\":{\"gameId\":\"1\",\"name\":\"n\"},\"position\":{\"x\":1,\"y\":2,\"z\":3,\"w\":4},"
                        "\"cause\":\"fall\",\"killer\":{\"code\":\"x\"}}");
    EQ(D(Sanitize("player-death", death)), "{\"player\":{\"gameId\":\"1\",\"name\":\"n\"},\"position\":{\"x\":1,\"y\":2,\"z\":3}}");
    EQ(D(Sanitize("log", J("{\"msg\":\"a\",\"level\":\"info\"}"))), "{\"msg\":\"a\"}");
    EQ(D(Sanitize("chat-message", J("{\"player\":{\"gameId\":\"1\",\"name\":\"n\"},\"channel\":\"global\",\"msg\":\"hi\",\"teamOnly\":true}"))),
       "{\"player\":{\"gameId\":\"1\",\"name\":\"n\"},\"channel\":\"global\",\"msg\":\"hi\"}");
    CHECK(Sanitize("player-teleported", J("{}")).type == JsonValue::Null, "unknown type");
    CHECK(Sanitize("log", J("[1]")).type == JsonValue::Null, "non-object");
    // Every payload builder's output passes the whitelist unchanged.
    PlayerId me = Me();
    Position pos;
    pos.has = true;
    for (auto& tc : std::vector<std::pair<std::string, JsonValue>>{
             {"player-connected", ConnectedPayload(me)},
             {"player-disconnected", ConnectedPayload(me)},
             {"chat-message", ChatPayload(me, "Global", "hi")},
             {"player-death", DeathPayload(me, nullptr, pos, "m")},
             {"entity-killed", KilledPayload(me, "Imp", "Iron Sword")},
             {"log", LogPayload("line")}})
        EQ(D(Sanitize(tc.first, tc.second)), D(tc.second));
}

static void TestNaming() {
    EQ(CauseFromDamageType("Default__DmgTypeHealth_IgnoreArmor_BP_Fall_C"), "fall damage");
    EQ(CauseFromDamageType("Default__DmgTypeHealth_BP_C"), "");
    EQ(CauseFromDamageType("DmgType_Drowning_C"), "drowning");
    EQ(DeathMessage("Limon#67642", "", "fall damage"), "Limon#67642 died (fall damage)");
    EQ(DeathMessage("Limon#67642", "Imp", ""), "Limon#67642 was killed by Imp");
    EQ(DeathMessage("", "", ""), "A player died");
    EQ(WeaponName("XX_Unarmed Right", 51204), "Unarmed");
    EQ(WeaponName("", 51205), "Unarmed");
    EQ(WeaponName("Abysmal Blade", 51706), "Abysmal Blade");
    EQ(WeaponName("XX_Elk_bc_3", 1), "");
    EQ(WeaponName("xxx_Stygian Raider Mask_Surge", 1), "");
    CHECK(IsInternalName("dev_thing") && IsInternalName("Deprecated sword") && !IsInternalName("Xenophobe"), "filter");
    CHECK(!IsInternalName("Rabbit") && IsInternalName(""), "filter 2");
}

static void TestLogHelpers() {
    LineSplitter s(20);
    auto a = s.Feed("\xEF\xBB\xBFline one\r\nline t");
    CHECK(a.size() == 1 && a[0] == "line one", "got %zu", a.size());
    auto b = s.Feed("wo\n\n\nthis line is much longer than twenty bytes\n");
    CHECK(b.size() == 2 && b[0] == "line two" && b[1] == "this line is much lo...", "got %zu '%s'", b.size(),
          b.size() > 1 ? b[1].c_str() : "");
    auto c = s.Feed("bad \xff byte\tok\n");
    CHECK(c.size() == 1 && c[0] == "bad ? byte\tok", "got '%s'", c.empty() ? "" : c[0].c_str());
    EQ(CleanUtf8("caf\xc3\xa9"), "caf\xc3\xa9");

    std::vector<std::string> secrets = {"0123456789abcdef-identity", "short"};
    EQ(RedactLine("LogNet: Join request: /Game/Maps/ConanSandbox?Name=Limon?Password=example-pass-1?SplitscreenCount=1", secrets),
       "LogNet: Join request: /Game/Maps/ConanSandbox?Name=Limon?Password=[redacted]?SplitscreenCount=1");
    EQ(RedactLine("cmd -RconPassword=example-pass-2 -Port=7777", secrets), "cmd -RconPassword=[redacted] -Port=7777");
    EQ(RedactLine("identityToken: \"abc\" token is 0123456789abcdef-identity", secrets),
       "identityToken: \"[redacted]\" token is [redacted]");
    EQ(RedactLine("ChatWindow: Character werwerwer said: the token count is high", secrets),
       "ChatWindow: Character werwerwer said: the token count is high");
    EQ(RedactLine("short stays", secrets), "short stays");

    RateLimiter r(10, 5);
    int ok = 0;
    for (int i = 0; i < 20; i++) ok += r.Allow(1000) ? 1 : 0;
    CHECK(ok == 5 && r.Dropped() == 15, "burst: ok=%d dropped=%llu", ok, (unsigned long long)r.Dropped());
    CHECK(r.Allow(1100) && !r.Allow(1100), "refill 1 token per 100 ms");
    CHECK(r.TakeDroppedSinceLast() == 16 && r.TakeDroppedSinceLast() == 0, "drop summary");
}

static void Append(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "ab");
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

static void TestLogTail() {
    char dir[] = "/tmp/l2c-logtail-XXXXXX";
    if (!mkdtemp(dir)) {
        CHECK(false, "mkdtemp");
        return;
    }
    const std::string path = std::string(dir) + "/ConanSandbox.log";
    Append(path, "previous run line 1\nprevious run line 2\n");
    std::vector<std::string> got;
    LogTailOptions o;
    o.path = path;
    o.secrets = {"supersecret-registration"};
    o.perSecond = 1000;
    o.burst = 3;
    o.emit = [&](const std::string& l) { got.push_back(l); };
    LogTail t(o);
    t.Step(1000);
    CHECK(got.empty(), "the previous run's backlog is skipped (%zu)", got.size());
    Append(path, "LogTemp: one\nLogTemp: two with supersecret-registration\nLogTemp: par");
    t.Step(1001);
    CHECK(got.size() == 2 && got[0] == "LogTemp: one" && got[1] == "LogTemp: two with [redacted]", "got %zu", got.size());
    Append(path, "tial\n");
    t.Step(1002);
    CHECK(got.size() == 3 && got[2] == "LogTemp: partial", "partial line joined");
    // Rotation: the engine renames the file at start and creates a new one.
    rename(path.c_str(), (std::string(dir) + "/ConanSandbox-backup-2026.10.03.log").c_str());
    Append(path, "new run 1\nnew run 2\n");
    t.Step(5000);
    CHECK(got.size() == 5 && got[3] == "new run 1" && got[4] == "new run 2", "rotation read from 0: %zu", got.size());
    // Truncation in place is a rotation too.
    FILE* f = fopen(path.c_str(), "wb");
    fputs("x\n", f);
    fclose(f);
    t.Step(9000);
    CHECK(got.size() == 6 && got[5] == "x", "truncate: %zu", got.size());
    // Rate limit: burst 3, then a summary line after 10 s.
    got.clear();
    std::string many;
    for (int i = 0; i < 10; i++) many += "spam " + std::to_string(i) + "\n";
    Append(path, many);
    t.Step(9001);
    CHECK(got.size() == 3, "rate limited to the burst: %zu", got.size());
    t.Step(30000);
    CHECK(!got.empty() && got.back().find("7 server log line(s) were not forwarded") != std::string::npos, "summary: %s",
          got.empty() ? "" : got.back().c_str());

    // A file that does not exist at load is read from its first line once it appears.
    const std::string p2 = std::string(dir) + "/Later.log";
    std::vector<std::string> got2;
    LogTailOptions o2 = o;
    o2.path = p2;
    o2.burst = 100;
    o2.emit = [&](const std::string& l) { got2.push_back(l); };
    LogTail t2(o2);
    t2.Step(1);
    Append(p2, "first\n");
    t2.Step(2);
    CHECK(got2.size() == 1 && got2[0] == "first", "new file from 0: %zu", got2.size());
    std::string cmd = std::string("rm -rf ") + dir;
    CHECK(system(cmd.c_str()) == 0, "cleanup");
}

// ---- hook dispatch table ----
static std::vector<std::string> g_order;
static bool Before1(const HookDispatch::Call& c, void* ctx) {
    g_order.push_back(std::string("before:") + (const char*)ctx + ":" + std::to_string(c.Get<int32_t>(0, -7)));
    return true;
}
static bool Veto(const HookDispatch::Call&, void* ctx) {
    g_order.push_back(std::string("veto:") + (const char*)ctx);
    return false;
}
static bool After1(const HookDispatch::Call& c, void* ctx) {
    g_order.push_back(std::string("after:") + (const char*)ctx + ":" + std::to_string(c.Ptr(1) != 0));
    return true;
}
static void Original(void*, void* func, void*) { g_order.push_back("original:" + std::to_string((uintptr_t)func)); }

static HookDispatch::HandlerRef H(HookDispatch::Handler fn, const char* ctx, HookDispatch::Phase ph, int32_t off0,
                                  int32_t off1 = -1) {
    HookDispatch::HandlerRef h{};
    h.fn = fn;
    h.ctx = (void*)ctx;
    h.phase = ph;
    h.sub = 0;
    for (auto& o : h.off) o = -1;
    h.off[0] = off0;
    h.off[1] = off1;
    return h;
}

static void TestDispatch() {
    using HookDispatch::Phase;
    CHECK(HookDispatch::Match((void*)0x1000) == nullptr, "no table: no match");
    std::vector<std::pair<uintptr_t, HookDispatch::HandlerRef>> e;
    // 300 functions at UObject-like addresses (16-aligned), forcing probes past collisions.
    for (uintptr_t i = 0; i < 300; i++) e.push_back({0x7f0000100000 + i * 0x60, H(Before1, "bulk", Phase::Before, 0)});
    e.push_back({0x7f0000900010, H(After1, "a", Phase::After, -1, 8)});
    e.push_back({0x7f0000900010, H(Before1, "b", Phase::Before, 0)});
    e.push_back({0x7f0000900020, H(Veto, "v", Phase::Before, -1)});
    e.push_back({0x7f0000900020, H(After1, "never", Phase::After, -1)});
    HookDispatch::PublishForTest(e);
    int hits = 0;
    for (uintptr_t i = 0; i < 300; i++) hits += HookDispatch::Match((void*)(0x7f0000100000 + i * 0x60)) != nullptr;
    CHECK(hits == 300, "every subscribed function matches: %d", hits);
    int false_hits = 0;
    for (uintptr_t i = 0; i < 100000; i++) false_hits += HookDispatch::Match((void*)(0x7f0000200008 + i * 0x30)) != nullptr;
    CHECK(false_hits == 0, "no unsubscribed function matches: %d", false_hits);

    alignas(16) uint8_t parms[16] = {0};
    int32_t v = 42;
    memcpy(parms, &v, 4);
    uintptr_t p = 0x1234;
    memcpy(parms + 8, &p, 8);
    g_order.clear();
    const HookDispatch::Slot* s = HookDispatch::Match((void*)0x7f0000900010);
    CHECK(s && s->count == 2, "two handlers on one function");
    if (s) HookDispatch::Invoke(s, nullptr, (void*)0x7f0000900010, parms, Original);
    CHECK(g_order.size() == 3 && g_order[0] == "before:b:42" && g_order[1].rfind("original:", 0) == 0 &&
              g_order[2] == "after:a:1",
          "order: %s | %s | %s", g_order.size() > 0 ? g_order[0].c_str() : "", g_order.size() > 1 ? g_order[1].c_str() : "",
          g_order.size() > 2 ? g_order[2].c_str() : "");
    g_order.clear();
    s = HookDispatch::Match((void*)0x7f0000900020);
    if (s) HookDispatch::Invoke(s, nullptr, (void*)0x7f0000900020, parms, Original);
    CHECK(g_order.size() == 1 && g_order[0] == "veto:v", "a veto skips the original and the After handlers (%zu)",
          g_order.size());
    HookDispatch::PublishForTest({});
    CHECK(HookDispatch::Match((void*)0x7f0000900010) == nullptr, "empty table");
}

int main() {
    TestPayloads();
    TestSanitize();
    TestNaming();
    TestLogHelpers();
    TestLogTail();
    TestDispatch();
    printf("events tests: %d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
