// Host-side unit tests: the .sym parser (against a synthetic fixture), JSON, the event ring buffer
// and password redaction. No game process involved.
#include "mapsurvey.h"
#include "common.h"
#include "config_file.h"
#include "perf.h"
#include "state.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_failed = 0, g_ran = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_ran++;                                                             \
        if (!(cond)) {                                                       \
            g_failed++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            printf("     ");                                                 \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)
#define EQ(a, b) CHECK((a) == (b), "got '%s' want '%s'", std::string(a).c_str(), std::string(b).c_str())

// ---------------------------------------------------------------------------------------------
// .sym fixture: the exact on-disk shape sym.cpp parses.
//   u32 N | N x {u64 rva, u32 line, u32 fileOff, u32 nameOff} | '\n'-separated name table

struct Rec {
    uint64_t rva;
    uint32_t line, fileOff, nameOff;
};

static std::string BuildSymFixture(std::vector<Rec>& recs, const std::vector<std::string>& names,
                                   std::vector<uint32_t>& nameOffs) {
    std::string table;
    for (auto& n : names) {
        nameOffs.push_back((uint32_t)table.size());
        table += n;
        table += "\n";
    }
    std::string out;
    uint32_t n = (uint32_t)recs.size();
    out.append((const char*)&n, 4);
    for (auto& r : recs) {
        out.append((const char*)&r.rva, 8);
        out.append((const char*)&r.line, 4);
        out.append((const char*)&r.fileOff, 4);
        out.append((const char*)&r.nameOff, 4);
    }
    out += table;
    return out;
}

// Mirrors the two passes of ScanSymFile(): name table -> wanted offsets, records -> lowest rva.
struct Resolved {
    uint64_t rva = 0;
    uint32_t records = 0;
    bool contiguous = true;
    std::string signature;
};

static std::map<std::string, Resolved> ScanFixture(const std::string& blob,
                                                   const std::vector<std::pair<std::string, std::string>>& wanted) {
    std::map<std::string, Resolved> out;
    uint32_t n;
    memcpy(&n, blob.data(), 4);
    const char* names = blob.data() + 4 + (size_t)n * 20;
    size_t namesLen = blob.size() - 4 - (size_t)n * 20;
    std::map<std::string, std::string> byExact, byBase;  // key -> wanted name
    for (auto& w : wanted) {
        if (!w.second.empty()) byExact[w.second] = w.first;
        else byBase[w.first] = w.first;
    }
    std::map<uint32_t, std::vector<std::string>> offToName;
    size_t pos = 0;
    while (pos < namesLen) {
        const char* nl = (const char*)memchr(names + pos, '\n', namesLen - pos);
        size_t end = nl ? (size_t)(nl - names) : namesLen;
        std::string line(names + pos, end - pos);
        auto e = byExact.find(line);
        if (e != byExact.end()) offToName[(uint32_t)pos].push_back(e->second);
        std::string base = line.substr(0, line.find('('));
        auto b = byBase.find(base);
        if (b != byBase.end()) offToName[(uint32_t)pos].push_back(b->second);
        if (!nl) break;
        pos = end + 1;
    }
    std::map<std::string, std::pair<uint32_t, uint32_t>> runs;  // first/last record index
    for (uint32_t i = 0; i < n; i++) {
        Rec r;
        memcpy(&r.rva, blob.data() + 4 + (size_t)i * 20, 8);
        memcpy(&r.nameOff, blob.data() + 4 + (size_t)i * 20 + 16, 4);
        auto it = offToName.find(r.nameOff);
        if (it == offToName.end()) continue;
        for (const std::string& key : it->second) {
            Resolved& e = out[key];
            if (!e.rva || r.rva < e.rva) {
                e.rva = r.rva;
                const char* nl = (const char*)memchr(names + r.nameOff, '\n', namesLen - r.nameOff);
                e.signature.assign(names + r.nameOff, nl ? (size_t)(nl - names - r.nameOff) : 0);
            }
            e.records++;
            auto& run = runs[key];
            if (e.records == 1) run.first = i;
            run.second = i;
        }
    }
    for (auto& kv : out) {
        auto& run = runs[kv.first];
        kv.second.contiguous = (run.second - run.first + 1) == kv.second.records;
    }
    return out;
}

