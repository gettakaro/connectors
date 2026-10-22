// Unit tests for the #203 research probe's pure logic (config, matching, stub, in-flight table).
#include "command_probe.h"

#include <cstdio>
#include <map>

static int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

using namespace CommandProbe;

static Config Cfg(std::map<std::string, std::string> kv) {
    return ParseConfig([&](const char* k) { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; });
}

static const char* kPuid = "000204f6f3a444a99a44c995d80ff2a7";

int main() {
    // Off unless explicitly on, and off when on but misconfigured.
    CHECK(!Cfg({}).enabled);
    CHECK(!Cfg({{"TAKARO_COMMAND_PROBE", "0"}, {"TAKARO_COMMAND_PROBE_NONCE", "abcd1234"}}).enabled);
    CHECK(!Cfg({{"TAKARO_COMMAND_PROBE", "1"}, {"TAKARO_COMMAND_PROBE_NONCE", "short"},
                {"TAKARO_COMMAND_PROBE_PLAYER", kPuid}}).enabled);
    CHECK(!Cfg({{"TAKARO_COMMAND_PROBE", "1"}, {"TAKARO_COMMAND_PROBE_NONCE", "ABCD1234"},
                {"TAKARO_COMMAND_PROBE_PLAYER", kPuid}}).enabled);
    CHECK(!Cfg({{"TAKARO_COMMAND_PROBE", "1"}, {"TAKARO_COMMAND_PROBE_NONCE", "abcd1234"},
                {"TAKARO_COMMAND_PROBE_PLAYER", "nothex"}}).enabled);
    Config c = Cfg({{"TAKARO_COMMAND_PROBE", "1"}, {"TAKARO_COMMAND_PROBE_NONCE", "abcd1234"},
                    {"TAKARO_COMMAND_PROBE_PLAYER", "000204F6F3A444A99A44C995D80FF2A7"},
                    {"TAKARO_COMMAND_PROBE_DEADLINE_MS", "99999"}});
    CHECK(c.enabled && c.player == kPuid && c.deadlineMs == 2000 && c.stubUp);

    // Exactly the nonce, from exactly the player, with either prefix.
    CHECK(Classify(c, "@takaro_probe_abcd1234", kPuid).match == Match::Probe);
    CHECK(Classify(c, "/takaro_probe_abcd1234", kPuid).prefix == '/');
    CHECK(Classify(c, "@takaro_probe_abcd1234 deny", kPuid).mode == "deny");
    CHECK(Classify(c, "@takaro_probe_abcd1234", kPuid).mode == "ok");
    // Everything else stays ordinary chat.
    CHECK(Classify(c, "hello", kPuid).match == Match::None);
    CHECK(Classify(c, "@ping", kPuid).match == Match::None);
    CHECK(Classify(c, "@takaro_probe_abcd12345", kPuid).reason == "nonce mismatch");
    CHECK(Classify(c, "@takaro_probe_other999", kPuid).match == Match::None);
    CHECK(Classify(c, "!takaro_probe_abcd1234", kPuid).match == Match::None);
    CHECK(Classify(c, " @takaro_probe_abcd1234", kPuid).match == Match::None);
    CHECK(Classify(c, "@takaro_probe_abcd1234 rm -rf", kPuid).reason == "unknown mode");
    CHECK(Classify(c, "@takaro_probe_abcd1234", "11111111111111111111111111111111").reason ==
          "sender is not the probe player");
    CHECK(Classify(c, "@takaro_probe_abcd1234", "").match == Match::None);
    Config off;
    CHECK(Classify(off, "@takaro_probe_abcd1234", kPuid).match == Match::None);

    // Stub outcomes; timeout answers after the deadline; a down stub is unavailable for every mode.
    CHECK(Stub(c, "ok").outcome == Outcome::Success && Stub(c, "ok").delayMs < c.deadlineMs);
    CHECK(Stub(c, "deny").outcome == Outcome::Denied);
    CHECK(Stub(c, "error").outcome == Outcome::Error);
    CHECK(Stub(c, "timeout").delayMs > c.deadlineMs);
    Config down = c;
    down.stubUp = false;
    CHECK(Stub(down, "ok").outcome == Outcome::Unavailable && Stub(down, "timeout").delayMs == 0);

    // First settle wins; a late answer is never delivered; the table is bounded.
    Pending p;
    uint64_t a = p.Begin(1000, 2000);
    CHECK(a != 0 && p.InFlight() == 1);
    CHECK(p.Settle(a));
    CHECK(!p.Settle(a));
    CHECK(!p.Settle(0));
    uint64_t ids[8];
    for (auto& id : ids) id = p.Begin(1000, 2000);
    CHECK(ids[7] != 0 && p.Begin(1000, 2000) == 0);
    CHECK(p.Begin(1000 + 2000 + 10001, 2000) != 0);  // an abandoned slot is reclaimed eventually
    CHECK(ReplyText(Outcome::Timeout, 3).find("will not run later") != std::string::npos);

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("command_probe_test: all passed\n");
    return 0;
}
