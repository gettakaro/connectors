// Host tests for the Blueprint SDK research probe (conan/blueprint_probe.h): the console-route
// log line parser, the request payload, and off by default.
#include "common.h"
#include "conan/blueprint_probe.h"
#include "takaro/json_util.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace conan;
using namespace conan::bpprobe;

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

static const char* kPre = "[2026.10.31-10.00.00:012][345]";

static void TestParse() {
    Request r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro run 76561198000000001 @ping");
    CHECK(r.ok, "run: %s", r.error.c_str());
    EQ(r.verb, "run");
    EQ(r.steam64, "76561198000000001");
    EQ(r.text, "@ping");

    r = ParseLogLine(std::string(kPre) + "Command not recognized: TAKARO Run 76561198000000001   @tp  \"a b\"  ");
    CHECK(r.ok, "case and spaces: %s", r.error.c_str());
    EQ(r.text, "@tp  \"a b\"");

    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro event - questDone {\"quest\":\"q1\"}");
    CHECK(r.ok, "event: %s", r.error.c_str());
    EQ(r.verb, "event");
    EQ(r.steam64, "");
    EQ(r.name, "questDone");
    EQ(r.json, "{\"quest\":\"q1\"}");

    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro event 76561198000000001 boss");
    CHECK(r.ok && r.steam64 == "76561198000000001" && r.json.empty(), "event with player, no payload");

    // Not ours: no error, not ok.
    r = ParseLogLine(std::string(kPre) + "Command not recognized: nosuchcmd");
    CHECK(!r.ok && r.error.empty(), "other command ignored");
    r = ParseLogLine(std::string(kPre) + "LogNet: takaro run 76561198000000001 @ping");
    CHECK(!r.ok && r.error.empty(), "only the not-recognized line counts");
    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaroish run 76561198000000001 @ping");
    CHECK(!r.ok && r.error.empty(), "root word must match exactly");

    // Ours but malformed.
    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro run Limon @ping");
    CHECK(!r.ok && !r.error.empty(), "name instead of Steam64");
    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro run 76561198000000001");
    CHECK(!r.ok && !r.error.empty(), "no text");
    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro event 123 x");
    CHECK(!r.ok && !r.error.empty(), "bad player id");
    r = ParseLogLine(std::string(kPre) + "Command not recognized: takaro frobnicate");
    CHECK(!r.ok && !r.error.empty(), "unknown verb");
}

static void TestPayload() {
    Request r = ParseLogLine("Command not recognized: takaro run 76561198000000001 @ping");
    JsonValue v;
    CHECK(takaro::ParseJson(RequestJson("ab-1", r, 1793430000000, 120), v), "payload parses");
    EQ(takaro::Str(v.get("id")).value_or(""), "ab-1");
    EQ(takaro::Str(v.get("verb")).value_or(""), "run");
    EQ(takaro::Str(v.get("steam64")).value_or(""), "76561198000000001");
    EQ(takaro::Str(v.get("text")).value_or(""), "@ping");
    EQ(takaro::Str(v.get("route")).value_or(""), "console-log");
}

static void TestOffByDefault() {
    unsetenv("TAKARO_CONAN_BLUEPRINT_PROBE");
    Start("/nonexistent");
    CHECK(!Enabled(), "probe must stay off without the env var");
    EQ(HealthJson(), "null");
}

int main() {
    TestParse();
    TestPayload();
    TestOffByDefault();
    printf("blueprint_probe_test: %d checks, %d failed\n", g_ran, g_failed);
    return g_failed ? 1 : 0;
}
