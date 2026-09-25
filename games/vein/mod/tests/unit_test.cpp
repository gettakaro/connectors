// Host-side unit tests. No game process involved:
//   * the ELF parser and the .symtab/.dynsym walker, run against this very test binary
//   * the byte-signature matcher, including the "exactly once" rule the resolver enforces
//   * the depot .sym record/name-table format
//   * JSON, the event ring buffer, the plugin ban list and password/token redaction
#include "common.h"
#include "events_parse.h"
#include "resolve.h"
#include "state.h"
#include "actions_util.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// A uniquely named symbol this test looks for in its own .symtab.
extern "C" int takaro_test_marker_function(int x) { return x * 3; }
namespace takaro_test { struct Marker { int Method(int v) const; }; }
int takaro_test::Marker::Method(int v) const { return v + 1; }

static void TestElfParser() {
    std::string self;
    CHECK(ReadFile("/proc/self/exe", self), "cannot read /proc/self/exe");
    if (self.empty()) return;
    const uint8_t* p = (const uint8_t*)self.data();

    ElfInfo info;
    CHECK(ResolveCore::ParseElfBuffer(p, self.size(), info), "ELF parse failed: %s", info.error.c_str());
    CHECK(info.textAddr != 0 && info.textSize != 0, "no .text");
    CHECK(!info.execRanges.empty(), "no executable PT_LOAD");
    CHECK(info.hasSymtab, "the test binary must be built unstripped");
    CHECK(info.symtabCount > 0, "empty .symtab");

    // The walker finds the C symbol by its plain name...
    bool foundC = false;
    uint64_t marker = 0;
    size_t visited = ResolveCore::ForEachSymbol(p, self.size(), false, [&](const ResolveCore::SymbolRec& s2) {
        if (!strcmp(s2.name, "takaro_test_marker_function")) { foundC = true; marker = s2.value; }
    });
    CHECK(visited > 0, "walked no symbols");
    CHECK(foundC && marker != 0, "takaro_test_marker_function not found in .symtab");

    // ...and the mangled C++ one demangles to the signature the resolver matches against.
    bool foundCpp = false;
    ResolveCore::ForEachSymbol(p, self.size(), false, [&](const ResolveCore::SymbolRec& s2) {
        if (strstr(s2.name, "Marker") == nullptr) return;
        std::string d = ResolveCore::Demangle(s2.name);
        if (d == "takaro_test::Marker::Method(int) const") foundCpp = true;
    });
    CHECK(foundCpp, "the demangler did not produce 'takaro_test::Marker::Method(int) const'");

    // A garbage ELF must be rejected, not crash.
    ElfInfo bad;
    CHECK(!ResolveCore::ParseElfBuffer((const uint8_t*)"not an elf", 10, bad), "garbage accepted");
    CHECK(ResolveCore::ForEachSymbol((const uint8_t*)"not an elf", 10, false,
                                     [](const ResolveCore::SymbolRec&) {}) == 0,
          "garbage walked");

    // Name matching: a pinned signature is exact, an unpinned key matches on the base name only.
    CHECK(ResolveCore::NameMatches("UObject::ProcessEvent(UFunction*, void*)", "UObject::ProcessEvent",
                                   "UObject::ProcessEvent(UFunction*, void*)"),
          "pinned signature");
    CHECK(!ResolveCore::NameMatches("UObject::ProcessEvent(UFunction*, void*, int)", "UObject::ProcessEvent",
                                    "UObject::ProcessEvent(UFunction*, void*)"),
          "a different overload must not match a pinned signature");
    CHECK(ResolveCore::NameMatches("FName::ToString() const", "FName::ToString", nullptr), "base name");
    CHECK(!ResolveCore::NameMatches("FNameXX::ToString() const", "FName::ToString", nullptr), "wrong class");
    EQ(ResolveCore::BaseName("UObject::ProcessEvent"), "ProcessEvent");
    EQ(ResolveCore::BaseName("RequestEngineExit"), "RequestEngineExit");
    EQ(ResolveCore::Demangle("plain_c_name"), "");
}

static void TestSignatureMatcher() {
    ResolveCore::Signature sig;
    std::string err;

    CHECK(ResolveCore::ParseSignature("48 8B ?? E8", sig, err), "parse failed: %s", err.c_str());
    CHECK(sig.bytes.size() == 4, "length %zu", sig.bytes.size());
    CHECK(sig.bytes[0] == 0x48 && sig.bytes[1] == 0x8B && sig.bytes[2] == -1 && sig.bytes[3] == 0xE8, "bytes");
    // `?` and `??` mean the same thing, whitespace is free-form.
    ResolveCore::Signature sig2;
    CHECK(ResolveCore::ParseSignature("48\t8b ? e8", sig2, err), "lenient parse: %s", err.c_str());
    CHECK(sig2.bytes == sig.bytes, "`?` and `??` must be equivalent");
    // Malformed input is rejected with a reason, never silently accepted.
    CHECK(!ResolveCore::ParseSignature("", sig2, err), "empty pattern accepted");
    CHECK(!ResolveCore::ParseSignature("4", sig2, err), "half a byte accepted");
    CHECK(!ResolveCore::ParseSignature("ZZ", sig2, err), "non-hex accepted");
    CHECK(!ResolveCore::ParseSignature("?? 48", sig2, err), "leading wildcard accepted");

    const uint8_t hay[] = {0x00, 0x48, 0x8B, 0x01, 0xE8, 0x90, 0x48, 0x8B, 0x02, 0xE8, 0x90};
    size_t off = 0;
    // Two matches -> the resolver must refuse the signature.
    CHECK(ResolveCore::ScanSignature(hay, sizeof hay, sig, off, 2) == 2, "expected two matches");
    // A pattern that pins the wildcard byte is unique.
    ResolveCore::Signature uniq;
    CHECK(ResolveCore::ParseSignature("48 8B 02 E8", uniq, err), "parse");
    CHECK(ResolveCore::ScanSignature(hay, sizeof hay, uniq, off, 2) == 1, "expected one match");
    CHECK(off == 6, "offset %zu", off);
    // No match, and a pattern longer than the haystack.
    ResolveCore::Signature none;
    CHECK(ResolveCore::ParseSignature("DE AD BE EF", none, err), "parse");
    CHECK(ResolveCore::ScanSignature(hay, sizeof hay, none, off, 2) == 0, "false positive");
    ResolveCore::Signature big;
    CHECK(ResolveCore::ParseSignature("48 8B 01 E8 90 48 8B 02 E8 90 00 11 22 33", big, err), "parse");
    CHECK(ResolveCore::ScanSignature(hay, sizeof hay, big, off, 2) == 0, "overlong pattern matched");
}

