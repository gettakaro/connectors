// Display names for the entities and weapons in entity-killed and listEntities. The entity code is the
// TF2 classname; players and Takaro see the name. Kept free of JSON and game state so
// games/tf2/tests/takaro_tf2_entity_names_test.sp can test it directly.

#if defined _takaro_tf2_entity_names_included
  #endinput
#endif
#define _takaro_tf2_entity_names_included

// The destructible TF2 entities a player can kill: Engineer buildings and the Halloween/MvM bosses.
#define TF2_ENTITY_COUNT 9

stock const char g_Tf2EntityCodes[TF2_ENTITY_COUNT][] = {
  "obj_sentrygun", "obj_dispenser", "obj_teleporter", "obj_attachment_sapper",
  "tank_boss", "headless_hatman", "eyeball_boss", "merasmus", "tf_zombie"
};
stock const char g_Tf2EntityNames[TF2_ENTITY_COUNT][] = {
  "Sentry Gun", "Dispenser", "Teleporter", "Sapper",
  "Tank", "Horseless Headless Horsemann", "MONOCULUS", "Merasmus", "Skeleton"
};
stock const char g_Tf2EntityTypes[TF2_ENTITY_COUNT][] = {
  "neutral", "neutral", "neutral", "neutral",
  "hostile", "hostile", "hostile", "hostile", "hostile"
};

// Kill icons (the `weapon` string of TF2 kill events) of the stock weapons and buildings. Unique
// weapons have their own icons; the plugin names those from the attacker's weapon instead.
#define TF2_KILL_ICON_COUNT 41

stock const char g_Tf2KillIcons[TF2_KILL_ICON_COUNT][] = {
  "tf_projectile_rocket", "tf_projectile_pipe", "tf_projectile_pipe_remote", "tf_projectile_arrow",
  "tf_projectile_sentryrocket", "obj_sentrygun", "obj_sentrygun2", "obj_sentrygun3",
  "obj_minisentry", "scattergun", "pistol_scout", "pistol",
  "bat", "shotgun_soldier", "shotgun_hwg", "shotgun_pyro",
  "shotgun_primary", "shovel", "flamethrower", "fireaxe",
  "bottle", "minigun", "fists", "wrench",
  "smg", "sniperrifle", "club", "knife",
  "revolver", "syringegun_medic", "bonesaw", "sword",
  "flaregun", "deflect_rocket", "deflect_promode", "deflect_sticky",
  "deflect_arrow", "deflect_flare", "telefrag", "world",
  "obj_attachment_sapper"
};
stock const char g_Tf2KillIconNames[TF2_KILL_ICON_COUNT][] = {
  "Rocket Launcher", "Grenade Launcher", "Stickybomb Launcher", "Huntsman",
  "Sentry Gun", "Sentry Gun", "Sentry Gun", "Sentry Gun",
  "Mini-Sentry", "Scattergun", "Pistol", "Pistol",
  "Bat", "Shotgun", "Shotgun", "Shotgun",
  "Shotgun", "Shovel", "Flame Thrower", "Fire Axe",
  "Bottle", "Minigun", "Fists", "Wrench",
  "SMG", "Sniper Rifle", "Kukri", "Knife",
  "Revolver", "Syringe Gun", "Bonesaw", "Eyelander",
  "Flare Gun", "Reflected Rocket", "Reflected Grenade", "Reflected Stickybomb",
  "Reflected Arrow", "Reflected Flare", "Telefrag", "World",
  "Sapper"
};

// TF2 object_destroyed objecttype -> entity code.
stock void GetTfObjectEntityCode(int objectType, char[] buffer, int maxlen)
{
  switch (objectType) {
    case 0: {
      strcopy(buffer, maxlen, "obj_dispenser");
    }
    case 1: {
      strcopy(buffer, maxlen, "obj_teleporter");
    }
    case 2: {
      strcopy(buffer, maxlen, "obj_sentrygun");
    }
    case 3: {
      strcopy(buffer, maxlen, "obj_attachment_sapper");
    }
    default: {
      Format(buffer, maxlen, "obj_%d", objectType);
    }
  }
}

// "tf_projectile_pipe_remote" -> "Pipe Remote": drops the engine prefix, splits on '_' and capitalises
// each word. For codes and icons without a known name; never returns an empty string.
stock void HumanizeTf2Code(const char[] code, char[] buffer, int maxlen)
{
  static const char prefixes[][] = { "tf_weapon_", "tf_projectile_", "obj_", "tf_" };
  int start = 0;
  for (int i = 0; i < sizeof(prefixes); i++) {
    int len = strlen(prefixes[i]);
    if (strncmp(code, prefixes[i], len, false) == 0 && code[len] != '\0') {
      start = len;
      break;
    }
  }

  int out = 0;
  bool wordStart = true;
  for (int i = start; code[i] != '\0' && out < maxlen - 1; i++) {
    int c = code[i];
    if (c == '_' || c == ' ') {
      if (out > 0 && buffer[out - 1] != ' ') {
        buffer[out++] = ' ';
      }
      wordStart = true;
      continue;
    }
    buffer[out++] = wordStart ? CharToUpper(c) : c;
    wordStart = false;
  }
  if (out > 0 && buffer[out - 1] == ' ') {
    out--;
  }
  buffer[out] = '\0';

  if (buffer[0] == '\0') {
    strcopy(buffer, maxlen, "Unknown");
  }
}

// The display name of an entity code. Unknown codes get a readable form of the code; returns false then.
stock bool GetTf2EntityName(const char[] code, char[] buffer, int maxlen)
{
  for (int i = 0; i < TF2_ENTITY_COUNT; i++) {
    if (StrEqual(code, g_Tf2EntityCodes[i])) {
      strcopy(buffer, maxlen, g_Tf2EntityNames[i]);
      return true;
    }
  }
  if (strncmp(code, "obj_", 4) == 0) {
    strcopy(buffer, maxlen, "Engineer Building");
    return false;
  }
  HumanizeTf2Code(code, buffer, maxlen);
  return false;
}

// The display name of a stock kill icon. Returns false (buffer untouched) for any other icon.
stock bool GetTf2KillIconName(const char[] icon, char[] buffer, int maxlen)
{
  for (int i = 0; i < TF2_KILL_ICON_COUNT; i++) {
    if (StrEqual(icon, g_Tf2KillIcons[i])) {
      strcopy(buffer, maxlen, g_Tf2KillIconNames[i]);
      return true;
    }
  }
  return false;
}

// The weapon name for a kill: a stock icon's name, else the attacker's weapon name (empty when unknown),
// else a readable form of the icon ("Unknown" without one).
stock void ResolveTf2WeaponName(const char[] icon, const char[] attackerWeaponName, char[] buffer, int maxlen)
{
  if (GetTf2KillIconName(icon, buffer, maxlen)) {
    return;
  }
  if (attackerWeaponName[0] != '\0') {
    strcopy(buffer, maxlen, attackerWeaponName);
    return;
  }
  HumanizeTf2Code(icon, buffer, maxlen);
}
