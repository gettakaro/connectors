// Host tests for lane L2b (mutations): giveItem, teleportPlayer, executeConsoleCommand, kickPlayer,
// banPlayer, unbanPlayer, listBans, shutdown.
//
//   Part A: the action logic (core/conan/actions.cpp, bans.cpp, shutdown.cpp) against a fake
//           MutationGame: argument shapes incl. explicit JSON null, chunked give with read-back,
//           verified teleport and kick, the durable ban list (offline, timed, expiry, corrupt
//           file), the console refusals and log bracket, the shutdown countdown.
//   Part B: the real game half (core/conan/actions_game.cpp + actions_ue.cpp + the stage 1
//           discovery in core/ue) against fake UE objects built in memory from
//           tests/fixtures/l2b-reflection-25639945.json, a subset of the live reflection dump of
//           build 25639945 with the real property and parameter offsets. A fake ProcessEvent
//           plays the engine, and a thread plays the game thread.
#include "common.h"
#include "conan/actions.h"
#include "conan/actions_game.h"
#include "conan/actions_ue.h"
#include "conan/adapter.h"
#include "conan/bans.h"
#include "conan/hook_dispatch.h"
#include "conan/shutdown.h"
#include "conan/text.h"
#include "gamethread.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"
#include "ue/ue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
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

static JsonValue J(const std::string& text) {
    JsonValue v;
    if (!JsonParse(text, v)) printf("bad test json: %s\n", text.c_str());
    return v;
}
static std::string Dump(const JsonValue& v) { return takaro::JsonDump(v); }