static void TestDepotSymParser() {
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
    // Plugin/bearer tokens are redacted the same way as passwords.
    EQ(Redact("TAKARO_PLUGIN_TOKEN=abc123"), "TAKARO_PLUGIN_TOKEN=***");
    EQ(Redact("{\"token\":\"abc123\"}"), "{\"token\":\"***\"}");
    EQ(Redact("AuthToken: abc123"), "AuthToken: ***");
    EQ(Redact("ServerPassword=fixture-pw-42"), "ServerPassword=***");
    // A key that is only a prefix of a longer word is left alone.
    EQ(Redact("tokenConfigured true"), "tokenConfigured true");
    // key without a value must not eat the rest of the line
    EQ(Redact("WorldPassword"), "WorldPassword");
    EQ(Redact("WorldPassword="), "WorldPassword=");
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
    auto called = std::make_shared<bool>(false);
    const uint64_t before = st.LatestSeq();
    st.EmitEventDeferred("log", [called] {
        *called = true;
        // This read takes the ring lock: serialization must run outside it.
        return "{\"observedSeq\":" + std::to_string(PluginState::Get().LatestSeq()) + "}";
    });
    CHECK(!*called, "event hooks must not serialize JSON");
    CHECK(JsonParse(st.EventsJson(before, 1), v), "deferred snapshot JSON invalid");
    CHECK(*called, "background event reader should serialize the owned snapshot");
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
    setenv("TAKARO_PLUGIN_DATA_DIR", "/tmp/takaro-vein-test-bans", 1);
    ::mkdir("/tmp/takaro-vein-test-bans", 0755);
    ::unlink(state::BansPath().c_str());

    state::BansLoad();
    CHECK(!state::IsBanned("0123456789abcdef0123456789abcdef"), "nothing should be banned yet");

    state::BanRecord r;
    r.gameId = "0123456789ABCDEF0123456789ABCDEF";  // upper case on purpose
    r.name = "takarotester";
    r.reason = "L3b test";
    CHECK(state::BanAdd(r), "BanAdd should update enforcement memory");
    CHECK(access(state::BansPath().c_str(), F_OK) != 0, "game-thread update must not write the file");
    CHECK(mkdir(state::BansPath().c_str(), 0755) == 0, "inject an atomic rename failure");
    CHECK(!state::FlushBans(), "failed persistence must be explicit");
    CHECK(!state::BanPersistenceError().empty(), "persistence failure must remain visible");
    CHECK(state::IsBanned(r.gameId), "persistence failure must retain live enforcement");
    CHECK(rmdir(state::BansPath().c_str()) == 0, "remove injected failure");
    CHECK(state::FlushBans(), "background retry should persist current bans");
    CHECK(state::BanPersistenceError().empty(), "successful retry clears persistence error");
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

    const uint64_t observedRevision = state::BanRevision();
    state::BanRecord newer = list[0];
    newer.reason = "newer permanent ban";
    CHECK(state::BanAdd(newer), "newer ban must update the revision");
    state::BanRecord recovered = list[0];
    recovered.expiresAt = "2099-01-01T00:00:00Z";
    CHECK(!state::BanAddIfRevision(recovered, observedRevision), "stale recovery must not overwrite a newer ban");
    EQ(state::BanList()[0].reason, "newer permanent ban");
    CHECK(state::BanList()[0].expiresAt.empty(), "stale recovery must preserve permanent enforcement");
    CHECK(state::BanAddIfRevision(recovered, state::BanRevision()), "matching revision permits recovery metadata");
    EQ(state::BanList()[0].expiresAt, "2099-01-01T00:00:00Z");

    CHECK(state::BanRemove("0123456789abcdef0123456789abcdef"), "BanRemove should report a hit");
    const auto beforeMissingRemove = state::ReadBans();
    CHECK(beforeMissingRemove.records.empty(), "atomic snapshot pairs empty records with their revision");
    CHECK(!state::BanRemove("0123456789abcdef0123456789abcdef"), "a second remove is a miss");
    CHECK(state::BanRevision() > beforeMissingRemove.revision,
          "game-only unban attempt must invalidate previously verified recovery");
    CHECK(!state::BanAddIfRevision(recovered, beforeMissingRemove.revision),
          "recovery cannot recreate a plugin ban after a game-only unban attempt");
    CHECK(!state::IsBanned("0123456789abcdef0123456789abcdef"), "unbanned");
    CHECK(state::FlushBans(), "background worker persists the removal");
    CHECK(ReadFile(state::BansPath(), text) && JsonParse(text, v) && v.get("bans")->arr.empty(),
          "the removal should be persisted");
    ::unlink(state::BansPath().c_str());
}

// The character name the server prints in its join line is the fallback for /players.
static void TestCharacterNameCache() {
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "");
    state::NoteCharacterName("0123456789ABCDEF0123456789ABCDEF", "takarotester");
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "takarotester");
    state::NoteCharacterName("0123456789abcdef0123456789abcdef", "");  // ignored
    EQ(state::CharacterName("0123456789abcdef0123456789abcdef"), "takarotester");
}


// ---------------------------------------------------------------------------------------------
// lane L3: the pure half of the action handlers (src/actions_util.cpp)

using namespace ActionsUtil;

// VEIN is Steam-only: gameId is the bare 17-digit SteamID64.
static void TestGameIdNormalisation() {
    EQ(NormalizeGameId("76561198000000000"), "76561198000000000");
    EQ(NormalizeGameId("steam:76561198000000000"), "76561198000000000");
    EQ(NormalizeGameId("STEAM:76561198000000000"), "76561198000000000");
    // Anything that is not a clean 17-digit id stays a (lower-cased) name, so /players/{id} can
    // still match a player by name.
    EQ(NormalizeGameId("TakaroTester"), "takarotester");
    EQ(NormalizeGameId("7656119800000000"), "7656119800000000");   // 16 digits: not an id
    EQ(NormalizeGameId("765611980000000000"), "765611980000000000");  // 18 digits: not an id
    // A name that merely *contains* digits must not be mangled into an id.
    EQ(NormalizeGameId("Player76561198000000000x"), "player76561198000000000x");
    EQ(NormalizeGameId(""), "");
}

// The SteamID64 bit pattern is what /players scans APlayerState::UniqueID for, so it has to reject
// the kinds of words that actually sit next to a net id in memory.
static void TestSteamIdPattern() {
    CHECK(LooksLikeSteamId64(76561198000000000ull), "a real SteamID64 must match");
    CHECK(LooksLikeSteamId64(0x0110000100000001ull), "the lowest valid account id matches");
    CHECK(!LooksLikeSteamId64(0x0110000100000000ull), "account id 0 is not a player");
    CHECK(!LooksLikeSteamId64(0), "a null word never matches");
    CHECK(!LooksLikeSteamId64(0x00007f1234567890ull), "a heap pointer never matches");
    CHECK(!LooksLikeSteamId64(0x0110000200000005ull), "a different universe never matches");
    CHECK(!LooksLikeSteamId64(0xffffffffffffffffull), "poison never matches");
}