static void TestSymParser() {
    std::vector<std::string> names = {
        "/a/file.cpp",                                        // 0
        "UObject::ProcessEvent(UFunction*, void*)",           // 1
        "FName::ToString() const",                            // 2
        "FName::ToString(FString&) const",                    // 3
        "SomethingElse(int)",                                 // 4
    };
    std::vector<uint32_t> off;
    std::vector<Rec> recs;
    std::string blob;
    {
        std::string table;
        for (auto& nm : names) { off.push_back((uint32_t)table.size()); table += nm; table += "\n"; }
        // Records sorted by rva, one contiguous run per function.
        recs = {
            {0x1000, 1, off[0], off[1]}, {0x1010, 2, off[0], off[1]}, {0x1020, 3, off[0], off[1]},
            {0x2000, 1, off[0], off[3]}, {0x2008, 2, off[0], off[3]},
            {0x3000, 1, off[0], off[2]},
            {0x4000, 1, off[0], off[4]},
        };
        std::vector<uint32_t> tmp;
        blob = BuildSymFixture(recs, names, tmp);
    }
    auto r = ScanFixture(blob, {{"UObject::ProcessEvent", "UObject::ProcessEvent(UFunction*, void*)"},
                                {"FName::ToString", ""},                        // base-name: lowest rva wins
                                {"FName::ToStringInto", "FName::ToString(FString&) const"}});
    CHECK(r.count("UObject::ProcessEvent") == 1, "ProcessEvent missing");
    CHECK(r["UObject::ProcessEvent"].rva == 0x1000, "got 0x%llx", (unsigned long long)r["UObject::ProcessEvent"].rva);
    CHECK(r["UObject::ProcessEvent"].records == 3, "records=%u", r["UObject::ProcessEvent"].records);
    CHECK(r["UObject::ProcessEvent"].contiguous, "run should be contiguous");
    // base-name lookup collapses both ToString overloads and keeps the lowest rva
    CHECK(r["FName::ToString"].rva == 0x2000, "got 0x%llx", (unsigned long long)r["FName::ToString"].rva);
    CHECK(r["FName::ToString"].records == 3, "base name must span both overloads, got %u",
          r["FName::ToString"].records);
    // the pinned overload resolves independently to its own address
    CHECK(r["FName::ToStringInto"].rva == 0x2000, "pinned overload");
    EQ(r["FName::ToStringInto"].signature, "FName::ToString(FString&) const");
    CHECK(r.count("SomethingElse") == 0, "unwanted name was collected");
    // vaddr = rva + loadBase
    CHECK(r["UObject::ProcessEvent"].rva + 0x200000 == 0x201000, "vaddr math");
}

static void TestJson() {
    JsonValue v;
    CHECK(JsonParse(R"({"a":1,"b":"x\ny","c":[1,2,{"d":null}],"e":true,"f":-1.5})", v), "parse failed");
    CHECK(v.type == JsonValue::Object, "not an object");
    EQ(v.get("b")->str, "x\ny");
    EQ(v.get("a")->str, "1");
    CHECK(v.get("c")->arr.size() == 3, "array size");
    CHECK(v.get("e")->b, "bool");
    CHECK(v.get("f")->num == -1.5, "number");
    // 64-bit integers survive as text
    CHECK(JsonParse(R"({"id":9223372036854775807})", v), "bigint parse");
    EQ(v.get("id")->str, "9223372036854775807");
    // malformed input is rejected, never crashes
    CHECK(!JsonParse("{", v), "unterminated object accepted");
    CHECK(!JsonParse("{\"a\":}", v), "missing value accepted");
    CHECK(!JsonParse("[1,2", v), "unterminated array accepted");
    CHECK(!JsonParse("{} trailing", v), "trailing junk accepted");
    CHECK(!JsonParse("", v), "empty accepted");
    EQ(JsonEscape("a\"b\\c\n"), "a\\\"b\\\\c\\n");
    EQ(JsonNum(3), "3");
    EQ(JsonNum(1.5), "1.5");
}