static std::string TempDir() {
    char tmpl[] = "/tmp/l2b-test-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

static const char* kSteam = "76561198000735875";
static const char* kOther = "76561198000000002";

// =============================================================================== Part A
struct FakeGame : MutationGame {
    std::vector<OnlinePlayer> online{{kSteam, "werwerwer"}};
    std::map<int32_t, int32_t> backpack;
    int32_t stackMax = 1000, room = 1 << 30;
    std::vector<std::pair<int32_t, int32_t>> giveCalls;
    Vec3 pos{1, 2, 3};
    bool teleportMoves = true;
    std::vector<std::pair<std::string, std::string>> kicks;
    bool kickDisconnects = true;
    std::vector<std::string> consoleCmds, broadcasts;
    int exits = 0;
    int64_t now = 1790000000000;
    int64_t slept = 0;
    std::string logPath;
    bool hookArmed = false;

    bool OnlinePlayers(std::vector<OnlinePlayer>& out, std::string&) override {
        out = online;
        return true;
    }
    bool ResolveItemCode(const std::string& code, int32_t& id, std::string& error) override {
        if (EqualsIgnoreCase(code, "Stone")) return id = 10001, true;
        error = "unknown item code '" + code + "'";
        return false;
    }
    GiveStep GiveChunk(const std::string& s, int32_t t, int32_t q) override {
        GiveStep st;
        st.ok = true;
        giveCalls.push_back({t, q});
        if (s != kSteam) return st.ok = false, st.error = "not online", st;
        if (t != 10001 && t != 12001) return st;  // unknown template: -1, nothing added
        int32_t n = std::min(std::min(q, stackMax), room);
        if (n <= 0) return st;
        room -= n;
        backpack[t] += n;
        st.added = n;
        st.slot = 0;
        return st;
    }
    bool Teleport(const std::string&, const Vec3& to, std::string&) override {
        if (teleportMoves) pos = {to.x, to.y, -20000};
        return true;
    }
    bool Location(const std::string&, Vec3& out, std::string&) override {
        out = pos;
        return true;
    }
    bool Kick(const std::string& s, const std::string& reason, std::string&) override {
        kicks.push_back({s, reason});
        if (kickDisconnects)
            online.erase(std::remove_if(online.begin(), online.end(), [&](const OnlinePlayer& p) { return p.steam64 == s; }),
                         online.end());
        return true;
    }
    ConsoleRun Console(const std::string& c) override {
        consoleCmds.push_back(c);
        ConsoleRun r;
        r.ok = true;
        r.frame = 7345;
        r.context = "player " + std::string(kSteam);
        if (!logPath.empty()) {
            FILE* f = fopen(logPath.c_str(), "a");
            fprintf(f, "[2026.10.03-10.00.00:000][344]LogNet: unrelated earlier line\n");
            fprintf(f, "[2026.10.03-10.00.00:010][345]ConanCheatManager: Your Player ID: 154\n");
            fprintf(f, "[2026.10.03-10.00.00:011][345]LogTemp: second line\n");
            fprintf(f, "[2026.10.03-10.00.00:020][346]LogDataTable: Warning: later noise\n");
            fclose(f);
        }
        return r;
    }
    bool Broadcast(const std::string& m, std::string&) override {
        broadcasts.push_back(m);
        return true;
    }
    bool Exit(std::string&) override {
        exits++;
        return true;
    }
    bool ArmLoginHook(void (*)()) override { return hookArmed = true; }
    int64_t NowMs() override { return now + slept; }
    void SleepMs(int ms) override { slept += ms; }
};

static void TestPureHelpers() {
    CHECK(GiveItemCode(J("{\"item\":\"Stone\"}")) == "Stone", "item");
    CHECK(GiveItemCode(J("{\"item\":10001}")) == "10001", "numeric item");
    CHECK(GiveItemCode(J("{\"item\":null,\"code\":\"x\"}")) == "x", "null item falls through");
    CHECK(GiveItemCode(J("{\"item\":{\"code\":\"Wood\"}}")) == "Wood", "nested");
    CHECK(GiveItemCode(J("{}")).empty(), "none");

    CHECK(ConsoleRefusal("").find("needs a command") != std::string::npos, "empty");
    CHECK(ConsoleRefusal("exit").find("shutdown action") != std::string::npos, "exit");
    CHECK(ConsoleRefusal("QUIT now").find("shutdown action") != std::string::npos, "quit");
    CHECK(ConsoleRefusal("listplayers").find("getPlayers") != std::string::npos, "rcon verb");
    CHECK(ConsoleRefusal("broadcast hi").find("sendMessage") != std::string::npos, "broadcast");
    CHECK(ConsoleRefusal("a\nb").find("control") != std::string::npos, "newline");
    CHECK(ConsoleRefusal("WhatsMyId").empty(), "allowed");

    std::string log =
        "[2026.10.03-10.00.00:000][344]A: x\n[2026.10.03-10.00.00:000][345]B: y\r\n[2026.10.03-10.00.00:000][  5]C: z\n"
        "junk\n[2026.10.03-10.00.00:000][345]D: w";
    auto l = LinesOfFrame(log, 12345);
    CHECK(l.size() == 2 && l[0] == "B: y" && l[1] == "D: w", "frame filter %zu", l.size());
    l = LinesOfFrame(log, 1005);
    CHECK(l.size() == 1 && l[0] == "C: z", "padded frame");
    CHECK(LinesOfFrame(log, -1).empty(), "no frame");

    auto m = CountdownMarks(60);
    CHECK((m == std::vector<int>{60, 30, 10, 5, 4, 3, 2, 1}), "marks 60");
    m = CountdownMarks(300);
    CHECK(m.front() == 300 && m[1] == 240 && m.back() == 1, "marks 300");
    CHECK(CountdownMarks(0).empty(), "marks 0");
    CHECK(CountdownMarks(7) == (std::vector<int>{7, 5, 4, 3, 2, 1}), "marks 7");
    CHECK(CountdownText(120) == "The server shuts down in 2 minutes.", "%s", CountdownText(120).c_str());
    CHECK(CountdownText(60) == "The server shuts down in 1 minute.", "1 min");
    CHECK(CountdownText(1) == "The server shuts down in 1 second.", "1 s");
    CHECK(CountdownText(90) == "The server shuts down in 90 seconds.", "90 s");
}

static void TestBanList() {
    std::string dir = TempDir();
    std::string path = dir + "/Config/Takaro/bans.json";
    BanList b(path);
    b.Load();
    CHECK(b.Error().empty() && b.Size() == 0, "missing file = empty");
    Ban x;
    x.gameId = kSteam;
    x.name = "werwerwer";
    x.reason = "griefing \"quoted\"";
    x.createdAtMs = 1790000000000;
    x.expiresAtMs = 1790000060000;
    std::string err;
    CHECK(b.Upsert(x, err), "%s", err.c_str());
    Ban y = x;
    y.gameId = kOther;
    y.expiresAtMs = 0;
    CHECK(b.Upsert(y, err), "%s", err.c_str());
    BanList c(path);
    c.Load();
    CHECK(c.Size() == 2, "reloaded %zu", c.Size());
    Ban got;
    CHECK(c.Find(kSteam, 1790000000001, got) && got.reason == x.reason && got.expiresAtMs == x.expiresAtMs, "find");
    CHECK(!c.Find(kSteam, 1790000060000, got), "expired not found");
    CHECK(c.Active(1790000070000).size() == 1, "active filters expired");
    auto gone = c.Expire(1790000070000, err);
    CHECK(gone.size() == 1 && gone[0].gameId == kSteam && c.Size() == 1, "expire");
    BanList d(path);
    d.Load();
    CHECK(d.Size() == 1, "expiry persisted");
    bool removed = false;
    CHECK(d.Remove(kOther, removed, err) && removed && d.Size() == 0, "remove");
    CHECK(d.Remove(kOther, removed, err) && !removed, "remove twice");
    JsonValue j = BanToJson(y);
    CHECK(Dump(j) == "{\"player\":{\"gameId\":\"76561198000000002\",\"name\":\"werwerwer\",\"steamId\":"
                     "\"76561198000000002\",\"platformId\":\"steam:76561198000000002\"},\"reason\":\"griefing "
                     "\\\"quoted\\\"\",\"expiresAt\":null}",
          "%s", Dump(j).c_str());
    CHECK(Dump(BanToJson(x)).find("\"expiresAt\":\"2026-09-21T") != std::string::npos, "%s", Dump(BanToJson(x)).c_str());

    // corrupt file: refuse every change, never drop it
    std::string bad = dir + "/bad.json";
    takaro::AtomicWriteFile(bad, "{not json", err);
    BanList e(bad);
    e.Load();
    CHECK(!e.Error().empty(), "corrupt detected");
    CHECK(!e.Upsert(x, err) && err.find("not a valid ban list") != std::string::npos, "%s", err.c_str());
    std::string text;
    bool exists;
    takaro::ReadWholeFile(bad, text, exists, err);
    CHECK(text == "{not json", "corrupt file untouched");
}

static void TestMutationsFake() {
    std::string dir = TempDir();
    auto g = std::make_shared<FakeGame>();
    std::string dirErr;
    takaro::EnsureDirectory(dir + "/Logs", dirErr);
    g->logPath = dir + "/Logs/ConanSandbox.log";
    FILE* f = fopen(g->logPath.c_str(), "w");
    fprintf(f, "[2026.10.03-09.59.59:000][345]Old: same frame tag but before the call\n");
    fclose(f);
    MutationOptions mo;
    mo.savedDir = dir;
    mo.startThreads = false;
    mo.consoleSettleMs = 1;
    mo.shutdownSeconds = 10;
    int exitsAtBeforeExit = -1;
    mo.beforeExit = [&] { exitsAtBeforeExit = g->exits; };
    Mutations m(mo, g);

    // giveItem: chunks of one stack, read back
    auto r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Stone\",\"amount\":2500,"
                                      "\"quality\":null}"));
    CHECK(r.ok && g->backpack[10001] == 2500 && g->giveCalls.size() == 3, "give 2500 in 3 stacks: %s %zu",
          r.error.c_str(), g->giveCalls.size());
    CHECK(g->giveCalls[0].second == 2500 && g->giveCalls[1].second == 1500 && g->giveCalls[2].second == 500,
          "remaining asked each step");
    r = m.Execute("giveItem", J("{\"player\":{\"gameId\":\"steam:76561198000735875\"},\"item\":\"12001\",\"amount\":\"3\","
                                "\"quality\":\"1\"}"));
    CHECK(r.ok && g->backpack[12001] == 3, "numeric code, nested platform id, amount string: %s", r.error.c_str());
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"99999999\",\"amount\":1}"));
    CHECK(!r.ok && r.error.find("is not a Conan item") != std::string::npos, "%s", r.error.c_str());
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"NoSuchItem\"}"));
    CHECK(!r.ok && r.error.find("unknown item code") != std::string::npos, "%s", r.error.c_str());
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Stone\",\"amount\":0}"));
    CHECK(!r.ok && r.error.find("whole number") != std::string::npos, "amount 0");
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Stone\",\"amount\":1.5}"));
    CHECK(!r.ok, "fractional amount");
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000000002\",\"item\":\"Stone\",\"amount\":1}"));
    CHECK(!r.ok && r.error.find("not online") != std::string::npos, "%s", r.error.c_str());
    g->room = 700;
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Stone\",\"amount\":1000}"));
    CHECK(!r.ok && r.error.find("gave 700 of 1000") != std::string::npos, "partial: %s", r.error.c_str());
    g->room = 1 << 30;

    // teleport, verified by read-back
    r = m.Execute("teleportPlayer", J("{\"player\":{\"gameId\":\"76561198000735875\"},\"x\":100.5,\"y\":-200,\"z\":\"300\","
                                      "\"dimension\":null}"));
    CHECK(r.ok && g->pos.x == 100.5 && g->pos.y == -200, "teleport: %s", r.error.c_str());
    g->teleportMoves = false;
    r = m.Execute("teleportPlayer", J("{\"gameId\":\"76561198000735875\",\"x\":5000,\"y\":5000,\"z\":0}"));
    CHECK(!r.ok && r.error.find("not at the target") != std::string::npos, "%s", r.error.c_str());
    g->teleportMoves = true;
    r = m.Execute("teleportPlayer", J("{\"gameId\":\"76561198000735875\",\"x\":1}"));
    CHECK(!r.ok && r.error.find("numeric x, y and z") != std::string::npos, "missing y z");

    // console: log bracket by frame
    r = m.Execute("executeConsoleCommand", J("{\"command\":\" WhatsMyId \"}"));
    CHECK(r.ok && Dump(r.payload) == "{\"success\":true,\"rawResult\":\"ConanCheatManager: Your Player ID: 154\\nLogTemp: "
                                      "second line\"}",
          "%s %s", r.error.c_str(), Dump(r.payload).c_str());
    CHECK(g->consoleCmds.back() == "WhatsMyId", "trimmed");
    r = m.Execute("executeConsoleCommand", J("{\"command\":\"exit\"}"));
    CHECK(!r.ok && g->consoleCmds.size() == 1, "exit refused before the game");
    g->logPath.clear();
    r = m.Execute("executeConsoleCommand", J("{\"command\":\"SomethingSilent\"}"));
    CHECK(r.ok && Dump(r.payload).find("logged no output") != std::string::npos, "%s", Dump(r.payload).c_str());

    // kick: verified by the online list
    r = m.Execute("kickPlayer", J("{\"player\":{\"gameId\":\"76561198000735875\"},\"reason\":null}"));
    CHECK(r.ok && g->kicks.back().second == "Kicked by an admin" && g->online.empty(), "kick: %s", r.error.c_str());
    r = m.Execute("kickPlayer", J("{\"gameId\":\"76561198000735875\",\"reason\":\"x\"}"));
    CHECK(!r.ok && r.error.find("not online") != std::string::npos, "kick offline");
    g->online = {{kSteam, "werwerwer"}};
    g->kickDisconnects = false;
    r = m.Execute("kickPlayer", J("{\"gameId\":\"76561198000735875\",\"reason\":\"stay\"}"));
    CHECK(!r.ok && r.error.find("still connected") != std::string::npos, "unverified kick fails: %s", r.error.c_str());
    g->kickDisconnects = true;

    // bans: online, kicked at once
    r = m.Execute("banPlayer", J("{\"player\":{\"gameId\":\"76561198000735875\"},\"reason\":\"grief\",\"expiresAt\":null}"));
    CHECK(r.ok && g->online.empty() && g->kicks.back().second.find("banned from this server: grief") != std::string::npos,
          "ban online kicks: %s", r.error.c_str());
    {
        // the woken sweep right after the ban action does not kick the same player a second time
        const size_t before = g->kicks.size();
        g->online = {{kSteam, "werwerwer"}};
        CHECK(m.SweepOnce() == 0 && g->kicks.size() == before, "one kick per banned login (action + sweep)");
        g->online.clear();
    }
    // offline, timed
    r = m.Execute("banPlayer", J("{\"gameId\":\"76561198000000002\",\"reason\":\"alt\",\"expiresAt\":\"2026-09-21T14:14:00.000Z\"}"));
    CHECK(r.ok, "offline timed ban: %s", r.error.c_str());
    r = m.Execute("banPlayer", J("{\"gameId\":\"76561198000000002\",\"expiresAt\":\"2020-01-01T00:00:00Z\"}"));
    CHECK(!r.ok && r.error.find("in the past") != std::string::npos, "past expiry");
    r = m.Execute("banPlayer", J("{\"gameId\":\"76561198000000002\",\"expiresAt\":\"tomorrow\"}"));
    CHECK(!r.ok && r.error.find("ISO-8601") != std::string::npos, "bad expiry");
    r = m.Execute("banPlayer", J("{\"gameId\":\"Bob\"}"));
    CHECK(!r.ok && r.error.find("not a Steam64") != std::string::npos, "%s", r.error.c_str());
    r = m.Execute("listBans", J("{}"));
    CHECK(r.ok && r.payload.type == JsonValue::Array && r.payload.arr.size() == 2, "list 2: %s", Dump(r.payload).c_str());
    CHECK(Dump(r.payload).find("\"name\":\"werwerwer\"") != std::string::npos &&
              Dump(r.payload).find("\"name\":\"76561198000000002\"") != std::string::npos &&
              Dump(r.payload).find("\"expiresAt\":\"2026-09-21T14:14:00.000Z\"") != std::string::npos,
          "%s", Dump(r.payload).c_str());
    // enforcement: a banned player who rejoins is kicked by the sweep, which also arms the login hook
    g->online = {{kSteam, "werwerwer"}, {"76561198000000009", "friend"}};
    g->slept += 6000;  // the rejoin comes after the 5 s kick gap
    int kicked = m.SweepOnce();
    CHECK(kicked == 1 && g->online.size() == 1 && g->online[0].steam64 == "76561198000000009" && g->hookArmed,
          "sweep kicks only the banned one");
    // a client that is slow to leave is not kicked again for 5 s (each kick stacks a client dialog)
    g->online = {{kSteam, "werwerwer"}};
    g->kickDisconnects = false;
    CHECK(m.SweepOnce() == 0, "no second kick within the gap");
    g->slept += 5000;
    CHECK(m.SweepOnce() == 1, "kicked again after the gap");
    g->kickDisconnects = true;
    g->online = {{"76561198000000009", "friend"}};
    // timed expiry lifts the ban (Takaro sends no unban)
    g->now = 1790000000000 + 10LL * 24 * 3600 * 1000;
    m.SweepOnce();
    r = m.Execute("listBans", J("[]"));
    CHECK(r.payload.arr.size() == 1 && Dump(r.payload).find(kSteam) != std::string::npos, "expired lifted: %s",
          Dump(r.payload).c_str());
    BanList onDisk(dir + "/Config/Takaro/bans.json");
    onDisk.Load();
    CHECK(onDisk.Size() == 1, "expiry saved");
    r = m.Execute("unbanPlayer", J("{\"gameId\":\"steam:76561198000735875\"}"));
    CHECK(r.ok && m.bans().Size() == 0, "unban");
    r = m.Execute("unbanPlayer", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(r.ok, "unban of a not-banned player is ok");
    r = m.Execute("listBans", J("{}"));
    CHECK(r.ok && Dump(r.payload) == "[]", "empty list");

    // shutdown: answers first, counts down, then exits once
    r = m.Execute("shutdown", J("{}"));
    CHECK(r.ok, "shutdown ok");
    r = m.Execute("shutdown", J("{}"));
    CHECK(r.ok, "second shutdown ok");
    m.JoinShutdown();
    CHECK(g->exits == 1, "one exit (%d)", g->exits);
    CHECK(exitsAtBeforeExit == 0, "beforeExit (shutdown logouts) runs before the exit (%d)", exitsAtBeforeExit);
    CHECK(g->broadcasts.size() == 7 && g->broadcasts[0] == "The server shuts down in 10 seconds." &&
              g->broadcasts[5] == "The server shuts down in 1 second." &&
              g->broadcasts[6] == "The server is shutting down now.",
          "countdown %zu", g->broadcasts.size());
    std::string h = m.HealthJson();
    CHECK(h.find("\"giveItem\":2") != std::string::npos && h.find("\"shutdownScheduled\":true") != std::string::npos,
          "%s", h.c_str());
}

