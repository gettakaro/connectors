// Host tests for the #205 research probe (conan/command_probe.h): catalog parsing, the claim
// decision and the invocation payload. The probe stays off unless TAKARO_CONAN_COMMAND_PROBE is set.
#include "common.h"
#include "conan/command_probe.h"
#include "takaro/json_util.h"

#include <cstdio>
#include <string>

using namespace conan;
using namespace conan::cmdprobe;

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

static const char* kCatalog = R"({"revision":"r1","prefix":"/TK","commands":[
  {"trigger":"ping","aliases":["p","Pong"]},
  {"trigger":"tp","aliases":["p"]},
  {"trigger":"bad name"},
  {"trigger":"whoami"}]})";

static void TestCatalog() {
    Catalog c;
    std::string err;
    CHECK(ParseCatalog(kCatalog, c, err), "parse: %s", err.c_str());
    EQ(c.revision, "r1");
    EQ(c.prefix, "/tk");
    CHECK(c.trigger.size() == 5, "names %zu", c.trigger.size());
    CHECK(c.duplicates == 1, "duplicate alias p counted once, first wins");
    EQ(c.trigger["p"], "ping");
    EQ(c.trigger["pong"], "ping");
    CHECK(!ParseCatalog("{\"prefix\":\"/tk\",\"commands\":[]}", c, err), "no commands");
    CHECK(!ParseCatalog("{\"prefix\":\"/t k\",\"commands\":[{\"trigger\":\"a\"}]}", c, err), "prefix with space");
    CHECK(!ParseCatalog("{\"commands\":[{\"trigger\":\"a\"}]}", c, err), "no prefix");
    CHECK(!ParseCatalog("{\"prefix\":", c, err), "half-written file");
}

static void TestClassify() {
    Catalog c;
    std::string err;
    ParseCatalog(kCatalog, c, err);
    Classified k = Classify(c, "/tk ping");
    CHECK(k.namespaced && k.known, "known");
    EQ(k.trigger, "ping");
    k = Classify(c, "  /Tk  PONG  a  \"b c\" d\\\"e ");
    CHECK(k.known, "alias, case, spaces");
    EQ(k.trigger, "ping");
    EQ(k.word, "PONG");
    CHECK(k.args.size() == 3, "args %zu", k.args.size());
    if (k.args.size() == 3) {
        EQ(k.args[0], "a");
        EQ(k.args[1], "b c");
        EQ(k.args[2], "d\"e");
    }
    k = Classify(c, "/tk nope 1");
    CHECK(k.namespaced && !k.known, "unknown under the prefix is not claimed");
    k = Classify(c, "/tkping");
    CHECK(!k.namespaced, "prefix must end at a space");
    k = Classify(c, "/tk");
    CHECK(k.namespaced && !k.known, "bare prefix");
    k = Classify(c, "hello /tk ping");
    CHECK(!k.namespaced, "prefix only at the start");
    k = Classify(c, "/ping");
    CHECK(!k.namespaced, "vanilla-style slash text is not ours");
    CHECK(SplitArgs("").empty(), "empty");
    CHECK(SplitArgs("\"\"").size() == 1, "an empty quoted argument is kept");
}

static void TestInvocation() {
    Catalog c;
    std::string err;
    ParseCatalog(kCatalog, c, err);
    events::PlayerId p;
    p.steam64 = "76561198000735875";
    p.name = "Limon";
    std::string s = InvocationJson("ab-1", "r1", Classify(c, "/tk whoami \"x y\""), p, "Global", 1700000000000);
    JsonValue v;
    CHECK(takaro::ParseJson(s, v), "json %s", s.c_str());
    EQ(takaro::JsString(v.get("trigger")), "whoami");
    EQ(takaro::JsString(v.get("catalogRevision")), "r1");
    const JsonValue* pl = v.get("player");
    CHECK(pl && takaro::JsString(pl->get("gameId")) == "76561198000735875", "%s", s.c_str());
    CHECK(pl && takaro::JsString(pl->get("platformId")) == "steam:76561198000735875", "%s", s.c_str());
    const JsonValue* a = v.get("args");
    CHECK(a && a->arr.size() == 1 && a->arr[0].str == "x y", "%s", s.c_str());
}

static void TestOffByDefault() {
    CHECK(!Enabled(), "disabled without TAKARO_CONAN_COMMAND_PROBE");
    events::PlayerId p;
    CHECK(!Offer(p, true, "Global", "/tk ping"), "nothing claimed while disabled");
    EQ(HealthJson(), "null");
}

int main() {
    TestCatalog();
    TestClassify();
    TestInvocation();
    TestOffByDefault();
    printf("command_probe_test: %d checks, %d failed\n", g_ran, g_failed);
    return g_failed ? 1 : 0;
}