static void TestRedaction() {
    EQ(Redact("WorldPassword=s3cr3t-fixture"), "WorldPassword=***");
    EQ(Redact("AdminPassword=hunter2 rest"), "AdminPassword=*** rest");
    EQ(Redact("[Settings] WorldPassword=abc\nServerName=Takaro"), "[Settings] WorldPassword=***\nServerName=Takaro");
    EQ(Redact("{\"WorldPassword\":\"abc\",\"x\":1}"), "{\"WorldPassword\":\"***\",\"x\":1}");
    EQ(Redact("-Password=abc"), "-Password=***");
    EQ(Redact("no secrets here"), "no secrets here");
    // key without a value must not eat the rest of the line
    EQ(Redact("WorldPassword"), "WorldPassword");
    EQ(Redact("WorldPassword="), "WorldPassword=");
    EQ(Redact("Ticket=abcdef rest"), "Ticket=*** rest");
    EQ(Redact("TAKARO_PLUGIN_TOKEN=xyz"), "TAKARO_PLUGIN_TOKEN=***");
}

static void TestRingBuffer() {
    auto& st = PluginState::Get();
    CHECK(st.LatestSeq() == 0, "fresh state should start at 0");
    for (int i = 0; i < 10; i++) st.EmitEvent("log", "{\"n\":" + std::to_string(i) + "}");
    CHECK(st.LatestSeq() == 10, "latestSeq=%llu", (unsigned long long)st.LatestSeq());

    JsonValue v;
    CHECK(JsonParse(st.EventsJson(0, 5), v), "events json invalid");
    CHECK(v.get("events")->arr.size() == 5, "limit not honoured");
    EQ(v.get("seq")->str, "5");
    EQ(v.get("latestSeq")->str, "10");
    CHECK(v.get("truncated")->b == false, "nothing dropped yet");
    EQ(v.get("bootId")->str, BootId());

    // cursor round-trip: the returned seq fetches exactly the remainder
    CHECK(JsonParse(st.EventsJson(5, 100), v), "events json invalid");
    CHECK(v.get("events")->arr.size() == 5, "remainder");
    EQ(v.get("events")->arr[0].get("seq")->str, "6");

    // nothing new -> cursor stays put, empty list
    CHECK(JsonParse(st.EventsJson(10, 100), v), "events json invalid");
    CHECK(v.get("events")->arr.empty(), "should be empty");
    EQ(v.get("seq")->str, "10");

    // overflow drops the oldest and reports truncation
    for (size_t i = 0; i < PluginState::kMaxEvents + 50; i++) st.EmitEvent("log", "{}");
    CHECK(st.Buffered() == PluginState::kMaxEvents, "buffered=%zu", st.Buffered());
    CHECK(JsonParse(st.EventsJson(1, 10), v), "events json invalid");
    CHECK(v.get("truncated")->b == true, "truncated flag not set after overflow");

    // a cursor from a previous boot (ahead of latestSeq) yields nothing and a sane cursor
    CHECK(JsonParse(st.EventsJson(st.LatestSeq() + 1000, 10), v), "events json invalid");
    CHECK(v.get("events")->arr.empty(), "future cursor");
}

static void TestCapabilities() {
    auto& st = PluginState::Get();
    st.SetCapability("players", "degraded", "waiting for game world");
    EQ(st.Capability("players"), "degraded");
    EQ(st.Capability("nonexistent"), "unimplemented");
    JsonValue v;
    CHECK(JsonParse(st.CapabilitiesJson(), v), "capabilities json invalid");
    EQ(v.get("players")->str, "degraded");
    CHECK(JsonParse(st.CapabilityDetailsJson(), v), "details json invalid");
    EQ(v.get("players")->str, "waiting for game world");
    st.SetCapability("players", "ok", "");
    CHECK(JsonParse(st.CapabilityDetailsJson(), v), "details json invalid");
    CHECK(v.get("players") == nullptr, "empty detail should be omitted");
}