static void TestAdapterDispatch() {
    std::string dir = TempDir();
    auto g = std::make_shared<FakeGame>();
    AdapterOptions ao;
    ao.ready = true;
    ao.chat = [](const ChatRequest&) { return ChatOutcome(); };
    ao.mutationGame = g;
    ao.savedDir = dir;
    ao.mutationOptions.startThreads = false;
    Adapter a(ao);
    auto r = a.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Stone\",\"amount\":2}"));
    CHECK(r.ok && g->backpack[10001] == 2, "adapter routes giveItem: %s", r.error.c_str());
    r = a.Execute("listBans", J("null"));
    CHECK(r.ok && Dump(r.payload) == "[]", "listBans bare array");
    CHECK(a.HealthJson().find("\"mutations\":{") != std::string::npos, "health");
    AdapterOptions no;
    no.refusal = "unsupported server build (x)";
    no.mutationGame = g;
    Adapter b(no);
    r = b.Execute("kickPlayer", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(!r.ok && r.error.find("refused") != std::string::npos && g->kicks.empty(), "refusal wins");
}

// =============================================================================== Part B
namespace fake {

void* Alloc(size_t n) {
    void* p = nullptr;
    if (posix_memalign(&p, 16, n) != 0) abort();
    memset(p, 0, n);
    return p;
}
template <typename T>
void W(uintptr_t a, T v) {
    memcpy((void*)a, &v, sizeof v);
}
template <typename T>
T R(uintptr_t a) {
    T v;
    memcpy(&v, (const void*)a, sizeof v);
    return v;
}

struct Pool {
    uint32_t cur, cursor;
    uintptr_t blocks[4];
};
Pool* g_pool;
uint8_t* g_block;
std::map<std::string, uint32_t> g_names;

uint32_t Name(const std::string& s) {
    std::string k = s;
    for (auto& c : k) c = (char)tolower(c);
    auto it = g_names.find(k);
    if (it != g_names.end()) return it->second;
    uint32_t off = g_pool->cursor;
    uint16_t h = (uint16_t)(s.size() << 6);
    memcpy(g_block + off, &h, 2);
    memcpy(g_block + off + 2, s.data(), s.size());
    uint32_t idx = off / 2;
    off += 2 + (uint32_t)s.size();
    off += off & 1;
    g_pool->cursor = off;
    g_names[k] = idx;
    return idx;
}

struct ObjArray {
    uintptr_t objects;
    int32_t num, max;
};
ObjArray* g_arr;
uintptr_t* g_chunks;
uint8_t* g_chunk0;

uintptr_t NewObject(size_t size, uintptr_t cls, const std::string& name, uintptr_t outer = 0, uint32_t flags = 0) {
    uintptr_t o = (uintptr_t)Alloc(size);
    int32_t idx = g_arr->num++;
    W<uint32_t>(o + 0x08, flags);
    W<int32_t>(o + 0x0C, idx);
    W<uintptr_t>(o + 0x10, cls);
    W<uint32_t>(o + 0x18, Name(name));
    W<uintptr_t>(o + 0x20, outer);
    W<uintptr_t>((uintptr_t)g_chunk0 + idx * 0x18 + 8, o);
    return o;
}

uintptr_t g_metaClass, g_metaFunction, g_metaScriptStruct;
std::map<std::string, uintptr_t> g_fieldClasses, g_classes;
std::map<uintptr_t, std::string> g_funcNames;  // UFunction -> "Owner.Name"
std::map<std::string, std::function<void(uintptr_t, uint8_t*)>> g_handlers;
std::atomic<int> g_calls{0};

uintptr_t FieldClass(const std::string& type) {
    auto it = g_fieldClasses.find(type);
    if (it != g_fieldClasses.end()) return it->second;
    uintptr_t fc = (uintptr_t)Alloc(0x40);
    W<uint32_t>(fc + 0x08, Name(type));
    return g_fieldClasses[type] = fc;
}

void AddProp(uintptr_t owner, const JsonValue& p) {
    uintptr_t f = (uintptr_t)Alloc(0x80);
    W<uintptr_t>(f + 0x08, FieldClass(p.get("type")->str));
    W<uint32_t>(f + 0x20, Name(p.get("name")->str));
    W<int32_t>(f + 0x2C, 1);
    W<int32_t>(f + 0x30, (int32_t)p.get("size")->num);
    W<uint64_t>(f + 0x38, strtoull(p.get("flags")->str.c_str(), nullptr, 16));
    W<int32_t>(f + 0x44, (int32_t)p.get("offset")->num);
    if (p.get("byteMask")) {
        W<uint8_t>(f + 0x70, 1);
        W<uint8_t>(f + 0x71, (uint8_t)p.get("byteOffset")->num);
        W<uint8_t>(f + 0x72, (uint8_t)p.get("byteMask")->num);
        W<uint8_t>(f + 0x73, (uint8_t)p.get("fieldMask")->num);
    }
    // append to the ChildProperties list
    uintptr_t at = owner + 0x50;
    while (R<uintptr_t>(at)) at = R<uintptr_t>(at) + 0x18;
    W<uintptr_t>(at, f);
}

uintptr_t AddFunction(uintptr_t cls, const std::string& owner, const JsonValue& fn) {
    const std::string name = fn.get("name")->str;
    uintptr_t f = NewObject(0x100, g_metaFunction, name, cls);
    W<uint32_t>(f + 0xB0, (uint32_t)strtoul(fn.get("flags")->str.c_str(), nullptr, 16));
    W<uint16_t>(f + 0xB6, (uint16_t)fn.get("parmsSize")->num);
    for (auto& p : fn.get("params")->arr) AddProp(f, p);
    W<uintptr_t>(f + 0x28, R<uintptr_t>(cls + 0x48));  // prepend to Children
    W<uintptr_t>(cls + 0x48, f);
    g_funcNames[f] = owner + "." + name;
    return f;
}

uintptr_t MakeClass(const std::string& name, uintptr_t super) {
    uintptr_t c = NewObject(0x100, g_metaClass, name);
    W<uintptr_t>(c + 0x40, super);
    return g_classes[name] = c;
}

const JsonValue* g_fixture;

uintptr_t FixtureClass(const std::string& name) {
    auto it = g_classes.find(name);
    if (it != g_classes.end()) return it->second;
    const JsonValue* e = g_fixture->get("classes")->get(name);
    uintptr_t super = 0;
    if (name != "Object") {
        const JsonValue* s = e ? e->get("super") : nullptr;
        std::string sn = s && s->type == JsonValue::String ? s->str : "Object";
        if (!g_fixture->get("classes")->get(sn)) sn = "Object";
        super = FixtureClass(sn);
    }
    uintptr_t c = MakeClass(name, super);
    if (e) {
        for (auto& p : e->get("properties")->arr) AddProp(c, p);
        for (auto& f : e->get("functions")->arr) AddFunction(c, name, f);
    }
    return c;
}

void ProcessEvent(void* obj, void* func, void* parms) {
    g_calls++;
    auto it = g_funcNames.find((uintptr_t)func);
    if (it == g_funcNames.end()) {
        printf("FAIL fake ProcessEvent: unknown function %p\n", func);
        g_failed++;
        return;
    }
    auto h = g_handlers.find(it->second);
    if (h == g_handlers.end()) {
        printf("FAIL fake ProcessEvent: no handler for %s\n", it->second.c_str());
        g_failed++;
        return;
    }
    h->second((uintptr_t)obj, (uint8_t*)parms);
}

std::u16string ReadFStr(const uint8_t* at) {
    uintptr_t data;
    int32_t num;
    memcpy(&data, at, 8);
    memcpy(&num, at + 8, 4);
    if (!data || num <= 1) return u"";
    return std::u16string((const char16_t*)data, (size_t)num - 1);
}
std::u16string* NewFStr(uintptr_t at, const std::u16string& s) {
    auto* keep = new std::u16string(s + u'\0');
    W<uintptr_t>(at, (uintptr_t)keep->data());
    W<int32_t>(at + 8, (int32_t)keep->size());
    W<int32_t>(at + 12, (int32_t)keep->size());
    return keep;
}

}  // namespace fake