// Reflection artefacts must never reach an /items or /entities listing.
static void TestGeneratedArtefacts() {
    CHECK(IsGeneratedArtefact(""), "empty");
    CHECK(IsGeneratedArtefact("SKEL_BP_Item_Hammer_C"), "SKEL_");
    CHECK(IsGeneratedArtefact("REINST_BP_Item_Hammer_C"), "REINST_");
    CHECK(IsGeneratedArtefact("Default__BP_Item_Hammer_C"), "CDO");
    CHECK(IsGeneratedArtefact("TRASHCLASS_BP_Item_Hammer_C"), "TRASHCLASS_");
    CHECK(IsGeneratedArtefact("PLACEHOLDER-CLASS"), "PLACEHOLDER-");
    CHECK(!IsGeneratedArtefact("BP_Item_Hammer_C"), "a real item class must survive");
    CHECK(!IsGeneratedArtefact("VeinZombieCharacter"), "a real AI class must survive");
}

// POST /command parsing: the message must keep its own spacing.
static void TestCommandWords() {
    auto w = Words("  give   76561198000000000  BP_Item_Hammer_C 3 ");
    CHECK(w.size() == 4, "got %zu words", w.size());
    EQ(w[0], "give");
    EQ(w[3], "3");
    CHECK(Words("").empty(), "no words in an empty command");
    EQ(Rest("say hello   world", 1), "hello   world");
    EQ(Rest("whisper 76561198000000000 hi there", 2), "hi there");
    EQ(Rest("say", 1), "");
    EQ(Rest("say  ", 1), "");
}

// Item lookup: exact code, exact name, unique substring, and a refusal when it is ambiguous.
static void TestItemLookup() {
    std::vector<CatalogueEntry> cat = {
        {"BP_Item_Hammer_C", "Hammer"},
        {"BP_Item_HammerHead_C", "Hammer Head"},
        {"BP_Item_Bandage_C", "Bandage"},
    };
    std::string err;
    CHECK(LookupIndex(cat, "BP_Item_Hammer_C", err) == 0, "exact code");
    CHECK(LookupIndex(cat, "bp_item_hammer_c", err) == 0, "exact code is case-insensitive");
    CHECK(LookupIndex(cat, "Hammer", err) == 0, "an exact *name* beats a substring of another code");
    CHECK(LookupIndex(cat, "Bandage", err) == 2, "exact name");
    CHECK(LookupIndex(cat, "bandag", err) == 2, "a unique substring resolves");

    err.clear();
    CHECK(LookupIndex(cat, "HammerH", err) == 1, "a unique substring of one code resolves");
    err.clear();
    CHECK(LookupIndex(cat, "Item_Hammer", err) == -1, "an ambiguous substring must be refused");
    CHECK(err.find("ambiguous") != std::string::npos, "got '%s'", err.c_str());
    CHECK(err.find("2 matches") != std::string::npos, "the count is reported: '%s'", err.c_str());
    err.clear();
    CHECK(LookupIndex(cat, "Sword", err) == -1, "an unknown code must be refused");
    CHECK(err.find("unknown item code") != std::string::npos, "got '%s'", err.c_str());
    err.clear();
    CHECK(LookupIndex({}, "anything", err) == -1, "an empty catalogue resolves nothing");
}

// GET /items?search= filters on code or name, case-insensitively; no filter keeps everything.
static void TestItemSearchFilter() {
    CatalogueEntry e{"BP_Item_Hammer_C", "Hammer"};
    CHECK(MatchesSearch(e, ""), "an empty search matches everything");
    CHECK(MatchesSearch(e, "hammer"), "matches the name");
    CHECK(MatchesSearch(e, "bp_item"), "matches the code");
    CHECK(!MatchesSearch(e, "bandage"), "a miss is a miss");
    CHECK(!MatchesSearch(e, "HAMMER"), "the needle is expected pre-lowered by the caller");
}


// ---------------------------------------------------------------------------------------------
// lane L2: the VEIN log grammar (context/games/vein/research/2026-09-17-log-grammar.md).
// Every line below is one the game really wrote, or a deliberate malformed variant of it.

static void TestLogLineSplit() {
    auto l = EventsParse::SplitLogLine(
        "[2026.09.17-05.58.10:857][ 27]LogVeinChat: [76561198765432109] Limon (aka Takaro Tester): hi");
    CHECK(l.hasPrefix, "the standard UE prefix must be recognised");
    EQ(l.timestamp, "2026.09.17-05.58.10:857");
    EQ(l.frame, "27");
    EQ(l.category, "LogVeinChat");
    EQ(l.message, "[76561198765432109] Limon (aka Takaro Tester): hi");

    // The first lines of a log file are written before LogTimes initialises and carry no prefix.
    auto n = EventsParse::SplitLogLine("LogInit: Display: Running engine for game: Vein");
    CHECK(!n.hasPrefix, "a prefix-less line must still parse");
    EQ(n.category, "LogInit");
    EQ(n.message, "Display: Running engine for game: Vein");

    // A line that is not "Category: message" at all keeps its whole text.
    auto raw = EventsParse::SplitLogLine("just some text without a category");
    EQ(raw.category, "");
    EQ(raw.message, "just some text without a category");

    // A trailing CR from a Windows-written line must not leak into the message.
    auto cr = EventsParse::SplitLogLine("LogVein: hello\r");
    EQ(cr.message, "hello");
}

static void TestSteamIdShape() {
    CHECK(EventsParse::LooksLikeSteamId64("76561198765432109"), "Tester's id is a SteamID64");
    CHECK(!EventsParse::LooksLikeSteamId64("7656119800073587"), "16 digits is not a SteamID64");
    CHECK(!EventsParse::LooksLikeSteamId64("765611987654321090"), "18 digits is not a SteamID64");
    CHECK(!EventsParse::LooksLikeSteamId64("16561198000735875"), "the 7656119 prefix is required");
    CHECK(!EventsParse::LooksLikeSteamId64("7656119800073587a"), "a non-digit is not a SteamID64");
    CHECK(!EventsParse::LooksLikeSteamId64(""), "an empty string is not a SteamID64");
}

