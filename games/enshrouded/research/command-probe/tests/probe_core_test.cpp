// Host test for the #202 command probe's decision core. Run: research/command-probe/tests/run.sh
#include "probe_core.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace probe;

HMODULE g_selfModule = nullptr;  // common.cpp (PluginBaseDir) expects the DLL's module handle

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) g_pass++;                                             \
        else {                                                          \
            g_fail++;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

static const char* kCatalog = R"({
  "revision": "r1", "prefix": "@", "nonce": "tkp7f3a", "issuedAtMs": 1000000, "maxAgeSeconds": 300,
  "commands": [
    {"name": "tkprobe", "aliases": ["tkp"]},
    {"name": "Balance"},
    {"name": "off", "enabled": false},
    {"name": "admin", "permission": "probe.admin"},
    {"name": "dupe-a", "aliases": ["dup"]},
    {"name": "dupe-b", "aliases": ["dup"]}
  ],
  "grants": {"76561198000000001": ["probe.admin"]}
})";

static Sender Good(const char* steam = "76561198000000002") {
    Sender s;
    s.valid = true;
    s.steamId = steam;
    s.machine = 1;
    return s;
}

int main() {
    Catalog c;
    std::string err;
    CHECK(ParseCatalog(kCatalog, c, err));
    const uint64_t now = 1000000 + 1000;

    // not ours: passes unchanged
    CHECK(Decide("hello", &c, now, Good()).kind == Kind::Pass);
    CHECK(Decide("/tkprobe", &c, now, Good()).kind == Kind::Pass);
    CHECK(Decide("!tkprobe", &c, now, Good()).kind == Kind::Pass);
    // leading/trailing whitespace: trimmed like the connector's chat-message msg, so Takaro and the probe agree
    CHECK(Decide(" @tkprobe", &c, now, Good()).kind == Kind::Invoke);
    CHECK(Decide("\t@link 1234 \r\n", &c, now, Good()).reason == "reserved command");
    CHECK(Decide("@tkprobe  a b  ", &c, now, Good()).args == "a b");
    CHECK(Decide("\"@tkprobe\"", &c, now, Good()).kind == Kind::Pass);  // quoted
    CHECK(Decide("@", &c, now, Good()).kind == Kind::Pass);
    Decision unk = Decide("@nosuch arg", &c, now, Good());
    CHECK(unk.kind == Kind::Pass && unk.reason == "unknown command");
    CHECK(Decide("@tkprobe", nullptr, now, Good()).kind == Kind::Pass);  // probe off

    // claimed and accepted
    Decision a = Decide("@tkprobe  one two", &c, now, Good());
    CHECK(a.kind == Kind::Invoke && a.command == "tkprobe" && a.args == "one two");
    CHECK(Decide("@TKP", &c, now, Good()).command == "tkprobe");  // alias, case-insensitive
    CHECK(Decide("@balance", &c, now, Good()).kind == Kind::Invoke);
    Decision n = Decide("@tkp7f3a", &c, now, Good());
    CHECK(n.kind == Kind::Invoke && n.command == "tkp7f3a");
    CHECK(Decide("@tkprobe ünïcødé ✓", &c, now, Good()).args == "ünïcødé ✓");

    // claimed and refused: always Reject (suppressed), never Pass
    CHECK(Decide("@off", &c, now, Good()).reason == "command disabled");
    CHECK(Decide("@link 1234", &c, now, Good()).reason == "reserved command");
    CHECK(Decide("@admin", &c, now, Good()).reason == "permission denied");
    CHECK(Decide("@admin", &c, now, Good("76561198000000001")).kind == Kind::Invoke);
    CHECK(Decide("@dup", &c, now, Good()).reason == "ambiguous command");
    CHECK(Decide("@dupe-a", &c, now, Good()).kind == Kind::Invoke);  // the unique name still works
    Sender bad;
    bad.reason = "stale-handle";
    Decision b = Decide("@tkprobe", &c, now, bad);
    CHECK(b.kind == Kind::Reject && b.reason == "sender not verified (stale-handle)");
    CHECK(Decide("@hello", &c, now, bad).kind == Kind::Pass);  // an unclaimed name is never touched
    // stale catalog: claimed names stay private and fail
    CHECK(Decide("@tkprobe", &c, 1000000 + 301000, Good()).reason == "command list is out of date");
    CHECK(Decide("@tkprobe", &c, 999999, Good()).reason == "command list is out of date");  // clock before issue
    Catalog down = c;
    down.available = false;
    CHECK(Decide("@tkprobe", &down, now, Good()).reason == "Takaro is unavailable");
    CHECK(Decide("@link", &down, now, Good()).kind == Kind::Reject);

    // collision preflight
    Catalog x;
    const char* fresh = R"("issuedAtMs":1000000,"maxAgeSeconds":300)";
    CHECK(!ParseCatalog(std::string("{\"nonce\":\"tkp\",") + fresh + R"(,"commands":[{"name":"tkprobe","aliases":["tkp"]}]})", x, err));
    CHECK(err.find("collides") != std::string::npos);
    CHECK(!ParseCatalog(std::string(R"({"nonce":"link",)") + fresh + "}", x, err));
    CHECK(!ParseCatalog(std::string(R"({"commands":[],)") + fresh + "}", x, err));  // no nonce
    CHECK(!ParseCatalog(std::string(R"({"nonce":"a","prefix":"",)") + fresh + "}", x, err));
    CHECK(ParseCatalog(std::string(R"({"nonce":"a",)") + fresh + "}", x, err));
    // freshness is required and strict: a bad value never turns the stale check off
    CHECK(!ParseCatalog(R"({"nonce":"a","maxAgeSeconds":300})", x, err));                       // no issuedAtMs
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000})", x, err));                       // no maxAge
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000,"maxAgeSeconds":0})", x, err));
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000,"maxAgeSeconds":-5})", x, err));
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000,"maxAgeSeconds":1.5})", x, err));
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000,"maxAgeSeconds":"300"})", x, err));
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":1000000,"maxAgeSeconds":99999999999999999999})", x, err));
    CHECK(!ParseCatalog(R"({"nonce":"a","issuedAtMs":-1,"maxAgeSeconds":300})", x, err));
    CHECK(!ParseCatalog("not json", x, err));

    // replies
    CHECK(ReplyText(a, "b-1", false) == "[probe] tkprobe: submitted (id b-1)");
    CHECK(ReplyText(a, "b-1", true) == "[probe] tkprobe: failed, not run (id b-1)");
    CHECK(ReplyText(Decide("@off", &c, now, Good()), "", false) == "[probe] off: command disabled");
    CHECK(Hex("@a\xc3\xbc") == "4061c3bc");

    std::printf("probe_core_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