struct World {
    uintptr_t pc = 0, ps = 0, pawn = 0, inv = 0, gs = 0, gm = 0, sysLib = 0, textLib = 0, postLogin = 0;
    uintptr_t players = 0;  // PlayerArray data
    std::map<int32_t, int32_t> counts;
    double loc[3] = {10, 20, 30};
    std::vector<std::string> kicks, told, commands;
    std::vector<int> adminDuringCall;
    std::vector<uintptr_t> execSpecific, execWorld;
    bool tpCheat = true, tpSnap = false;
    float durPct = -1;
    float dur = 0;
    int addCalls = 0, addIndex = 0;
    std::string logPath;
};
static World G;

static void SetOnline(bool on) {
    fake::W<uintptr_t>(G.players, G.ps);
    fake::W<int32_t>(G.gs + 776 + 8, on ? 1 : 0);
}

static void BuildWorld(const std::string& dir) {
    using namespace fake;
    g_pool = (Pool*)Alloc(sizeof(Pool));
    g_block = (uint8_t*)Alloc(0x20000);
    g_pool->blocks[0] = (uintptr_t)g_block;
    g_pool->cursor = 0;
    g_arr = (ObjArray*)Alloc(sizeof(ObjArray));
    g_chunks = (uintptr_t*)Alloc(64);
    g_chunk0 = (uint8_t*)Alloc(65536 * 0x18);
    g_chunks[0] = (uintptr_t)g_chunk0;
    g_arr->objects = (uintptr_t)g_chunks;
    // every name the stage 1 discovery wants must exist
    for (auto* n : {"ClientReceiveChatMessage", "ConanPlayerController", "Function", "Class", "GameStateBase",
                    "PlayerArray", "Owner", "UserIDFromURLOptions", "PlayerNamePrivate", "ArrayProperty",
                    "ObjectProperty", "StrProperty"})
        Name(n);
    g_metaClass = NewObject(0x100, 0, "Class");
    W<uintptr_t>(g_metaClass + 0x10, g_metaClass);
    g_metaFunction = NewObject(0x100, g_metaClass, "Function");
    g_metaScriptStruct = NewObject(0x100, g_metaClass, "ScriptStruct");

    static std::string text;
    std::string err;
    bool exists;
    takaro::ReadWholeFile("tests/fixtures/l2b-reflection-25639945.json", text, exists, err);
    static JsonValue fixture;
    CHECK(exists && JsonParse(text, fixture), "fixture loads");
    g_fixture = &fixture;
    for (auto& kv : fixture.get("classes")->obj) FixtureClass(kv.first);

    // Blueprint subclasses as on the live server
    uintptr_t pcC = MakeClass("FunCombat_PlayerController_C", g_classes["ConanPlayerController"]);
    uintptr_t pawnC = MakeClass("BasePlayerChar_C", g_classes["ConanCharacter"]);
    uintptr_t gsC = MakeClass("BaseGameState_C", g_classes["GameStateBase"]);
    uintptr_t gmC = MakeClass("BaseGameMode_C", g_classes["GameModeBase"]);
    for (auto& f : fixture.get("classes")->get("GameModeBase")->get("functions")->arr)
        if (f.get("name")->str == "K2_PostLogin") G.postLogin = AddFunction(gmC, "BaseGameMode_C", f);
    uintptr_t psC = g_classes["PlayerState"];

    // instances
    G.gs = NewObject(0x400, gsC, "BaseGameState_C_1");
    G.gm = NewObject(0x400, gmC, "BaseGameMode_C_1");
    G.pc = NewObject(0x1200, pcC, "FunCombat_PlayerController_C_1");
    G.ps = NewObject(0x400, psC, "PlayerState_1");
    G.pawn = NewObject(0x400, pawnC, "BasePlayerChar_C_1");
    G.inv = NewObject(0x300, g_classes["ItemInventory"], "ItemInventory_1");
    G.sysLib = NewObject(0x40, g_classes["KismetSystemLibrary"], "Default__KismetSystemLibrary", 0, 0x10);
    G.textLib = NewObject(0x40, g_classes["KismetTextLibrary"], "Default__KismetTextLibrary", 0, 0x10);
    W<uintptr_t>(G.gs + 760, G.gm);  // AuthorityGameMode
    G.players = (uintptr_t)Alloc(64);
    W<uintptr_t>(G.gs + 776, G.players);
    SetOnline(true);
    W<uintptr_t>(G.ps + 360, G.pc);   // Owner
    NewFStr(G.ps + 896, u"Limon#67642");  // PlayerNamePrivate
    // A Funcom Live Services account: UserIDFromURLOptions holds the FLS id, not the Steam64 (lane L3).
    // The Steam64 is the FUniqueNetId behind PlayerState.UniqueID: object pointer at +8, FString at +16.
    NewFStr(G.pc + 2992, u"A-1HFFLI28NN");
    W<uintptr_t>(G.pc + 760, G.ps);  // Controller.PlayerState
    uintptr_t netId = (uintptr_t)Alloc(64);
    NewFStr(netId + 16, u"76561198000735875");
    W<uintptr_t>(G.ps + 768 + 8, netId);  // PlayerState.UniqueID -> FUniqueNetId
    W<uintptr_t>(G.pc + 816, G.pawn);  // Pawn

    // item-name tables: one plain, one with a hole, and a decoy the connector must not use
    uintptr_t rowStruct = NewObject(0x100, g_metaScriptStruct, "ItemNameToTemplateIDStruct");
    W<int32_t>(rowStruct + 0x58, 4);
    auto table = [&](const std::string& name, std::vector<std::pair<std::string, int32_t>> rows, int hole) {
        uintptr_t t = NewObject(0x80, g_classes["DataTable"], name);
        W<uintptr_t>(t + 0x28, rowStruct);
        uintptr_t data = (uintptr_t)Alloc(24 * (rows.size() + 1));
        uint32_t bits = 0;
        int32_t n = 0;
        for (size_t i = 0; i < rows.size(); i++, n++) {
            if ((int)i == hole) {  // a free slot: garbage the reader must skip
                W<uint32_t>(data + 24 * n, Name("Garbage"));
                W<uintptr_t>(data + 24 * n + 8, (uintptr_t)Alloc(8));
                n++;
            }
            bits |= 1u << n;
            uintptr_t row = (uintptr_t)Alloc(8);
            W<int32_t>(row, rows[i].second);
            W<uint32_t>(data + 24 * n, Name(rows[i].first));
            W<uintptr_t>(data + 24 * n + 8, row);
        }
        W<uintptr_t>(t + 0x30, data);
        W<int32_t>(t + 0x38, n);
        W<uint32_t>(t + 0x40, bits);
        W<int32_t>(t + 0x64, hole >= 0 ? 1 : 0);
    };
    table("ItemNameToHarvestXPValue", {{"Stone", 777}}, -1);
    table("ItemNameToTemplateID", {{"Stone", 10001}, {"Wood", 10011}}, -1);
    table("DLC_Siptah_ItemNameToTemplateID", {{"Siptah_Shard", 53000}, {"Siptah_Gem", 53001}}, 1);

    // engine behaviour
    auto& H = g_handlers;
    H["ConanPlayerController.ClientReceiveChatMessage"] = [](uintptr_t, uint8_t*) {};
    H["ConanCharacter.GetBackpackInventory"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.pawn, "backpack asked of the pawn");
        memcpy(p, &G.inv, 8);
    };
    H["ItemInventory.GetNumberOfItemsByTemplate"] = [](uintptr_t, uint8_t* p) {
        int32_t t, n;
        memcpy(&t, p, 4);
        n = G.counts[t];
        memcpy(p + 4, &n, 4);
    };
    H["ItemInventory.AddItemTemplate"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.inv, "added to the backpack");
        int32_t t, idx, q, ret = -1;
        uint64_t ctx;
        memcpy(&t, p, 4);
        memcpy(&idx, p + 4, 4);
        memcpy(&ctx, p + 8, 8);
        memcpy(&q, p + 16, 4);
        memcpy(&G.durPct, p + 24, 4);
        memcpy(&G.dur, p + 28, 4);
        G.addCalls++;
        G.addIndex = idx;
        CHECK(ctx == 0 && p[20] == 0, "Context None, loot false");
        if (t == 10001 || t == 53001) {
            int32_t n = std::min(q, 1000);
            G.counts[t] += n;
            ret = 3;
        }
        memcpy(p + 32, &ret, 4);
    };
    H["ConanPlayerController.TeleportPlayerServer"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.pc, "teleport on the controller");
        memcpy(G.loc, p, 24);
        G.loc[2] = -21000;  // SnapToGround
        G.tpCheat = p[48] != 0;
        G.tpSnap = p[49] != 0;
        CHECK(p[50] && p[51], "ForceTeleportClients, UsePawnRotationInstead");
    };
    H["Actor.K2_GetActorLocation"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.pawn, "location of the pawn");
        memcpy(p, G.loc, 24);
    };
    H["KismetTextLibrary.Conv_StringToText"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.textLib, "text library CDO");
        auto* s = new std::u16string(fake::ReadFStr(p));
        uintptr_t ptr = (uintptr_t)s;
        memcpy(p + 16, &ptr, 8);
        uint64_t magic = 0x7e47;
        memcpy(p + 24, &magic, 8);
    };
    H["PlayerController.ClientWasKicked"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.pc, "the kicked controller is told the reason");
        uintptr_t ptr;
        memcpy(&ptr, p, 8);
        G.told.push_back(Utf16To8(*(std::u16string*)ptr));
    };
    H["PlayerController.ClientReturnToMainMenuWithTextReason"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.pc, "kick the controller");
        uintptr_t ptr;
        uint64_t magic;
        memcpy(&ptr, p, 8);
        memcpy(&magic, p + 8, 8);
        CHECK(magic == 0x7e47, "the FText from Conv_StringToText");
        G.kicks.push_back(Utf16To8(*(std::u16string*)ptr));
        SetOnline(false);  // the client leaves
    };
    H["KismetSystemLibrary.GetFrameCount"] = [](uintptr_t, uint8_t* p) {
        int64_t f = 120345;
        memcpy(p, &f, 8);
    };
    H["KismetSystemLibrary.ExecuteConsoleCommand"] = [](uintptr_t o, uint8_t* p) {
        CHECK(o == G.sysLib, "system library CDO");
        uintptr_t world, specific;
        memcpy(&world, p, 8);
        memcpy(&specific, p + 24, 8);
        std::string cmd = Utf16To8(fake::ReadFStr(p + 8));
        G.commands.push_back(cmd);
        G.execWorld.push_back(world);
        G.execSpecific.push_back(specific);
        G.adminDuringCall.push_back(specific ? (fake::R<uint8_t>(G.pc + 4360) & 1) : -1);
        FILE* f = fopen(G.logPath.c_str(), "a");
        fprintf(f, "[2026.10.03-10.00.00:000][345]ConanCheatManager: ran %s\n", cmd.c_str());
        fclose(f);
    };
    takaro::EnsureDirectory(dir + "/Logs", err);
    G.logPath = dir + "/Logs/ConanSandbox.log";
    FILE* f = fopen(G.logPath.c_str(), "w");
    fclose(f);
    UE::SetGlobals((uintptr_t)g_arr, (uintptr_t)g_pool->blocks, &fake::ProcessEvent);
}