static void TestChatLineParser() {
    auto c = EventsParse::ParseChatLine(
        "[2026.09.17-05.58.10:857][ 27]LogVeinChat: [76561198765432109] Limon (aka Takaro Tester): "
        "takaro-l0b-sandbox-075811");
    CHECK(c.ok, "the observed chat line must parse");
    EQ(c.gameId, "76561198765432109");
    EQ(c.platformName, "Limon");
    EQ(c.characterName, "Takaro Tester");
    EQ(c.msg, "takaro-l0b-sandbox-075811");

    // A player who has not selected a character yet has no "(aka ...)" part.
    auto noAka = EventsParse::ParseChatLine("LogVeinChat: [76561198765432109] Limon: hello there");
    CHECK(noAka.ok, "the no-character form must parse");
    EQ(noAka.platformName, "Limon");
    EQ(noAka.characterName, "");
    EQ(noAka.msg, "hello there");

    // A message full of colons must not be truncated: the split is on the FIRST ": " after the name.
    auto colons = EventsParse::ParseChatLine(
        "LogVeinChat: [76561198765432109] Limon (aka Takaro Tester): 12:30: meet me at: the farm");
    CHECK(colons.ok, "a colon-heavy message must parse");
    EQ(colons.msg, "12:30: meet me at: the farm");

    // A leading '/' is broadcast verbatim by this build - it is chat, not a command.
    auto slash = EventsParse::ParseChatLine("LogVeinChat: [76561198765432109] Limon: /help");
    CHECK(slash.ok && slash.msg == "/help", "a slash message is still chat on this build");

    // Rejections: wrong category, no id, a non-Steam id.
    CHECK(!EventsParse::ParseChatLine("LogVein: [76561198765432109] Limon: hi").ok, "wrong category");
    CHECK(!EventsParse::ParseChatLine("LogVeinChat: Limon: hi").ok, "no bracketed id");
    CHECK(!EventsParse::ParseChatLine("LogVeinChat: [nope] Limon: hi").ok, "not a SteamID64");
    CHECK(!EventsParse::ParseChatLine("").ok, "an empty line is not chat");
}

static void TestCharacterSelectParser() {
    auto s = EventsParse::ParseCharacterSelectLine(
        "[2026.09.17-05.55.15:350][925]LogVein: [] Player Limon selected character "
        "5463D6DD44DCEC89488FB7AB71820255 (aka Takaro Tester)");
    CHECK(s.ok, "the observed character-selection line must parse");
    EQ(s.platformName, "Limon");
    EQ(s.characterId, "5463D6DD44DCEC89488FB7AB71820255");
    EQ(s.characterName, "Takaro Tester");
    CHECK(!EventsParse::ParseCharacterSelectLine("LogVein: [] Player Limon selected character short (aka X)").ok,
          "a non-32-hex character id must be rejected");
}

// The server-side join lines are the log-only fallback for player-connected. They are parsed from
// the RAW line, before redaction, which is why the password and ticket handling below matters.
static void TestJoinLineParser() {
    auto a = EventsParse::ParseJoinLine(
        "[2026.09.17-07.10.00:001][123]LogNet: Login request: ?Password=fixture-pw-42?Name=Limon??ID=76561198765432109"
        "?Ticket=AAAABBBB userId: NULL:gamer-0123456789abcdef0123456789abcdef platform: NULL");
    CHECK(a.ok, "the observed login line must parse");
    EQ(a.gameId, "76561198765432109");
    EQ(a.name, "Limon");

    auto b = EventsParse::ParseJoinLine(
        "LogNet: Join request: /Game/Vein/Maps/ChamplainValley?Password=fixture-pw-42?Name=Limon"
        "??ID=76561198765432109?Ticket=X?SplitscreenCount=1");
    CHECK(b.ok && b.gameId == "76561198765432109" && b.name == "Limon", "the join line must parse too");

    auto c = EventsParse::ParseJoinLine(
        "LogVein: Player 76561198765432109 (gamer-0123456789abcdef0123456789abcdef) authenticated successfully.");
    CHECK(c.ok, "the authentication line must parse");
    EQ(c.gameId, "76561198765432109");
    EQ(c.name, "");  // this form carries no display name

    CHECK(!EventsParse::ParseJoinLine("LogNet: Login request: ?Password=x?Name=y").ok,
          "a login line without an ID is not a join");
    CHECK(!EventsParse::ParseJoinLine("LogVein: Player 123 authenticated successfully.").ok,
          "a non-SteamID64 must be rejected");
    CHECK(!EventsParse::ParseJoinLine("LogNet: NotifyAcceptedConnection: UniqueId: INVALID").ok,
          "an accepted connection is not yet a join");
}

static void TestPlayerStateIdParser() {
    auto a = EventsParse::ParsePlayerStateIdLine(
        "[2026.09.17-05.51.58:240][207]LogVein: PlayerState ID changed to 76561198765432109");
    CHECK(a.ok, "the id-assignment line must parse");
    EQ(a.gameId, "76561198765432109");
    // The game emits an empty variant first; treating that as an id would invent a player.
    CHECK(!EventsParse::ParsePlayerStateIdLine("LogVein: PlayerState ID changed to ").ok,
          "the empty variant must be rejected");
}

// The travel URL carries a Steam auth session ticket and the join URL the world password. Neither
// may ever reach Takaro, so redaction is tested on the real line shapes.
static void TestLogRedaction() {
    std::string t = EventsParse::RedactLogLine(
        "LogNet: Browse: /Game/Vein/Maps/TheFarm??ID=76561198765432109?Ticket=AAAABBBBCCCCDDDD== next");
    CHECK(t.find("AAAABBBBCCCCDDDD") == std::string::npos, "the Steam auth ticket must be gone: %s", t.c_str());
    CHECK(t.find("76561198765432109") != std::string::npos, "the SteamID64 is not a secret and must stay");
    CHECK(t.find("<redacted>") != std::string::npos, "the removal must be visible");
    CHECK(t.find(" next") != std::string::npos, "only the ticket value is removed: %s", t.c_str());

    std::string p = EventsParse::RedactLogLine("LogNet: Login request: ?p=c2VjcmV0 userId=x");
    CHECK(p.find("c2VjcmV0") == std::string::npos, "the base64 world password must be gone: %s", p.c_str());

    std::string ini = EventsParse::RedactLogLine("Password=fixture-pw-42");
    CHECK(ini.find("fixture-pw-42") == std::string::npos, "common.cpp's Password= redaction still applies: %s", ini.c_str());

    // The real join line: the password and the ticket must go, the SteamID64 and the display name
    // must survive - Takaro needs both, and common.cpp's Redact() alone would swallow them because
    // it does not treat '?' as a value terminator.
    std::string join = EventsParse::RedactLogLine(
        "LogNet: Login request: ?Password=fixture-pw-42?Name=Limon??ID=76561198765432109?Ticket=AAAABBBB userId: NULL");
    CHECK(join.find("fixture-pw-42") == std::string::npos, "the join password must be gone: %s", join.c_str());
    CHECK(join.find("AAAABBBB") == std::string::npos, "the auth ticket must be gone: %s", join.c_str());
    CHECK(join.find("Limon") != std::string::npos, "the display name must survive: %s", join.c_str());
    CHECK(join.find("76561198765432109") != std::string::npos, "the SteamID64 must survive: %s", join.c_str());

    std::string clean = EventsParse::RedactLogLine("LogVein: nothing secret here");
    EQ(clean, "LogVein: nothing secret here");
}

