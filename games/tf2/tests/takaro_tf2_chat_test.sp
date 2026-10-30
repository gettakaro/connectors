// Unit tests for takaro_tf2_chat.inc (sender name resolution and the chat renderer).
// scripts/build-release.sh compiles it into the stage out/ folder (never shipped). Load the .smx on any SourceMod server (`sm plugins load`)
// and it prints and logs "takaro_tf2_chat_test: PASS <n>" or one FAIL line per broken case, then unloads.

#include <sourcemod>
#include "takaro_tf2_chat.inc"

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
  name = "Takaro TF2 chat tests",
  author = "Takaro",
  description = "Unit tests for the Takaro TF2 server chat name",
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

void ExpectString(const char[] name, const char[] actual, const char[] expected)
{
  if (StrEqual(actual, expected)) {
    g_Passed++;
  } else {
    g_Failed++;
    Report("takaro_tf2_chat_test: FAIL %s: got \"%s\", want \"%s\"", name, actual, expected);
  }
}

void ExpectBool(const char[] name, bool actual, bool expected)
{
  if (actual == expected) {
    g_Passed++;
  } else {
    g_Failed++;
    Report("takaro_tf2_chat_test: FAIL %s: got %d, want %d", name, actual, expected);
  }
}

void ExpectResolved(const char[] name, const char[] senderNameOverride, const char[] configuredName, bool expectedFound, const char[] expectedName)
{
  char resolved[MAX_SERVER_CHAT_NAME_LENGTH];
  bool found = ResolveServerChatName(senderNameOverride, configuredName, resolved, sizeof(resolved));
  ExpectBool(name, found, expectedFound);
  ExpectString(name, resolved, expectedName);
}

void ExpectRendered(const char[] name, const char[] overrideName, const char[] configuredName, const char[] message, const char[] expected)
{
  char senderName[MAX_SERVER_CHAT_NAME_LENGTH];
  ResolveServerChatName(overrideName, configuredName, senderName, sizeof(senderName));
  char line[640];
  FormatServerChatMessage(senderName, message, line, sizeof(line));
  ExpectString(name, line, expected);
}

public void OnPluginStart()
{
  ExpectResolved("override wins over config", "Override", "Config", true, "Override");
  ExpectResolved("override alone", "Override", "", true, "Override");
  ExpectResolved("config fallback", "", "Config", true, "Config");
  ExpectResolved("override is trimmed", "  Override  ", "Config", true, "Override");
  ExpectResolved("whitespace override is unset", "   ", "Config", true, "Config");
  ExpectResolved("config is trimmed", "", "\tConfig ", true, "Config");
  ExpectResolved("whitespace config is unset", "", "  ", false, "");
  ExpectResolved("nothing set", "", "", false, "");

  // Every send path (global PrintToChatAll and private PrintToChat) prints this one line.
  ExpectRendered("no name renders as today", "", "", "hello", "\x04[Takaro]\x01 hello");
  ExpectRendered("whitespace everywhere renders as today", " ", " ", "hello", "\x04[Takaro]\x01 hello");
  ExpectRendered("config name renders", "", "MyServer", "hello", "\x04[MyServer]\x01 hello");
  ExpectRendered("override name renders", "Admin", "MyServer", "hello", "\x04[Admin]\x01 hello");
  ExpectRendered("percent signs are kept", "", "100% Fun", "50% off", "\x04[100% Fun]\x01 50% off");

  // Player chat counts once the game sends its SayText2 shortly after the say.
  ExpectBool("all chat confirms", ConfirmsPlayerChat("TF_Chat_All", 10.0, 10.0), true);
  ExpectBool("team chat a frame later confirms", ConfirmsPlayerChat("TF_Chat_Team", 10.0, 10.02), true);
  ExpectBool("dead chat confirms", ConfirmsPlayerChat("TF_Chat_AllDead", 10.0, 10.5), true);
  ExpectBool("engine notice does not confirm", ConfirmsPlayerChat("#TF_Name_Change", 10.0, 10.0), false);
  ExpectBool("late SayText2 does not confirm", ConfirmsPlayerChat("TF_Chat_All", 10.0, 11.5), false);
  ExpectBool("nothing pending does not confirm", ConfirmsPlayerChat("TF_Chat_All", -1.0, 10.0), false);

  if (g_Failed == 0) {
    Report("takaro_tf2_chat_test: PASS %d", g_Passed);
  } else {
    Report("takaro_tf2_chat_test: FAILED %d of %d", g_Failed, g_Passed + g_Failed);
  }
  char file[PLATFORM_MAX_PATH];
  GetPluginFilename(null, file, sizeof(file));
  ServerCommand("sm plugins unload \"%s\"", file);
}