static void TestReflectedGame() {
    std::string dir = TempDir();
    BuildWorld(dir);
    std::atomic<bool> stop{false};
    std::thread gameThread([&] {
        while (!stop) {
            GameThread::Drain();
            usleep(500);
        }
    });

    // the toolkit itself
    std::string err;
    auto idx = rx::FindNameIndices({"ItemNameToTemplateID", "nope_not_here", "conanplayercontroller"});
    CHECK(idx[0] >= 0 && idx[1] == -1 && idx[2] >= 0, "name scan");
    CHECK(rx::NameText((uint32_t)idx[0]) == "ItemNameToTemplateID", "decode");
    rx::Func tp;
    CHECK(rx::ResolveFunc(rx::ClassOf(G.pc), "TeleportPlayerServer",
                          {{"TargetLocation", "StructProperty", 24}, {"RunCheatCheck", "BoolProperty", 1}}, tp, err),
          "%s", err.c_str());
    CHECK(tp.parmsSize == 52 && tp.P("RunCheatCheck")->offset == 48, "real offsets from the dump");
    CHECK(!rx::ResolveFunc(rx::ClassOf(G.pc), "TeleportPlayerServer", {{"RunCheatCheck", "IntProperty", 4}}, tp, err) &&
              err.find("RunCheatCheck is not IntProperty") != std::string::npos,
          "type check: %s", err.c_str());
    rx::Param adm;
    CHECK(rx::FindProperty(rx::ClassOf(G.pc), "m_IsAdmin", "BoolProperty", 1, adm) && adm.offset == 4360 &&
              adm.boolByteMask == 1,
          "m_IsAdmin");
    CHECK(rx::IsA(G.pawn, "ConanCharacter") && !rx::IsA(G.pc, "ConanCharacter"), "IsA by name");

    CHECK(rx::Steam64Of(G.pc, "A-1HFFLI28NN") == kSteam, "Steam64 from PlayerState.UniqueID, not the FLS id");
    fake::W<uintptr_t>(G.ps + 768 + 8, 0);
    CHECK(rx::Steam64Of(G.pc, "A-1HFFLI28NN") == "A-1HFFLI28NN" && rx::Steam64Of(G.pc, kSteam) == kSteam,
          "no net id: the URL id, the same gameId getPlayers reports (conan/identity.h)");
    fake::W<uintptr_t>(G.ps + 768 + 8, 0x10);
    CHECK(rx::Steam64Of(G.pc, "").empty(), "implausible net id pointer is not followed");
    {
        uintptr_t netId = (uintptr_t)fake::Alloc(64);
        fake::NewFStr(netId + 16, u"STEAM:76561198000735875");
        fake::W<uintptr_t>(G.ps + 768 + 8, netId);
        CHECK(rx::Steam64Of(G.pc, "A-1HFFLI28NN") == kSteam, "STEAM: prefix stripped");
    }
    auto game = MakeUeMutationGame();
    std::vector<OnlinePlayer> online;
    CHECK(game->OnlinePlayers(online, err) && online.size() == 1 && online[0].steam64 == kSteam &&
              online[0].name == "Limon#67642",
          "online via the stage 1 discovery: %s", err.c_str());

    MutationOptions mo;
    mo.savedDir = dir;
    mo.startThreads = false;
    mo.consoleSettleMs = 1;
    mo.verifyTimeoutMs = 1000;
    Mutations m(mo, game);

    auto r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"stone\",\"amount\":2500}"));
    CHECK(r.ok && G.counts[10001] == 2500 && G.addCalls == 3 && G.addIndex == -1 && G.durPct == 1.0f && G.dur == -1.0f,
          "giveItem by name through AddItemTemplate, full durability (-1 = by percentage): %s (calls %d)", r.error.c_str(), G.addCalls);
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Siptah_Gem\",\"amount\":2}"));
    CHECK(r.ok && G.counts[53001] == 2, "DLC table row after a hole: %s", r.error.c_str());
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"Garbage\",\"amount\":2}"));
    CHECK(!r.ok && r.error.find("unknown item code") != std::string::npos, "free slot skipped: %s", r.error.c_str());
    r = m.Execute("giveItem", J("{\"gameId\":\"76561198000735875\",\"item\":\"10011\",\"amount\":1}"));
    CHECK(!r.ok && r.error.find("is not a Conan item") != std::string::npos, "AddItemTemplate -1: %s", r.error.c_str());

    r = m.Execute("teleportPlayer", J("{\"gameId\":\"76561198000735875\",\"x\":-88937,\"y\":256066,\"z\":-20000}"));
    CHECK(r.ok && G.loc[0] == -88937 && !G.tpCheat && G.tpSnap, "teleport: %s", r.error.c_str());

    r = m.Execute("executeConsoleCommand", J("{\"command\":\"WhatsMyId\"}"));
    CHECK(r.ok && Dump(r.payload).find("ConanCheatManager: ran WhatsMyId") != std::string::npos, "console: %s %s",
          r.error.c_str(), Dump(r.payload).c_str());
    CHECK(G.execSpecific.back() == G.pc && G.execWorld.back() == G.pc && G.adminDuringCall.back() == 1 &&
              (fake::R<uint8_t>(G.pc + 4360) & 1) == 0,
          "admin flag set only during the call");

    r = m.Execute("kickPlayer", J("{\"gameId\":\"76561198000735875\",\"reason\":\"Bye now\"}"));
    CHECK(r.ok && G.kicks.size() == 1 && G.kicks[0] == "Bye now" && G.told.size() == 1 && G.told[0] == "Bye now", "kick with the reason: %s", r.error.c_str());

    // with nobody online the console is the engine's
    r = m.Execute("executeConsoleCommand", J("{\"command\":\"stat fps\"}"));
    CHECK(r.ok && G.execSpecific.back() == 0 && G.execWorld.back() == G.gs, "engine console without players");

    // bans: offline ban, rejoin -> sweep kicks and arms the K2_PostLogin hook
    r = m.Execute("banPlayer", J("{\"gameId\":\"76561198000735875\",\"reason\":\"grief\"}"));
    CHECK(r.ok, "offline ban: %s", r.error.c_str());
    SetOnline(true);
    CHECK(m.SweepOnce() == 1 && G.kicks.back().find("banned from this server: grief") != std::string::npos,
          "banned rejoin kicked");
    CHECK(HookDispatch::HealthJson().find("\"owner\":\"bans\",\"function\":\"GameModeBase.K2_PostLogin\"") !=
              std::string::npos,
          "login hook subscribed in the shared registry: %s", HookDispatch::HealthJson().c_str());
    HookDispatch::BindForTest("K2_PostLogin", G.postLogin);
    uint64_t before = g_loginCount.load();
    uint8_t parms[8];
    memcpy(parms, &G.pc, 8);
    static int originals = 0;
    auto original = [](void*, void*, void*) { originals++; };
    const HookDispatch::Slot* slot = HookDispatch::Match((void*)G.postLogin);
    CHECK(slot != nullptr, "K2_PostLogin matched by the detour's hot path");
    if (slot) HookDispatch::Invoke(slot, (void*)G.gm, (void*)G.postLogin, parms, original);
    CHECK(g_loginCount.load() == before + 1 && originals == 1, "K2_PostLogin hook fires after the original");
    CHECK(HookDispatch::Match((void*)0x1234) == nullptr, "other functions do not match");
    r = m.Execute("unbanPlayer", J("{\"gameId\":\"76561198000735875\"}"));
    CHECK(r.ok && m.bans().Size() == 0, "unban");

    // the engine exit for shutdown
    CHECK(game->Exit(err) && G.commands.back() == "exit" && G.execSpecific.back() == 0 && G.execWorld.back() == G.gs,
          "exit through ExecuteConsoleCommand, engine console with nobody online: %s", err.c_str());
    SetOnline(true);
    CHECK(game->Exit(err) && G.commands.back() == "exit" && G.execSpecific.back() == G.pc &&
              G.adminDuringCall.back() == 1 && (fake::R<uint8_t>(G.pc + 4360) & 1) == 0,
          "exit with a player online runs in that console with the admin flag: %s", err.c_str());

    stop = true;
    gameThread.join();
}

int main() {
    TestPureHelpers();
    TestBanList();
    TestMutationsFake();
    TestAdapterDispatch();
    TestReflectedGame();
    printf("l2b tests: %d/%d checks passed\n", g_ran - g_failed, g_ran);
    return g_failed ? 1 : 0;
}