static void TestLogNoiseFilter() {
    CHECK(EventsParse::IsNoise(""), "an empty line is noise");
    CHECK(EventsParse::IsNoise("[S_API FAIL] SteamAPI_Init not called"), "the Steam API spam is noise");
    CHECK(EventsParse::IsNoise("LogNetTraffic: Verbose: sending 42 bytes"), "net traffic is noise");
    CHECK(!EventsParse::IsNoise("LogVeinChat: [76561198765432109] Limon: hi"), "chat is never noise");
    CHECK(!EventsParse::IsNoise("LogVein: Created session GameSession."), "the ready marker is never noise");
}

// ---------------------------------------------------------------------------------------------
// lane L3b: TAKARO_ADMIN_STEAMIDS / TAKARO_SUPERADMIN_STEAMIDS parsing.
//
// This list is handed straight to AVeinGameSession::SetAdmin on the live game session, so a token
// that is not a real SteamID64 must be dropped here rather than written into the game's array.

static void TestAdminIdListParsing() {
    std::vector<std::string> bad;
    // the ordinary case: one id, exactly as the rig's .env carries it
    auto one = ParseSteamIdList("76561198765432109", &bad);
    CHECK(one.size() == 1, "one id parses to one entry (got %zu)", one.size());
    EQ(one[0], "76561198765432109");
    CHECK(bad.empty(), "a clean id rejects nothing (got %zu)", bad.size());

    // separators: comma, semicolon, whitespace and newlines all work, in any mixture
    auto many = ParseSteamIdList("76561198765432109, 76561198000000001;76561198000000002\n76561198000000003");
    CHECK(many.size() == 4, "comma/semicolon/space/newline all separate (got %zu)", many.size());
    EQ(many[0], "76561198765432109");
    EQ(many[3], "76561198000000003");

    // decoration is stripped: the steam: platform prefix, quotes and padding
    auto deco = ParseSteamIdList("  \"steam:76561198765432109\" , 'STEAM:76561198000000001'  ");
    CHECK(deco.size() == 2, "steam: prefix and quotes are stripped (got %zu)", deco.size());
    EQ(deco[0], "76561198765432109");
    EQ(deco[1], "76561198000000001");

    // order is first-seen and duplicates collapse
    auto dup = ParseSteamIdList("76561198000000002,76561198000000001,76561198000000002,steam:76561198000000001");
    CHECK(dup.size() == 2, "duplicates collapse (got %zu)", dup.size());
    EQ(dup[0], "76561198000000002");
    EQ(dup[1], "76561198000000001");

    // everything that is not a SteamID64 is rejected, and reported so /health can show it
    bad.clear();
    auto mixed = ParseSteamIdList("Limon, 7656119800073587, 765611987654321095, 0, steam:, 76561198765432109", &bad);
    CHECK(mixed.size() == 1, "only the real id survives (got %zu)", mixed.size());
    EQ(mixed[0], "76561198765432109");
    CHECK(bad.size() == 5, "every rejected token is reported (got %zu)", bad.size());
    EQ(bad[0], "Limon");
    EQ(bad[1], "7656119800073587");

    // an account id of 0 has the right prefix but is nobody
    bad.clear();
    auto zero = ParseSteamIdList("76561197960265728", &bad);
    CHECK(zero.empty(), "SteamID64 with account id 0 is refused (got %zu)", zero.size());
    CHECK(bad.size() == 1, "and is reported as rejected");

    // an id outside the individual/public universe pattern is not a player id
    CHECK(ParseSteamIdList("11111111111111111").empty(), "a 17-digit number that is not a SteamID64 is refused");

    // empty and separator-only input is empty, not an error
    bad.clear();
    CHECK(ParseSteamIdList("", &bad).empty(), "empty input yields no ids");
    CHECK(ParseSteamIdList("  ,, ; \n ", &bad).empty(), "separator-only input yields no ids");
    CHECK(bad.empty(), "separators are not rejected tokens (got %zu)", bad.size());

    // the rejected-list argument is optional
    CHECK(ParseSteamIdList("nonsense").empty(), "a null `rejected` argument is allowed");
}


// ---------------------------------------------------------------------------------------------
// lane L3d: broadcast path selection. NetMulticast_SendChat SIGSEGVs on a null sender, so the one
// invariant these tests exist to defend is: kChatWithSender is NEVER returned without haveSender.

static void TestBroadcastPathChoice() {
    using ActionsUtil::BroadcastCaps;
    using ActionsUtil::BroadcastPath;
    using ActionsUtil::ChooseBroadcastPath;
    std::string err;

    // everything available, no preference -> the sender-less multicast wins
    BroadcastCaps all;
    all.gameStateServerMsg = all.adminServerMsg = all.sendChat = all.haveSender = true;
    CHECK(ChooseBroadcastPath("", all, err) == BroadcastPath::kGameStateServerMsg, "auto picks the game state");
    CHECK(ChooseBroadcastPath("auto", all, err) == BroadcastPath::kGameStateServerMsg, "'auto' is the default");
    CHECK(ChooseBroadcastPath("AUTO", all, err) == BroadcastPath::kGameStateServerMsg, "case insensitive");
    CHECK(ChooseBroadcastPath("  auto  ", all, err) == BroadcastPath::kGameStateServerMsg, "trimmed");

    // explicit preferences
    CHECK(ChooseBroadcastPath("chat", all, err) == BroadcastPath::kChatWithSender, "chat when a sender exists");
    CHECK(ChooseBroadcastPath("admin", all, err) == BroadcastPath::kAdminServerMsg, "admin pins the admin RPC");
    CHECK(ChooseBroadcastPath("servermessage", all, err) == BroadcastPath::kGameStateServerMsg, "servermessage pins");

    // THE GUARD: chat asked for, but nobody can be the sender
    BroadcastCaps noSender = all;
    noSender.haveSender = false;
    CHECK(ChooseBroadcastPath("chat", noSender, err) != BroadcastPath::kChatWithSender,
          "chat must never be chosen without a sender");
    CHECK(ChooseBroadcastPath("chat", noSender, err) == BroadcastPath::kGameStateServerMsg,
          "it falls back to the safe path instead");

    // only chat resolved, and no sender -> nothing is safe to call
    BroadcastCaps chatOnly;
    chatOnly.sendChat = true;
    err.clear();
    CHECK(ChooseBroadcastPath("", chatOnly, err) == BroadcastPath::kNone, "chat alone with no sender is unusable");
    CHECK(err.find("no sender available") != std::string::npos, "and it says why: '%s'", err.c_str());
    err.clear();
    CHECK(ChooseBroadcastPath("chat", chatOnly, err) == BroadcastPath::kNone, "even when chat was asked for");
    CHECK(err.find("no sender available") != std::string::npos, "still says why");

    // only chat resolved, with a sender -> chat is now allowed
    BroadcastCaps chatSender = chatOnly;
    chatSender.haveSender = true;
    CHECK(ChooseBroadcastPath("", chatSender, err) == BroadcastPath::kChatWithSender, "last-resort chat");

    // nothing at all
    BroadcastCaps none;
    err.clear();
    CHECK(ChooseBroadcastPath("", none, err) == BroadcastPath::kNone, "nothing resolved");
    CHECK(!err.empty(), "an empty capability set must explain itself");

    // a pinned path that did not resolve degrades to whatever is safe
    BroadcastCaps adminOnly;
    adminOnly.adminServerMsg = true;
    CHECK(ChooseBroadcastPath("servermessage", adminOnly, err) == BroadcastPath::kAdminServerMsg,
          "an unavailable pin falls forward");
    CHECK(ChooseBroadcastPath("nonsense", adminOnly, err) == BroadcastPath::kAdminServerMsg,
          "an unknown preference is just 'auto'");

    // the reported symbol names
    EQ(std::string(ActionsUtil::BroadcastPathSymbol(BroadcastPath::kGameStateServerMsg)),
       std::string("AVeinGameStateBase::NetMulticast_BroadcastServerMessage"));
    EQ(std::string(ActionsUtil::BroadcastPathSymbol(BroadcastPath::kAdminServerMsg)),
       std::string("UAdminComponent::Server_SendServerMessage"));
    EQ(std::string(ActionsUtil::BroadcastPathSymbol(BroadcastPath::kChatWithSender)),
       std::string("AVeinGameStateBase::NetMulticast_SendChat"));
    EQ(std::string(ActionsUtil::BroadcastPathSymbol(BroadcastPath::kNone)), std::string(""));
}