// Lane L3b: the plugin-side ban list must round-trip through bans.json and survive a reload,
// because it is what the PreLogin hook refuses a rejoin with after a restart.
static void TestPluginBanList() {
    setenv("TAKARO_PLUGIN_DATA_DIR", "/tmp/takaro-dragonwilds-test-bans", 1);
    ::mkdir("/tmp/takaro-dragonwilds-test-bans", 0755);
    ::unlink(state::BansPath().c_str());

    state::BansLoad();
    CHECK(!state::IsBanned("0123456789abcdef0123456789abcdef"), "nothing should be banned yet");

    state::BanRecord r;
    r.gameId = "0123456789ABCDEF0123456789ABCDEF";  // upper case on purpose
    r.name = "takarotester";
    r.reason = "L3b test";
    CHECK(state::BanAdd(r), "BanAdd should accept the record");
    // BanAdd is memory-only (it runs on the game thread); persistence is a background flush.
    CHECK(state::FlushBans(), "FlushBans should persist");
    CHECK(state::IsBanned("0123456789abcdef0123456789abcdef"), "lookup must be case-insensitive");
    CHECK(state::IsBanned("0123456789ABCDEF0123456789ABCDEF"), "lookup must be case-insensitive");
    CHECK(!state::IsBanned(""), "an empty id is never banned");

    std::string text;
    CHECK(ReadFile(state::BansPath(), text), "bans.json not written");
    JsonValue v;
    CHECK(JsonParse(text, v), "bans.json is not valid JSON");
    CHECK(v.get("bans")->arr.size() == 1, "one ban expected");
    EQ(v.get("bans")->arr[0].get("gameId")->str, "0123456789abcdef0123456789abcdef");
    EQ(v.get("bans")->arr[0].get("reason")->str, "L3b test");
    CHECK(!v.get("bans")->arr[0].get("createdAt")->str.empty(), "createdAt should be stamped");

    auto list = state::BanList();
    CHECK(list.size() == 1 && list[0].name == "takarotester", "BanList should report the entry");

    CHECK(state::BanRemove("0123456789abcdef0123456789abcdef"), "BanRemove should report a hit");
    CHECK(!state::BanRemove("0123456789abcdef0123456789abcdef"), "a second remove is a miss");
    CHECK(state::FlushBans(), "FlushBans after remove");
    CHECK(!state::IsBanned("0123456789abcdef0123456789abcdef"), "unbanned");
    CHECK(ReadFile(state::BansPath(), text) && JsonParse(text, v) && v.get("bans")->arr.empty(),
          "the removal should be persisted");
    ::unlink(state::BansPath().c_str());
}

// Timed-ban expiry and crash recovery depend on the revision guard: any mutation, including a
// remove of an id the plugin list never held (it may still be in the game's own list), bumps it.
static void TestBanRevision() {
    uint64_t r0 = state::BanRevision();
    state::BanRecord r;
    r.gameId = "fedcba9876543210fedcba9876543210";
    r.expiresAt = "2030-01-01T00:00:00.000Z";
    CHECK(state::BanAddIfRevision(r, r0), "conditional add at the current revision");
    CHECK(state::BanRevision() == r0 + 1, "add bumps the revision");
    CHECK(!state::BanAddIfRevision(r, r0), "a stale revision is refused");
    auto snap = state::ReadBans();
    CHECK(snap.revision == r0 + 1 && snap.records.size() == 1, "snapshot carries records + revision");
    EQ(snap.records[0].expiresAt, "2030-01-01T00:00:00.000Z");
    CHECK(state::FlushBans(), "flush timed ban");
    std::string text;
    CHECK(ReadFile(state::BansPath(), text) && text.find("2030-01-01T00:00:00.000Z") != std::string::npos,
          "expiresAt persisted in bans.json");
    CHECK(!state::BanRemove("00000000000000000000000000000000"), "missing id is a miss");
    CHECK(state::BanRevision() == r0 + 2, "but still bumps the revision");
    CHECK(state::BanRemove("fedcba9876543210fedcba9876543210"), "remove");
    CHECK(state::FlushBans(), "flush");
    EQ(state::BanPersistenceError(), "");
    ::unlink(state::BansPath().c_str());
}

