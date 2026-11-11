// Unit tests for takaro_tf2_reconnect.inc (the reconnect delay policy).
// scripts/build-release.sh compiles it into the stage out/ folder (never shipped). Load the .smx on any SourceMod server (`sm plugins load`)
// and it prints and logs "takaro_tf2_reconnect_test: PASS <n>" or one FAIL line per broken case, then unloads.

#include <sourcemod>
#include "takaro_tf2_reconnect.inc"

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
  name = "Takaro TF2 reconnect tests",
  author = "Takaro",
  description = "Unit tests for the Takaro TF2 reconnect policy",
  version = "1",
  url = "https://github.com/gettakaro/connectors"
};

int g_Passed;
int g_Failed;

void Report(const char[] format, any ...)
{
  char line[1024];
  VFormat(line, sizeof(line), format, 2);
  LogMessage("%s", line);
  PrintToServer("%s", line);
}

void ExpectInt(const char[] name, int actual, int expected)
{
  if (actual == expected) {
    g_Passed++;
  } else {
    g_Failed++;
    Report("takaro_tf2_reconnect_test: FAIL %s: got %d, want %d", name, actual, expected);
  }
}

void ExpectDelay(const char[] name, int streak, int baseMs, int capMs, int jitter, int expected)
{
  ExpectInt(name, ReconnectDelayMs(streak, baseMs, capMs, jitter), expected);
}

public void OnPluginStart()
{
  // First retry after a stable session: 0.25-1.0 s whatever the configured backoff.
  ExpectDelay("stable drop, no jitter", 0, 1000, 60000, 0, 250);
  ExpectDelay("stable drop, half jitter", 0, 1000, 60000, 500, 625);
  ExpectDelay("stable drop, full jitter", 0, 1000, 60000, 1000, 1000);
  ExpectDelay("stable drop ignores a slow base", 0, 30000, 60000, 1000, 1000);

  // Consecutive failures: base * 2^(streak-1), plus up to 100 % jitter.
  ExpectDelay("streak 1 low", 1, 1000, 60000, 0, 1000);
  ExpectDelay("streak 1 high", 1, 1000, 60000, 1000, 2000);
  ExpectDelay("streak 2 low", 2, 1000, 60000, 0, 2000);
  ExpectDelay("streak 3 mid", 3, 1000, 60000, 500, 6000);
  ExpectDelay("streak 5 low", 5, 1000, 60000, 0, 16000);
  ExpectDelay("streak 6 high is capped", 6, 1000, 60000, 1000, 60000);

  // At the cap: 75-100 % of it, never above.
  ExpectDelay("cap low", 7, 1000, 60000, 0, 45000);
  ExpectDelay("cap high", 7, 1000, 60000, 1000, 60000);
  ExpectDelay("long streak stays capped", 1000, 1000, 60000, 1000, 60000);
  ExpectDelay("old 30 s cap is honoured", 9, 1000, 30000, 1000, 30000);

  // Bad config and jitter values are clamped.
  ExpectDelay("jitter above range", 0, 1000, 60000, 5000, 1000);
  ExpectDelay("jitter below range", 0, 1000, 60000, -5, 250);
  ExpectDelay("zero base uses the floor", 1, 0, 60000, 0, 100);
  ExpectDelay("cap below base wins", 3, 2000, 500, 0, 375);
  ExpectDelay("cap below base wins at full jitter", 1, 2000, 500, 1000, 500);

  // The streak resets only on a stable connection.
  ExpectInt("stable resets the streak", NextFailureStreak(4, true), 0);
  ExpectInt("unstable counts a failure", NextFailureStreak(0, false), 1);
  ExpectInt("failures add up", NextFailureStreak(3, false), 4);

  if (g_Failed == 0) {
    Report("takaro_tf2_reconnect_test: PASS %d", g_Passed);
  } else {
    Report("takaro_tf2_reconnect_test: FAILED %d of %d", g_Failed, g_Passed + g_Failed);
  }
  char file[PLATFORM_MAX_PATH];
  GetPluginFilename(null, file, sizeof(file));
  ServerCommand("sm plugins unload \"%s\"", file);
}