// ---- lane L3e: chat channel + the verification predicates --------------------------------------

static void TestChatChannelMapping() {
    // DWARF: enum class EChatSegment : unsigned char { All=0, Local=1, Global=2, Radio=3 }.
    EQ(std::string(EventsParse::ChatChannelName(0)), std::string("all"));
    EQ(std::string(EventsParse::ChatChannelName(1)), std::string("local"));
    EQ(std::string(EventsParse::ChatChannelName(2)), std::string("global"));
    EQ(std::string(EventsParse::ChatChannelName(3)), std::string("radio"));
    // L6b finding 3: Local used to come back as "global", so onlyGlobalChat relayed local chat.
    CHECK(std::string(EventsParse::ChatChannelName(1)) != "global", "Local must not report as global");
    // Unknown, and "the call carried no segment at all" (-1), stay on the old default.
    EQ(std::string(EventsParse::ChatChannelName(-1)), std::string("global"));
    EQ(std::string(EventsParse::ChatChannelName(9)), std::string("global"));
    EQ(std::string(EventsParse::ChatSegmentName(1)), std::string("Local"));
    EQ(std::string(EventsParse::ChatSegmentName(2)), std::string("Global"));
    CHECK(EventsParse::ChatSegmentName(7) == nullptr, "an unknown segment has no name");
}

static void TestTeleportVerification() {
    const double origin[3] = {0, 0, 0};
    const double target[3] = {1000, 2000, 300};
    // The exact L6b failure: the call ran, the pawn never moved, and it was reported as success.
    CHECK(!ActionsUtil::TeleportArrived(origin, origin, target, 500.0), "no movement is never a teleport");
    // Landed on the target.
    CHECK(ActionsUtil::TeleportArrived(origin, target, target, 500.0), "landing on the target verifies");
    // Landed near it: the engine snaps the capsule to the ground and out of geometry.
    const double near_[3] = {1000, 2000, 120};
    CHECK(ActionsUtil::TeleportArrived(origin, near_, target, 500.0), "within tolerance verifies");
    // Moved, but nowhere near the target: walking is not teleporting.
    const double walked[3] = {50, 0, 0};
    CHECK(!ActionsUtil::TeleportArrived(origin, walked, target, 500.0), "moving the wrong way is not arrival");
    // Just outside the tolerance.
    const double far_[3] = {1000, 2000, -400};
    CHECK(!ActionsUtil::TeleportArrived(origin, far_, target, 500.0), "700 cm off is outside a 500 cm tolerance");
    CHECK(ActionsUtil::Distance3(origin, origin) == 0.0, "distance to self is zero");
    CHECK(ActionsUtil::Distance3(origin, target) > 2200.0, "3-4-5 style distance");
}

static void TestGiveVerification() {
    CHECK(!ActionsUtil::GiveArrived(7, 7), "an unchanged inventory is the L6b silent no-op, not a success");
    CHECK(ActionsUtil::GiveArrived(7, 8), "one more unit verifies");
    CHECK(ActionsUtil::GiveArrived(0, 3), "three more units verify");
    CHECK(!ActionsUtil::GiveArrived(3, 1), "losing items is never a successful give");
}

// ---- lane L3f / finding F19: never answer from a stale or foreign container -------------------

// ---- lane L2c: the entity-killed `weapon` field --------------------------------------------------
// The bug this pins down (Takaro 2026-09-17): every entity-killed carried the class name of the
// death event's DamageCauser, so a wolf that a zombie ate arrived as `weapon: "BP_Zombie_C"` and a
// real melee kill as `weapon: "MeleeEquippedItem"`. Neither is a weapon.
static void TestKillWeaponName() {
    using ActionsUtil::KillWeaponName;
    // The real case: the causer is the swung AMeleeEquippedItem and its UItem resolved to a name.
    EQ(KillWeaponName("MeleeEquippedItem", "Baseball Bat"), std::string("Baseball Bat"));
    EQ(KillWeaponName("BP_Rifle_Equipped_C", "Hunting Rifle"), std::string("Hunting Rifle"));
    // A weapon-ish actor whose item could not be resolved still says what hit, humanised.
    EQ(KillWeaponName("MeleeEquippedItem", ""), std::string("Melee Equipped"));  // HumaniseCode drops the "Item" suffix
    EQ(KillWeaponName("BP_Bullet_C", ""), std::string("Bullet"));
    EQ(KillWeaponName("BP_ThrowableRock_C", ""), std::string("Throwable Rock"));
    // A PAWN is never a weapon - these are the two shapes that produced the wrong records.
    EQ(KillWeaponName("BP_Zombie_C", ""), std::string(""));          // a zombie's bite
    EQ(KillWeaponName("BP_Wolf_C", ""), std::string(""));            // a wolf's bite
    EQ(KillWeaponName("BP_VeinPlayerCharacter_C", ""), std::string(""));  // POST /debug/kill-nearest
    EQ(KillWeaponName("AVeinZombieCharacter", ""), std::string(""));
    // ...not even when something tried to hand it an item name for a pawn: a bite is not a weapon.
    EQ(KillWeaponName("BP_Zombie_C", "Baseball Bat"), std::string(""));
    // Nothing at all -> nothing reported; the caller omits the field.
    EQ(KillWeaponName("", ""), std::string(""));
    EQ(KillWeaponName("BP_Door_C", ""), std::string(""));
    EQ(KillWeaponName("APainCausingVolume", ""), std::string(""));
}

