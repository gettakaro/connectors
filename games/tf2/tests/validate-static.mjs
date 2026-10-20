#!/usr/bin/env node
import { readFileSync, existsSync } from "node:fs";
import { join, resolve } from "node:path";

const projectRoot = resolve(new URL("..", import.meta.url).pathname);
const repoRoot = resolve(projectRoot, "../..");
const scriptingRoot = join(projectRoot, "mod", "addons", "sourcemod", "scripting");

const pluginPath = join(scriptingRoot, "takaro_tf2.sp");
const itemCatalogPath = join(scriptingRoot, "takaro_tf2_item_catalog.inc");
const configPath = join(projectRoot, "mod", "cfg", "sourcemod", "takaro_tf2.cfg");
const readmePath = join(projectRoot, "README.md");
const capabilitiesPath = join(projectRoot, "capabilities.json");
const packagePath = join(projectRoot, "scripts", "build-release.sh");
const itemCatalogGeneratorPath = join(projectRoot, "scripts", "generate-tf2-item-catalog.mjs");

const plugin = readFileSync(pluginPath, "utf8");
const itemCatalog = existsSync(itemCatalogPath) ? readFileSync(itemCatalogPath, "utf8") : "";
const config = readFileSync(configPath, "utf8");
const readme = readFileSync(readmePath, "utf8");
const capabilities = JSON.parse(readFileSync(capabilitiesPath, "utf8"));

const failures = [];

function assert(condition, message) {
  if (!condition) failures.push(message);
}

for (const path of [pluginPath, itemCatalogPath, configPath, readmePath, capabilitiesPath, packagePath, itemCatalogGeneratorPath]) {
  assert(existsSync(path), `missing required file: ${path}`);
}

const actions = [
  "testReachability",
  "getPlayers",
  "getPlayer",
  "sendMessage",
  "executeConsoleCommand",
  "getPlayerLocation",
  "teleportPlayer",
  "kickPlayer",
  "banPlayer",
  "unbanPlayer",
  "listBans",
  "shutdown",
  "getPlayerInventory",
  "giveItem",
  "listItems",
  "listEntities",
  "listLocations"
];

for (const action of actions) {
  assert(plugin.includes(`"${action}"`), `plugin does not mention action ${action}`);
  assert(capabilities.functions[action], `capabilities missing function ${action}`);
}

const cvars = [
  "takaro_tf2_ws_url",
  "takaro_tf2_server_name",
  "takaro_tf2_identity_token",
  "takaro_tf2_registration_token",
  "takaro_tf2_include_bots",
  "takaro_tf2_reconnect_min_ms",
  "takaro_tf2_reconnect_max_ms",
  "takaro_tf2_command_allowlist_exact",
  "takaro_tf2_command_allowlist_prefixes",
  "takaro_tf2_shutdown_enabled"
];

for (const cvar of cvars) {
  assert(plugin.includes(cvar), `plugin missing cvar ${cvar}`);
  assert(config.includes(cvar), `config missing cvar ${cvar}`);
}

const requiredHooks = [
  "public void OnClientPostAdminCheck(int client)",
  "OnClientDisconnect",
  "AddCommandListener(Command_Say",
  "AddCommandListener(Command_TeamSay",
  "HookEvent(\"player_death\"",
  "HookEvent(\"object_destroyed\"",
  "EmitGameEvent(\"player-connected\"",
  "EmitGameEvent(\"player-disconnected\"",
  "EmitGameEvent(\"chat-message\"",
  "EmitGameEvent(\"player-death\"",
  "EmitGameEvent(\"entity-killed\"",
  "EmitLogEvent("
];

for (const hook of requiredHooks) {
  assert(plugin.includes(hook), `plugin missing hook or event path ${hook}`);
}