// The ring stores serialisers, not JSON: hooks capture owned values and readers render later.
static void TestDeferredEvents() {
    auto& st = PluginState::Get();
    uint64_t before = st.LatestSeq();
    int renders = 0;
    st.EmitEventDeferred("chat-message", [&renders] { ++renders; return std::string("{\"msg\":\"later\"}"); });
    CHECK(renders == 0, "emit must not serialise");
    JsonValue v;
    CHECK(JsonParse(st.EventsJson(before, 10), v), "events json invalid");
    CHECK(renders == 1, "rendered once by the reader");
    EQ(v.get("events")->arr[0].get("data")->get("msg")->str, "later");
}

static void TestPerfJson() {
    Perf::RecordTick(12000, 2, false);
    Perf::RecordFilter(500, true, false);
    Perf::RecordFilter(300, false, true);
    Perf::RecordEntry();
    { Perf::Scope sc("unit.sweep"); }
    JsonValue v;
    CHECK(JsonParse(Perf::Json(), v), "perf json invalid");
    CHECK(v.get("tick") && v.get("tick")->get("p99Us"), "tick p99");
    CHECK(v.get("processEventFilter") && v.get("processEventFilter")->get("avgNs")->num == 400, "filter avg ns");
    CHECK(v.get("gameThreadEntries")->get("count")->num == 1, "entries");
    CHECK(v.get("sweeps")->get("unit.sweep") != nullptr, "named sweep");
}

// The character name the server prints in its join line is the fallback for /players.
static void TestCharacterNameCache() {
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "");
    state::NoteCharacterName("0123456789ABCDEF0123456789ABCDEF", "takarotester");
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "takarotester");
    state::NoteCharacterName("0123456789abcdef0123456789abcdef", "");  // ignored
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "takarotester");
}


// The plugin's shutdown handler is registered with atexit before the first log flush, so every
// lazily built path is constructed after it and, if destructible, freed before it runs. The last
// flush then wrote through a freed string (a stray file named after a heap pointer, seen on a live
// server). Runs in a fresh process because the statics are already built in this one.
static int ExitFlushChild(const char* dir) {
    if (chdir(dir) != 0) return 2;
    setenv("TAKARO_PLUGIN_DATA_DIR", (std::string(dir) + "/data").c_str(), 1);
    std::atexit([] { PluginLog("late exit line"); FlushPluginLogs(); });
    PluginLog("early line");
    FlushPluginLogs();
    return 0;
}

static void TestLogFlushAtExit() {
    char dir[] = "/tmp/takaro-dragonwilds-exitflush-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr, "mkdtemp failed");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "unit_test", "--exit-flush-child", dir, (char*)nullptr);
        _exit(3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exit-flush child did not exit cleanly (status %d)", status);
    std::ifstream in(std::string(dir) + "/data/plugin.log");
    std::stringstream log;
    log << in.rdbuf();
    CHECK(log.str().find("late exit line") != std::string::npos, "the flush from an atexit handler was lost");
    std::vector<std::string> stray;
    if (DIR* d = opendir(dir)) {
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && strcmp(e->d_name, "data")) stray.push_back(e->d_name);
        closedir(d);
    }
    CHECK(stray.empty(), "the exit flush created a stray file in the working directory");
}

