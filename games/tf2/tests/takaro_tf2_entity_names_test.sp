// Unit tests for takaro_tf2_entity_names.inc (entity and weapon display names).
// scripts/build-release.sh compiles it into the stage out/ folder (never shipped). Load the .smx on any SourceMod server (`sm plugins load`)
// and it prints and logs "takaro_tf2_entity_names_test: PASS <n>" or one FAIL line per broken case, then unloads.

#include <sourcemod>
#include "takaro_tf2_entity_names.inc"

#pragma semicolon 1
#pragma newdecls required

public Plugin myinfo = {
  name = "Takaro TF2 entity name tests",
  author = "Takaro",
  description = "Unit tests for the Takaro TF2 entity and weapon names",
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
    Report("takaro_tf2_entity_names_test: FAIL %s: got \"%s\", want \"%s\"", name, actual, expected);
  }
}

void ExpectBool(const char[] name, bool actual, bool expected)
{
  if (actual == expected) {
    g_Passed++;
  } else {
    g_Failed++;
    Report("takaro_tf2_entity_names_test: FAIL %s: got %d, want %d", name, actual, expected);
  }
}

void ExpectObject(int objectType, const char[] expectedCode, const char[] expectedName)
{
  char code[64];
  GetTfObjectEntityCode(objectType, code, sizeof(code));
  char name[128];
  GetTf2EntityName(code, name, sizeof(name));
  char label[64];
  Format(label, sizeof(label), "objecttype %d", objectType);
  ExpectString(label, code, expectedCode);
  ExpectString(label, name, expectedName);
}

void ExpectEntity(const char[] code, bool expectedKnown, const char[] expectedName)
{
  char name[128];
  bool known = GetTf2EntityName(code, name, sizeof(name));
  ExpectBool(code, known, expectedKnown);
  ExpectString(code, name, expectedName);
}

void ExpectWeapon(const char[] label, const char[] icon, const char[] attackerWeapon, const char[] expected)
{
  char name[128];
  ResolveTf2WeaponName(icon, attackerWeapon, name, sizeof(name));
  ExpectString(label, name, expected);
}

void ExpectHumanized(const char[] code, const char[] expected)
{
  char name[128];
  HumanizeTf2Code(code, name, sizeof(name));
  ExpectString(code, name, expected);
}

public void OnPluginStart()
{
  // object_destroyed objecttype -> code (entityCode) and display name (entity).
  ExpectObject(0, "obj_dispenser", "Dispenser");
  ExpectObject(1, "obj_teleporter", "Teleporter");
  ExpectObject(2, "obj_sentrygun", "Sentry Gun");
  ExpectObject(3, "obj_attachment_sapper", "Sapper");
  ExpectObject(7, "obj_7", "Engineer Building");

  // Every listEntities row: the name is a display name, never the code.
  for (int i = 0; i < TF2_ENTITY_COUNT; i++) {
    ExpectEntity(g_Tf2EntityCodes[i], true, g_Tf2EntityNames[i]);
    ExpectBool(g_Tf2EntityCodes[i], StrEqual(g_Tf2EntityCodes[i], g_Tf2EntityNames[i]), false);
    ExpectBool(g_Tf2EntityCodes[i], StrContains(g_Tf2EntityNames[i], "_") == -1, true);
  }
  ExpectEntity("headless_hatman", true, "Horseless Headless Horsemann");
  ExpectEntity("tank_boss", true, "Tank");
  ExpectEntity("tf_robot_destruction_robot", false, "Robot Destruction Robot");
  ExpectEntity("obj_unknown_thing", false, "Engineer Building");

  // Kill icons: stock icons by table, unique icons by the attacker's weapon, the rest humanized.
  ExpectWeapon("rocket", "tf_projectile_rocket", "Black Box", "Rocket Launcher");
  ExpectWeapon("sticky", "tf_projectile_pipe_remote", "", "Stickybomb Launcher");
  ExpectWeapon("level 3 sentry", "obj_sentrygun3", "Wrench", "Sentry Gun");
  ExpectWeapon("mini sentry", "obj_minisentry", "", "Mini-Sentry");
  ExpectWeapon("wrench", "wrench", "Jag", "Wrench");
  ExpectWeapon("shotgun", "shotgun_soldier", "", "Shotgun");
  ExpectWeapon("unique icon uses attacker weapon", "quake_rl", "Original", "Original");
  ExpectWeapon("unique icon without weapon", "the_original_launcher", "", "The Original Launcher");
  ExpectWeapon("weapon prefix dropped", "tf_weapon_grenadelauncher_x", "", "Grenadelauncher X");
  ExpectWeapon("sapper", "obj_attachment_sapper", "Revolver", "Sapper");
  ExpectWeapon("empty icon uses attacker weapon", "", "Rocket Launcher", "Rocket Launcher");
  ExpectWeapon("empty icon without weapon", "", "", "Unknown");
  // Every table name is readable.
  for (int i = 0; i < TF2_KILL_ICON_COUNT; i++) {
    ExpectBool(g_Tf2KillIcons[i], StrContains(g_Tf2KillIconNames[i], "_") == -1 && g_Tf2KillIconNames[i][0] != '\0', true);
  }

  ExpectHumanized("tf_zombie", "Zombie");
  ExpectHumanized("obj_", "Obj");
  ExpectHumanized("__a__b__", "A B");
  ExpectHumanized("_", "Unknown");
  ExpectHumanized("", "Unknown");

  // A short buffer truncates instead of overflowing.
  char small[6];
  HumanizeTf2Code("tf_projectile_pipe_remote", small, sizeof(small));
  ExpectString("short buffer", small, "Pipe");

  if (g_Failed == 0) {
    Report("takaro_tf2_entity_names_test: PASS %d", g_Passed);
  } else {
    Report("takaro_tf2_entity_names_test: FAILED %d of %d", g_Failed, g_Passed + g_Failed);
  }
  char file[PLATFORM_MAX_PATH];
  GetPluginFilename(null, file, sizeof(file));
  ServerCommand("sm plugins unload \"%s\"", file);
}