static std::string SplitStr(const std::vector<int>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) o += (i ? "," : "") + std::to_string(v[i]);
    return o;
}

static void TestStackSplit() {
    using ActionsUtil::StackSplit;
    auto EQSPLIT = [](const std::vector<int>& got, const char* want) { EQ(SplitStr(got), std::string(want)); };
    // A stackable item: whole stacks, then the remainder.
    EQSPLIT(StackSplit(1, 50), "1");
    EQSPLIT(StackSplit(50, 50), "50");
    EQSPLIT(StackSplit(120, 50), "50,50,20");
    // THE L3f defect: a NON-stackable item (maxStack 1, e.g. BP_Corn_C whose bStackable is false).
    // The old split built ONE instance with Stack=3; the game gave one corn and the reader read
    // three. One instance per unit is the only correct answer.
    EQSPLIT(StackSplit(3, 1), "1,1,1");
    EQSPLIT(StackSplit(1, 1), "1");
    // A missing/implausible limit is treated as 1 rather than as "put it all in one instance".
    EQSPLIT(StackSplit(3, 0), "1,1,1");
    EQSPLIT(StackSplit(2, -5), "1,1");
    // Nothing to give.
    CHECK(StackSplit(0, 10).empty(), "amount 0 adds nothing");
    CHECK(StackSplit(-1, 10).empty(), "a negative amount adds nothing");
    // Whatever the split, it must hand out exactly `amount` units, none of them over the limit.
    bool conserves = true, withinLimit = true;
    for (int amount = 1; amount <= 40; amount++)
        for (int max_ = 1; max_ <= 7; max_++) {
            int sum = 0;
            for (int n : StackSplit(amount, max_)) {
                withinLimit = withinLimit && n >= 1 && n <= max_;
                sum += n;
            }
            conserves = conserves && sum == amount;
        }
    CHECK(conserves, "the split hands out exactly `amount` units, for every amount 1..40 x limit 1..7");
    CHECK(withinLimit, "no instance ever exceeds the item's own stack limit");
}

static void TestPlayerInventoryClassGuard() {
    using ActionsUtil::IsPlayerInventoryClass;
    // The live character's own bag, in every spelling the game uses for it.
    CHECK(IsPlayerInventoryClass("BaseInventoryComponent"), "the character's own component is accepted");
    CHECK(IsPlayerInventoryClass("UBaseInventoryComponent"), "the C++ spelling is accepted");
    CHECK(IsPlayerInventoryClass("PlayerInventoryComponent"), "a player inventory subclass is accepted");
    CHECK(IsPlayerInventoryClass("BP_VeinInventoryComponent_C"), "a blueprint subclass is accepted");
    // The container that produced the reported defect: after a ~300 m fall death VEIN keeps the
    // body's loot in a UPersistentCorpseInventory, which IS a UBaseInventoryComponent, so an IsA
    // test alone let a corpse answer for the player ("Corn 2" for a character holding no corn).
    CHECK(!IsPlayerInventoryClass("UPersistentCorpseInventory"), "a corpse's loot is never the player's");
    CHECK(!IsPlayerInventoryClass("PersistentCorpseInventory"), "same, without the U");
    CHECK(!IsPlayerInventoryClass("OfflineCharacterCacheInventory"), "the controller's character cache is not the player's");
    CHECK(!IsPlayerInventoryClass("ContainerInventoryComponent"), "a world container is not the player's");
    CHECK(!IsPlayerInventoryClass("VehicleInventoryComponent"), "a vehicle's boot is not the player's");
    CHECK(!IsPlayerInventoryClass("StorageInventoryComponent"), "a storage box is not the player's");
    // Anything that is not an inventory at all.
    CHECK(!IsPlayerInventoryClass("ConditionComponent"), "a non-inventory component is rejected");
    CHECK(!IsPlayerInventoryClass(""), "an unreadable class name is rejected");
}

static void TestCharacterIdFormatting() {
    using ActionsUtil::GuidDigits;
    // The live session the defect was found in: :8080/status reported this character id, and the
    // plugin has to print the same 32 upper-case hex digits from AVeinPlayerState::LoadedCharacterID.
    EQ(GuidDigits(0x6B09E1BCu, 0x89964DE6u, 0x83292C0Cu, 0xB420BB23u),
       std::string("6B09E1BC89964DE683292C0CB420BB23"));
    // Leading zeros must survive: a %X without the width would shorten the id.
    EQ(GuidDigits(1, 2, 3, 4), std::string("00000001000000020000000300000004"));
    EQ(GuidDigits(0xFFFFFFFFu, 0, 0, 0), std::string("FFFFFFFF000000000000000000000000"));
    // No character loaded yet: an all-zero GUID is "none", not a string of zeros.
    EQ(GuidDigits(0, 0, 0, 0), std::string(""));
}

static void TestAttemptedJson() {
    EQ(ActionsUtil::JsonStrArray({}), std::string("[]"));
    EQ(ActionsUtil::JsonStrArray({"AGameSession::KickPlayer"}), std::string("[\"AGameSession::KickPlayer\"]"));
    EQ(ActionsUtil::JsonStrArray({"a", "b"}), std::string("[\"a\",\"b\"]"));
    // The via strings are pasted into JSON, so quoting has to survive them.
    EQ(ActionsUtil::JsonStrArray({"a\"b"}), std::string("[\"a\\\"b\"]"));
}

static void TestMessagePrefixing() {
    using ActionsUtil::RenderMessage;
    EQ(RenderMessage("Server", "hello"), std::string("[Server] hello"));
    EQ(RenderMessage("", "hello"), std::string("hello"));
    EQ(RenderMessage("   ", "hello"), std::string("hello"));
    EQ(RenderMessage("  Takaro  ", "hi there"), std::string("[Takaro] hi there"));
    EQ(RenderMessage("Server", ""), std::string("[Server] "));
    EQ(RenderMessage("[Server]", "x"), std::string("[[Server]] x"));  // no unwrapping: the name is literal
    EQ(RenderMessage("Sérver", "ü"), std::string("[Sérver] ü"));      // UTF-8 passes through untouched
}


// ---- lane L3c: readable item names ---------------------------------------------------------