// takaro.cfg: which value wins, the file the connector creates, and the identity it writes back.
static void TestConfigResolve() {
    std::map<std::string, std::string> envVars;
    auto env = [&](const char* k) -> const char* {
        auto it = envVars.find(k);
        return it == envVars.end() ? nullptr : it->second.c_str();
    };
    auto load = [](const std::string& text) { return ConfigFile::FromText("/x/takaro.cfg", text); };
    const std::string fresh = ConfigFile::Template("11111111-2222-4333-8444-555555555555");

    // The created file: no token, the identity, a name of its own, the default URL.
    auto r = ConfigFile::Resolve(load(fresh), env);
    EQ(r.registration, "");
    EQ(r.identity, "11111111-2222-4333-8444-555555555555");
    EQ(r.serverName, "Dragonwilds (11111111)");
    EQ(r.url, ConfigFile::kDefaultUrl);
    CHECK(!r.envRegistration && !r.envIdentity && !r.envServerName && !r.envUrl, "nothing from env");
    // It says which keys reload: only the four above; any other key needs a restart.
    CHECK(fresh.find("Saving is enough for the four keys below") != std::string::npos, "reload scope");
    CHECK(fresh.find("those apply\n# after a restart") != std::string::npos, "restart note");
    // No name in the file: the unique name; no identity: nothing to connect as.
    r = ConfigFile::Resolve(load("TAKARO_IDENTITY_TOKEN= abcdefghij \nTAKARO_SERVER_NAME=\n"), env);
    EQ(r.identity, "abcdefghij");
    EQ(r.serverName, "Dragonwilds (abcdefgh)");
    r = ConfigFile::Resolve(load("TAKARO_REGISTRATION_TOKEN=t\n"), env);
    EQ(r.identity, "");
    EQ(r.serverName, "");

    // A non-empty environment variable wins per field; an empty one does not.
    envVars["TAKARO_REGISTRATION_TOKEN"] = " envtok ";
    envVars["TAKARO_IDENTITY_TOKEN"] = "";
    envVars["TAKARO_WS_URL"] = "wss://self.hosted/";
    r = ConfigFile::Resolve(load("TAKARO_REGISTRATION_TOKEN=filetok\nTAKARO_IDENTITY_TOKEN=fileid\n"), env);
    EQ(r.registration, "envtok");
    EQ(r.identity, "fileid");
    EQ(r.url, "wss://self.hosted/");
    CHECK(r.envRegistration && !r.envIdentity && r.envUrl, "env flags");
    envVars.clear();

    // Half-saved file: a line without '='.
    CHECK(ConfigFile::Unparseable(ConfigFile::Parse("TAKARO_REGISTRATION_TOK")), "half line");
    CHECK(!ConfigFile::Unparseable(ConfigFile::Parse(fresh)), "created file parses");
    CHECK(ConfigFile::Parse(fresh).warnings.empty(), "created file has no warnings");
    CHECK(!ConfigFile::Unparseable(ConfigFile::Parse("LD_PRELOAD=x\n")), "ignored key is not half-saved");
    auto parsed = ConfigFile::Parse("\xEF\xBB\xBF" "export DRAGONWILDS_LOG_FILE = \"/x y\"\r\nOTHER=1\n");
    CHECK(parsed.entries.size() == 1 && parsed.entries[0].second == "/x y" && parsed.warnings.size() == 1, "parse");

    // SetKey fills in the identity line and keeps everything else.
    const std::string noId = "# c\nTAKARO_REGISTRATION_TOKEN=t\nTAKARO_IDENTITY_TOKEN=\n";
    std::string set = ConfigFile::SetKey(noId, "TAKARO_IDENTITY_TOKEN", "abc");
    EQ(set, "# c\nTAKARO_REGISTRATION_TOKEN=t\nTAKARO_IDENTITY_TOKEN=abc\n");
    EQ(ConfigFile::SetKey("A=1\r\nTAKARO_IDENTITY_TOKEN = \r\nB=2\r\n", "TAKARO_IDENTITY_TOKEN", "x"),
       "A=1\r\nTAKARO_IDENTITY_TOKEN=x\r\nB=2\r\n");
    EQ(ConfigFile::SetKey("A=1", "TAKARO_IDENTITY_TOKEN", "x"), "A=1\nTAKARO_IDENTITY_TOKEN=x\n");
    EQ(ConfigFile::SetKey("", "TAKARO_IDENTITY_TOKEN", "x"), "TAKARO_IDENTITY_TOKEN=x\n");
    EQ(ConfigFile::SetKey("#TAKARO_IDENTITY_TOKEN=c\n", "TAKARO_IDENTITY_TOKEN", "x"),
       "#TAKARO_IDENTITY_TOKEN=c\nTAKARO_IDENTITY_TOKEN=x\n");

    // UUIDs: version 4, distinct.
    std::string a = ConfigFile::NewIdentity(), b = ConfigFile::NewIdentity();
    CHECK(a.size() == 36 && a[14] == '4' && a[8] == '-' && a != b, "uuid %s %s", a.c_str(), b.c_str());

    // WriteAtomic keeps the mode (and owner) of the file it replaces and leaves no temp file.
    char dir[] = "/tmp/takaro-dw-cfgw-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr, "mkdtemp");
    std::string path = std::string(dir) + "/takaro.cfg";
    CHECK(ConfigFile::WriteAtomic(path, "A=1\n", 0640), "write new");
    struct stat st{};
    stat(path.c_str(), &st);
    CHECK((st.st_mode & 0777) == 0640, "mode %o", st.st_mode & 0777);
    chmod(path.c_str(), 0604);
    CHECK(ConfigFile::WriteAtomic(path, "A=2\n", 0600), "replace");
    struct stat after{};
    stat(path.c_str(), &after);
    CHECK((after.st_mode & 0777) == 0604, "kept mode %o", after.st_mode & 0777);
    CHECK(after.st_uid == st.st_uid && after.st_gid == st.st_gid, "kept owner");
    EQ(*ConfigFile::ReadText(path), "A=2\n");
    DIR* d = opendir(dir);
    int entries = 0;
    while (dirent* e = d ? readdir(d) : nullptr) if (e->d_name[0] != '.') ++entries;
    if (d) closedir(d);
    CHECK(entries == 1, "no temp file left (%d entries)", entries);
    CHECK(!ConfigFile::ReadText(std::string(dir) + "/absent"), "absent is nullopt");
    unlink(path.c_str());

    // EnsureIdentity: a missing file is created (0600) with a new identity; an empty identity is
    // written back keeping the mode and every other line; anything else is left alone.
    auto gen = [] { return std::string("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"); };
    auto e = ConfigFile::EnsureIdentity(path, std::nullopt, gen);
    CHECK(e.what == ConfigFile::Ensured::Created, "created");
    EQ(*ConfigFile::ReadText(path), ConfigFile::Template("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"));
    EQ(*e.text, *ConfigFile::ReadText(path));
    stat(path.c_str(), &st);
    CHECK((st.st_mode & 0777) == 0600, "created mode %o", st.st_mode & 0777);
    e = ConfigFile::EnsureIdentity(path, ConfigFile::ReadText(path), gen);
    CHECK(e.what == ConfigFile::Ensured::Unchanged, "has an identity");
    CHECK(ConfigFile::WriteAtomic(path, noId), "user file");
    chmod(path.c_str(), 0640);
    e = ConfigFile::EnsureIdentity(path, noId, gen);
    CHECK(e.what == ConfigFile::Ensured::IdentityAdded, "identity added");
    EQ(*ConfigFile::ReadText(path), "# c\nTAKARO_REGISTRATION_TOKEN=t\nTAKARO_IDENTITY_TOKEN=aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\n");
    stat(path.c_str(), &st);
    CHECK((st.st_mode & 0777) == 0640 && st.st_uid == after.st_uid, "identity write kept mode %o", st.st_mode & 0777);
    e = ConfigFile::EnsureIdentity(path, std::string("TAKARO_REGISTRATION_TOK"), gen);
    CHECK(e.what == ConfigFile::Ensured::Unchanged, "half-saved file left alone");
    // Duplicate lines: the last one counts, so that is the one filled in.
    const std::string dup = "TAKARO_IDENTITY_TOKEN=old\nTAKARO_IDENTITY_TOKEN=\n";
    CHECK(ConfigFile::WriteAtomic(path, dup), "dup file");
    e = ConfigFile::EnsureIdentity(path, dup, gen);
    EQ(ConfigFile::Resolve(ConfigFile::FromText(path, *ConfigFile::ReadText(path)), env).identity,
       "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
    // Saved by someone else since it was read: left alone, the next read applies it.
    CHECK(ConfigFile::WriteAtomic(path, noId), "reset");
    e = ConfigFile::EnsureIdentity(path, std::string("TAKARO_IDENTITY_TOKEN=\n"), gen);
    CHECK(e.what == ConfigFile::Ensured::Unchanged && *ConfigFile::ReadText(path) == noId, "concurrent save kept");
    if (geteuid() != 0) {
        // There but unreadable: never replaced by a new file.
        chmod(path.c_str(), 0000);
        e = ConfigFile::EnsureIdentity(path, ConfigFile::ReadText(path), gen);
        chmod(path.c_str(), 0600);
        CHECK(e.what == ConfigFile::Ensured::NotWritable && *ConfigFile::ReadText(path) == noId, "unreadable kept");
    }
    unlink(path.c_str());
    if (geteuid() != 0) {
        chmod(dir, 0555);
        e = ConfigFile::EnsureIdentity(path, std::nullopt, gen);
        CHECK(e.what == ConfigFile::Ensured::NotWritable && !e.text, "read-only folder");
        chmod(dir, 0755);
    }
    rmdir(dir);
}

static void TestMapSurveyGrid() {
    MapSurvey::Grid g;
    CHECK(MapSurvey::PlanGrid(-1000, 1000, -500, 700, 100, 8192, g), "a normal box must plan");
    CHECK(g.w == 20 && g.h == 12, "grid is %dx%d, want 20x12", g.w, g.h);
    CHECK(g.minX == -1000 && g.minY == -500 && g.step == 100, "grid origin/step not kept");
    CHECK(MapSurvey::PlanGrid(0, 1050, 0, 10, 100, 8192, g) && g.w == 11 && g.h == 1,
          "a partial sample must round up (got %dx%d)", g.w, g.h);
    MapSurvey::Grid keep = g;
    CHECK(!MapSurvey::PlanGrid(0, 0, 0, 10, 100, 8192, g), "an empty box must be refused");
    CHECK(!MapSurvey::PlanGrid(10, 0, 0, 10, 100, 8192, g), "an inverted box must be refused");
    CHECK(!MapSurvey::PlanGrid(0, 10, 0, 10, 0, 8192, g), "a zero step must be refused");
    CHECK(!MapSurvey::PlanGrid(0, 10, 0, 10, -5, 8192, g), "a negative step must be refused");
    CHECK(!MapSurvey::PlanGrid(0, 1e9, 0, 10, 100, 8192, g), "more than maxSide samples must be refused");
    CHECK(!MapSurvey::PlanGrid(0, NAN, 0, 10, 100, 8192, g), "NaN bounds must be refused");
    CHECK(g.w == keep.w && g.h == keep.h, "a refused plan must leave the grid untouched");
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "--exit-flush-child") == 0) return ExitFlushChild(argv[2]);
    TestSymParser();
    TestJson();
    TestRedaction();
    TestRingBuffer();
    TestCapabilities();
    TestPluginBanList();
    TestBanRevision();
    TestDeferredEvents();
    TestPerfJson();
    TestCharacterNameCache();
    TestLogFlushAtExit();
    TestConfigResolve();
    TestMapSurveyGrid();
    printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "PASSED", g_ran, g_failed);
    return g_failed ? 1 : 0;
}