assert(plugin.includes("new WebSocket(url, WebSocket_JSON)"), "plugin must use ProjectSky JSON WebSocket mode");
assert(plugin.includes("JSON.Parse("), "plugin must parse incoming JSON through the JSON extension");
assert(plugin.includes("WriteJSON("), "plugin must write JSON through the WebSocket extension");
assert(plugin.includes('message.Set("payload", data)'), "identify must wrap connector fields in a payload object");
assert(plugin.includes('{\\"type\\":\\"gameEvent\\",\\"payload\\":{\\"type\\":\\"%s\\",\\"data\\":%s}}'), "v0 gameEvent must wrap event type/data in a payload object");
assert(plugin.includes('\\"streamId\\":\\"%s\\",\\"seq\\":%d'), "v1 gameEvent must carry streamId and seq");
assert(plugin.includes('"eventAck"') && plugin.includes("HandleEventAck"), "v1 events must be confirmed by eventAck");
assert(plugin.includes('data.Set("protocolVersions", versions)') && plugin.includes('data.Set("capabilities", capabilities)'), "identify must offer protocol versions and the capability manifest");
assert(!/"items\.give"/.test(plugin), "manifest must not declare giveItem");
assert(/if \(g_CvarShutdownEnabled\.BoolValue\) \{\s*JSONObject capability[\s\S]{0,80}"server\.shutdown"/.test(plugin), "server.shutdown is declared only when the shutdown cvar is on");
assert(!plugin.includes('data.SetString("game"'), "identify payload must not include connector-specific game metadata");
assert(!plugin.includes('data.SetString("connector"'), "identify payload must not include connector-specific connector metadata");
assert(!plugin.includes('data.SetString("version"'), "identify payload must not include connector-specific version metadata");
assert(!plugin.includes('response.SetBool("success"'), "responses must not set top-level success fields");
{
  const v1Start = plugin.indexOf("void SendErrorResponseV1(");
  const v1End = plugin.indexOf("\nvoid ", v1Start + 1);
  const outsideV1 = plugin.slice(0, v1Start) + plugin.slice(v1End);
  assert(v1Start !== -1 && plugin.slice(v1Start, v1End).includes('response.Set("error", error)'), "v1 errors must be { error: { code, message } }");
  assert(!outsideV1.includes('response.Set("error"'), "v0 errors must be encoded in the response payload");
}
assert(plugin.includes('payload.SetString("errorCode"'), "error responses must include payload.errorCode");
assert(plugin.includes('result.SetBool("connectable", true)'), "testReachability must return payload.connectable");
assert(plugin.includes('data.SetString("msg", message)'), "log events must include data.msg");
assert(!plugin.includes('data.SetString("level"'), "log events must not include payload.level because EventLogLine only accepts msg");
assert(!plugin.includes('data.SetString("message", message)'), "log events must not include payload.message because EventLogLine only accepts msg");
assert(!plugin.includes('data.SetString("source", "takaro_tf2")'), "log events must not include payload.source because EventLogLine only accepts msg");
assert(plugin.includes('data.SetString("msg", text)'), "chat-message events must include data.msg");
assert(!plugin.includes('data.SetString("message", text)'), "chat-message events must not include payload.message because EventChatMessage only accepts msg");
assert(!plugin.includes('data.SetBool("teamOnly"'), "chat-message events must not include payload.teamOnly because EventChatMessage only accepts channel/player/recipient/msg");
assert(plugin.includes('data.SetString("channel"'), "chat-message events must include data.channel");
assert(plugin.includes("IsCommandAllowed"), "plugin must enforce a command allowlist");
assert(plugin.includes("takaro_tf2_shutdown_enabled") && plugin.includes("ServerCommand(\"quit\")"), "shutdown must be cvar-gated");
assert(plugin.includes("GetClientAuthId(client, AuthId_SteamID64"), "plugin must use SteamID64 identity mapping");
assert(plugin.includes("platformId") && plugin.includes("steam:%s"), "plugin must emit steam:<steam64> platform IDs");
assert(plugin.includes('"SOURCE_%d"'), "plugin must keep a real-client fallback ID for insecure LAN clients without Steam auth");
assert(plugin.includes("ResolveClientIdentity(client, gameId") && plugin.includes("StrEqual(platformId, rawIdentifier"), "player-specific actions must resolve the same gameId/steamId/platformId tuple emitted by getPlayers");
assert(plugin.includes("HandleGetPlayerInventory") && plugin.includes("GetPlayerWeaponSlot"), "getPlayerInventory must enumerate equipped TF2 weapon entities");
assert(plugin.includes('#include "takaro_tf2_item_catalog.inc"'), "plugin must include the generated TF2 item catalog");
assert(plugin.includes("HandleListItems") && plugin.includes("PopulateGeneratedTf2ItemCatalog(items)"), "listItems must use the generated TF2 item catalog");
assert(itemCatalog.includes("Generated from https://raw.githubusercontent.com/tsuza/tf2-item-schema/master/files/localized/item_schema_json_en.txt"), "item catalog must record its GitHub schema source");
assert(itemCatalog.includes("void PopulateGeneratedTf2ItemCatalog(JSONArray items)"), "generated item catalog must expose PopulateGeneratedTf2ItemCatalog");
assert((itemCatalog.match(/PushItemDTO\(/g) || []).length >= 300, "generated item catalog must include the GitHub TF2 schema weapon catalog, not the old 15-item seed list");
assert(itemCatalog.includes('"13"') && itemCatalog.includes('"Scattergun"') && itemCatalog.includes('"tf_weapon_scattergun"'), "generated item catalog must include readable Scattergun schema data");
assert(itemCatalog.includes('"18"') && itemCatalog.includes('"Rocket Launcher"') && itemCatalog.includes('"tf_weapon_rocketlauncher"'), "generated item catalog must include readable Rocket Launcher schema data");
assert(plugin.includes('item.SetString("code", code)') && plugin.includes('item.SetString("name", name)'), "item DTOs must include Takaro-required code and name");
{
  const start = plugin.indexOf("JSONArray HandleListEntities()");
  const body = plugin.slice(start, plugin.indexOf("\n}\n", start));
  assert(start !== -1 && body.includes('"Sentry Gun"') && body.includes('"obj_sentrygun"'), "listEntities must list destructible TF2 entities with display names and classname codes");
  assert(!body.includes("GetMaxEntities"), "listEntities must not dump live engine classnames");
}
assert(plugin.includes('"m_iItemDefinitionIndex"') && plugin.includes("g_ItemNames.GetString"), "getPlayerInventory must use listItems codes (item definition index) and catalog display names");
assert(plugin.includes("HandleListLocations") && plugin.includes("item_teamflag") && plugin.includes("info_player_teamspawn") && !plugin.includes("ctf_2fort:"), "listLocations must read locations from the running map, not hardcoded coordinates");
assert(plugin.includes("HandleGetMapInfo") && plugin.includes('info.SetBool("enabled", false)') && plugin.includes('info.SetInt("mapBlockSize", 0)'), "getMapInfo must return a disabled Takaro MapInfoDTO schema fallback");
assert(plugin.includes('StrEqual(action, "getMapTile")') && plugin.includes("RespondString(requestId") && plugin.includes("iVBORw0KGgo"), "getMapTile must return a transparent placeholder tile instead of surfacing a Takaro 500");
assert(plugin.includes("ResolveRecipientFromRequest") && plugin.includes("/args/opts/recipient/gameId"), "sendMessage must support opts.recipient targeting");
assert(plugin.includes("if (recipient < 0) {") && plugin.includes("return client > 0 ? client : -1;"), "sendMessage to an offline recipient must be refused, never broadcast");
assert(plugin.includes("ExtractSenderNameOverride") && plugin.includes("/args/opts/senderNameOverride") && plugin.includes('strcopy(senderName, sizeof(senderName), "Takaro")'), "sendMessage must support opts.senderNameOverride with a Takaro default prefix");
assert(plugin.includes("g_PlayerPlatformIds.SetString(key, platformId)") && plugin.includes("g_PlayerPlatformIds.GetString"), "disconnect events must reuse the cached platformId emitted by getPlayers");
assert(plugin.includes('ServerCommand("banid %d %s"'), "banPlayer must route through the Source engine banid command");
assert(plugin.includes('ServerCommand("removeid %s"'), "unbanPlayer must route through the Source engine removeid command");
assert(plugin.includes("IsSteamId64") && plugin.includes("SteamId64ToSteam2") && plugin.includes("SOURCE_*/BOT_* fallback ids are not safe to ban"), "banPlayer must reject unsafe fallback identifiers and convert SteamID64 to Source auth format");
assert(plugin.includes("HandleListBans") && plugin.includes("g_TakaroBans.Snapshot()"), "listBans must expose connector-managed SourceMod bans");
assert(plugin.includes("Event_ObjectDestroyed") && plugin.includes('data.SetString("entity", entity)'), "entity-killed must map TF2 object_destroyed events to Takaro EventEntityKilled");
assert(!plugin.includes('data.SetString("timestamp"'), "events must not carry a local-time timestamp; Takaro stamps events itself");
assert(!plugin.includes('"Steam %s"'), "listBans must report the player's real name, never a placeholder");
assert(plugin.includes("SaveBans()") && plugin.includes("LoadAndApplyBans()"), "the ban ledger must persist across restarts");
assert(plugin.includes("cfg/banned_user.cfg") && plugin.includes("Steam2ToSteamId64"), "listBans must include permanent bans made outside Takaro");
assert(!config.match(/token_[A-Za-z0-9]|eyJ[A-Za-z0-9_-]+/), "example config appears to contain a secret token");
assert(!plugin.includes("node ") && !plugin.includes("child_process"), "plugin must not depend on a Node sidecar");
assert(plugin.includes("does not support item grants"), "giveItem must return a structured unsupported response");
assert(readme.includes("## What works") && readme.includes("## Install") && readme.includes("| ❌ |"), "README must hold the install guide and the works table, including what does not work");

function capabilityStatus(value) {
  if (value && typeof value === "object") return value.status;
  return value;
}

const allowedStatuses = new Set(["implemented", "partial", "stub", "missing", "live-supported", "schema-fallback", "unsupported"]);
for (const [name, value] of Object.entries(capabilities.functions)) {
  const status = capabilityStatus(value);
  assert(allowedStatuses.has(status), `invalid function status for ${name}: ${status}`);
}
for (const [name, value] of Object.entries(capabilities.events)) {
  const status = capabilityStatus(value);
  assert(allowedStatuses.has(status), `invalid event status for ${name}: ${status}`);
}

assert(capabilities.architecture === "in-game mod", "TF2 architecture must be in-game mod");
assert(capabilities.takaroConnection === "outbound_websocket", "TF2 Takaro connection must be outbound_websocket");
assert(capabilityStatus(capabilities.events["entity-killed"]) === "live-supported", "entity-killed (TF2 object_destroyed) is live-proven");
assert(capabilityStatus(capabilities.functions.shutdown) === "schema-fallback", "shutdown should be schema-fallback because it is disabled by default");

if (failures.length) {
  console.error("TF2 static validation failed:");
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}

console.log(`OK: TF2 static validation passed for ${pluginPath.replace(`${repoRoot}/`, "")}`);