static void TestHumaniseCode() {
    using ActionsUtil::HumaniseCode;
    // The class-name conventions actually present in the 1418-entry VEIN catalogue.
    EQ(HumaniseCode("BloodPressureCuffItem"), "Blood Pressure Cuff");
    EQ(HumaniseCode("BowItem"), "Bow");
    EQ(HumaniseCode("BatteryPoweredItem"), "Battery Powered");
    EQ(HumaniseCode("BP_NeedleThread_C"), "Needle Thread");
    EQ(HumaniseCode("BP_Bandage_Makeshift_C"), "Bandage Makeshift");
    EQ(HumaniseCode("BP_CL_Shoes_Sneakers_C"), "CL Shoes Sneakers");
    EQ(HumaniseCode("BP_USD_C"), "USD");
    EQ(HumaniseCode("BP_Zombie_C"), "Zombie");
    EQ(HumaniseCode("BP_DeerMale_C"), "Deer Male");
    // A run of capitals stays one word until a lower-case letter claims the last of them.
    EQ(HumaniseCode("USDBottleItem"), "USD Bottle");
    // Never destroys the only word, and never returns empty.
    EQ(HumaniseCode("Item"), "Item");
    EQ(HumaniseCode("BP_C"), "BP_C");
    EQ(HumaniseCode(""), "");
    // Digits stay attached to the word they follow: "T1", not "T 1".
    EQ(HumaniseCode("BP_RemoteControllerT1_C"), "Remote Controller T1");
    EQ(HumaniseCode("BP_Cigarettes01_C"), "Cigarettes01");
}

// The catalogue must never list a reflection artefact, and BP_PC_C (a *computer* item, not a
// player controller) must never be mistaken for one.
static void TestItemCatalogueExclusions() {
    CHECK(IsGeneratedArtefact("SKEL_BP_Pen_C"), "SKEL_ is excluded");
    CHECK(IsGeneratedArtefact("REINST_BP_Pen_C_42"), "REINST_ is excluded");
    CHECK(IsGeneratedArtefact("Default__BP_Pen_C"), "a CDO is excluded");
    CHECK(IsGeneratedArtefact("TRASHCLASS_BP_Pen_C"), "TRASHCLASS_ is excluded");
    CHECK(!IsGeneratedArtefact("BP_PC_C"), "BP_PC_C is a real item (a computer) and must survive");
    CHECK(!IsGeneratedArtefact("BP_GameController_C"), "BP_GameController_C is a real item");
    CHECK(!IsGeneratedArtefact("BowItem"), "a native item class is not an artefact");
}

// Finding F11: /items, /give and /inventory must all speak the UClass short name.
static void TestItemCodeRoundTrip() {
    using ActionsUtil::ItemCodeFromSoftPath;
    // What FVirtualItemInstance.Item actually holds on the rig (L6a cell 5).
    EQ(ItemCodeFromSoftPath("/Game/Vein/Items/Junk/Office/BP_Pen"), "BP_Pen_C");
    EQ(ItemCodeFromSoftPath("/Game/Vein/Items/Clothing/06_Feet/BP_CL_Shoes_Sneakers"),
       "BP_CL_Shoes_Sneakers_C");
    EQ(ItemCodeFromSoftPath("/Game/Vein/Items/Currency/BP_USD"), "BP_USD_C");
    // An object path, and a value that is already a class name.
    EQ(ItemCodeFromSoftPath("/Game/Vein/Items/Junk/Office/BP_Pen.BP_Pen_C"), "BP_Pen_C");
    EQ(ItemCodeFromSoftPath("BP_Pen_C"), "BP_Pen_C");
    EQ(ItemCodeFromSoftPath(""), "");
    EQ(ItemCodeFromSoftPath("/"), "");

    // The round trip: a catalogue code survives the fold, and giveItem accepts BOTH forms.
    std::vector<ActionsUtil::CatalogueEntry> cat = {{"BP_Pen_C", "Pen"},
                                                    {"BP_USD_C", "US Dollar"},
                                                    {"BP_CL_Shoes_Sneakers_C", "Sneakers"}};
    for (auto& e : cat) EQ(ItemCodeFromSoftPath(e.code), e.code);
    std::string err;
    int a = ActionsUtil::LookupIndex(cat, "BP_Pen_C", err);
    CHECK(a == 0, "the class short name resolves (got %d: %s)", a, err.c_str());
    int b = ActionsUtil::LookupIndex(cat, "/Game/Vein/Items/Junk/Office/BP_Pen", err);
    CHECK(b == 0, "the legacy asset path resolves to the same item (got %d: %s)", b, err.c_str());
    int c = ActionsUtil::LookupIndex(cat, "/Game/Vein/Items/Junk/Office/BP_Pen.BP_Pen_C", err);
    CHECK(c == 0, "an object path resolves to the same item (got %d: %s)", c, err.c_str());
    // A display name still resolves, and an unknown path still fails loudly.
    int d = ActionsUtil::LookupIndex(cat, "US Dollar", err);
    CHECK(d == 1, "the display name resolves (got %d)", d);
    err.clear();
    CHECK(ActionsUtil::LookupIndex(cat, "/Game/Vein/Items/Nope/BP_Nope", err) < 0,
          "an unknown asset path is refused");
    CHECK(!err.empty(), "and says why");
}

// A search must match the readable name as well as the code, case-insensitively.
static void TestItemSearchMatchesDisplayName() {
    using ActionsUtil::MatchesSearch;
    ActionsUtil::CatalogueEntry e{"BP_BloodPressureCuff_C", "Blood Pressure Cuff"};
    CHECK(MatchesSearch(e, "blood pressure"), "matches the display name");
    CHECK(MatchesSearch(e, "bp_blood"), "matches the code");
    CHECK(MatchesSearch(e, "cuff"), "matches a substring of the name");
    CHECK(MatchesSearch(e, ""), "an empty needle matches everything");
    CHECK(!MatchesSearch(e, "hammer"), "an unrelated needle does not match");
}

int main() {
    TestElfParser();
    TestSignatureMatcher();
    TestDepotSymParser();
    TestJson();
    TestRedaction();
    TestRingBuffer();
    TestCapabilities();
    TestPluginBanList();
    TestCharacterNameCache();
    TestLogLineSplit();
    TestSteamIdShape();
    TestChatLineParser();
    TestCharacterSelectParser();
    TestJoinLineParser();
    TestPlayerStateIdParser();
    TestLogRedaction();
    TestLogNoiseFilter();
    TestGameIdNormalisation();
    TestSteamIdPattern();
    TestGeneratedArtefacts();
    TestCommandWords();
    TestItemLookup();
    TestItemSearchFilter();
    TestAdminIdListParsing();
    TestHumaniseCode();
    TestItemCatalogueExclusions();
    TestItemCodeRoundTrip();
    TestItemSearchMatchesDisplayName();
    TestBroadcastPathChoice();
    TestMessagePrefixing();
    TestChatChannelMapping();
    TestTeleportVerification();
    TestGiveVerification();
    TestAttemptedJson();
    TestStackSplit();
    TestPlayerInventoryClassGuard();
    TestCharacterIdFormatting();
    TestKillWeaponName();
    printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "PASSED", g_ran, g_failed);
    return g_failed ? 1 : 0;
}
