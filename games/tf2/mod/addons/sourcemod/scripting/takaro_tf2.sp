#include <sourcemod>
#include <sdktools>
#include <tf2_stocks>
#include <json>
#include <websocket>

#include "takaro_tf2_item_catalog.inc"

#pragma semicolon 1
#pragma newdecls required

#define PLUGIN_VERSION "0.1.0" // x-release-please-version
#define MAX_TOKEN_LENGTH 512
#define MAX_WS_URL_LENGTH 256
#define MAX_SERVER_NAME_LENGTH 128
#define MAX_COMMAND_POLICY_LENGTH 1024
#define MAX_TAKARO_MESSAGE_LENGTH 8192
#define MAX_PLAYER_CACHE_VALUE 128
#define OUTBOX_MAX_EVENTS 512
#define OUTBOX_DATA_BYTES 4096
#define ACK_TIMEOUT_SECONDS 30.0

public Plugin myinfo = {
  name = "Takaro TF2 Connector",
  author = "Takaro Connector Research",
  description = "Connects Team Fortress 2 SourceMod servers to Takaro Generic Connector over WebSocket",
  version = PLUGIN_VERSION,
  url = "https://docs.takaro.io/advanced/generic-connector-protocol/"
};

ConVar g_CvarWsUrl;
ConVar g_CvarServerName;
ConVar g_CvarIdentityToken;
ConVar g_CvarRegistrationToken;
ConVar g_CvarIncludeBots;
ConVar g_CvarReconnectMinMs;
ConVar g_CvarReconnectMaxMs;
ConVar g_CvarCommandAllowlistExact;
ConVar g_CvarCommandAllowlistPrefixes;
ConVar g_CvarShutdownEnabled;
ConVar g_CvarProtocol;

WebSocket g_WebSocket = null;
StringMap g_PlayerSteamIds = null;
StringMap g_PlayerNames = null;
StringMap g_PlayerPlatformIds = null;
StringMap g_TakaroBans = null;
StringMap g_TakaroBanAuthIds = null;
StringMap g_TakaroBanReasons = null;
StringMap g_TakaroBanExpiresAt = null;
StringMap g_TakaroBanNames = null;
StringMap g_LastSeenNames = null; // SteamID64 -> last in-game name, for bans of offline players
StringMap g_ItemNames = null; // item definition index -> catalog display name
bool g_RecordItemNames = false; // true while BuildItemNameLookup runs the generated catalog
bool g_Connected = false;
bool g_Identified = false;

// Takaro Generic Protocol version chosen in identifyResponse (0 unless Takaro picks 1).
int g_ProtocolVersion = 0;
bool g_OfferedV1 = false;
// Version 1 event stream: every event gets the next seq of this process's stream and stays queued
// until Takaro confirms it with a cumulative eventAck, so a reconnect resends it with the same seq.
// Version 0 sends the queue once and clears it.
char g_StreamId[48];
int g_NextSeq = 1;
// Intel home positions per team (index 2 = RED, 3 = BLU), cached at round start while flags are home.
float g_FlagHome[4][3];
bool g_FlagHomeSet[4];
ArrayList g_OutboxSeq = null;
ArrayList g_OutboxType = null;
ArrayList g_OutboxData = null;
float g_LastAckTime = 0.0;

public void OnPluginStart()
{
  g_OutboxSeq = new ArrayList();
  g_OutboxType = new ArrayList(ByteCountToCells(32));
  g_OutboxData = new ArrayList(ByteCountToCells(OUTBOX_DATA_BYTES));
  Format(g_StreamId, sizeof(g_StreamId), "tf2-%d-%d", GetTime(), GetRandomInt(100000, 999999));

  g_CvarWsUrl = CreateConVar("takaro_tf2_ws_url", "wss://connect.takaro.io", "Takaro Generic Connector WebSocket URL.");
  g_CvarServerName = CreateConVar("takaro_tf2_server_name", "TF2 Dedicated Server", "Name reported to Takaro.");
  g_CvarIdentityToken = CreateConVar("takaro_tf2_identity_token", "", "Takaro connector identity token.", FCVAR_PROTECTED);
  g_CvarRegistrationToken = CreateConVar("takaro_tf2_registration_token", "", "Takaro connector registration token.", FCVAR_PROTECTED);
  g_CvarIncludeBots = CreateConVar("takaro_tf2_include_bots", "0", "Include bots in players and events.");
  g_CvarReconnectMinMs = CreateConVar("takaro_tf2_reconnect_min_ms", "1000", "Minimum WebSocket reconnect wait in milliseconds.");
  g_CvarReconnectMaxMs = CreateConVar("takaro_tf2_reconnect_max_ms", "30000", "Maximum WebSocket reconnect wait in milliseconds.");
  g_CvarCommandAllowlistExact = CreateConVar("takaro_tf2_command_allowlist_exact", "status", "Semicolon-separated exact commands allowed for executeConsoleCommand.");
  g_CvarCommandAllowlistPrefixes = CreateConVar("takaro_tf2_command_allowlist_prefixes", "", "Semicolon-separated command prefixes allowed for executeConsoleCommand.");
  g_CvarShutdownEnabled = CreateConVar("takaro_tf2_shutdown_enabled", "0", "Allow Takaro shutdown to execute the quit command.");
  g_CvarProtocol = CreateConVar("takaro_tf2_protocol", "1", "Highest Takaro Generic Protocol version to offer (0 or 1).", _, true, 0.0, true, 1.0);

  AutoExecConfig(true, "takaro_tf2");

  g_PlayerSteamIds = new StringMap();
  g_PlayerNames = new StringMap();
  g_PlayerPlatformIds = new StringMap();
  g_TakaroBans = new StringMap();
  g_TakaroBanAuthIds = new StringMap();
  g_TakaroBanReasons = new StringMap();
  g_TakaroBanExpiresAt = new StringMap();
  g_TakaroBanNames = new StringMap();
  g_LastSeenNames = new StringMap();
  BuildItemNameLookup();

  AddCommandListener(Command_Say, "say");
  AddCommandListener(Command_TeamSay, "say_team");
  HookEvent("player_death", Event_PlayerDeath, EventHookMode_Post);
  HookEvent("object_destroyed", Event_ObjectDestroyed, EventHookMode_Post);
  HookEvent("teamplay_round_start", Event_RoundStart, EventHookMode_PostNoCopy);

  RegServerCmd("takaro_tf2_reconnect", Command_Reconnect);
  RegServerCmd("takaro_tf2_state", Command_State);

  CreateTimer(5.0, Timer_CheckEventAck, _, TIMER_REPEAT);

  EmitLogEvent("info", "Takaro TF2 connector loaded");
}

public void OnConfigsExecuted()
{
  static bool bansLoaded = false;
  if (!bansLoaded) {
    bansLoaded = true;
    LoadAndApplyBans();
  }
  ConnectTakaro();
}

public void OnPluginEnd()
{
  EmitLogEvent("info", "Takaro TF2 connector unloading");

  if (g_WebSocket != null) {
    if (g_WebSocket.ReadyState != CLOSED) {
      g_WebSocket.Disconnect();
    }
    delete g_WebSocket;
    g_WebSocket = null;
  }

  delete g_PlayerSteamIds;
  delete g_PlayerNames;
  delete g_PlayerPlatformIds;
  delete g_TakaroBans;
  delete g_TakaroBanAuthIds;
  delete g_TakaroBanReasons;
  delete g_TakaroBanExpiresAt;
  delete g_TakaroBanNames;
  delete g_LastSeenNames;
}

public void OnMapStart()
{
  for (int team = 0; team < 4; team++) {
    g_FlagHomeSet[team] = false;
  }
  EmitLogEvent("info", "TF2 map started");
}

// Runs once a real client is both in game and Steam-authorized. OnClientAuthorized alone usually fires
// while the client is still loading (not in game yet), so the connect event was never sent.
public void OnClientPostAdminCheck(int client)
{
  if (IsFakeClient(client) || !IsTakaroClient(client)) {
    return;
  }

  EmitPlayerConnected(client);
}

public void OnClientPutInServer(int client)
{
  if (!g_CvarIncludeBots.BoolValue || !IsFakeClient(client)) {
    return;
  }

  CreateTimer(0.1, Timer_EmitBotConnected, GetClientUserId(client), TIMER_FLAG_NO_MAPCHANGE);
}

public Action Timer_EmitBotConnected(Handle timer, any userid)
{
  int client = GetClientOfUserId(userid);
  if (IsTakaroClient(client)) {
    EmitPlayerConnected(client);
  }

  return Plugin_Stop;
}

void EmitPlayerConnected(int client)
{
  CacheClientIdentity(client);
  JSONObject data = new JSONObject();
  JSONObject player = BuildPlayerObject(client);
  data.Set("player", player);
  EmitGameEvent("player-connected", data);
  delete data;
  delete player;
}

public void OnClientDisconnect(int client)
{
  char key[16];
  ClientKey(client, key, sizeof(key));

  char steamId[64];
  char platformId[80];
  char name[MAX_NAME_LENGTH];
  bool hadSteam = g_PlayerSteamIds.GetString(key, steamId, sizeof(steamId));
  bool hadPlatform = g_PlayerPlatformIds.GetString(key, platformId, sizeof(platformId));
  bool hadName = g_PlayerNames.GetString(key, name, sizeof(name));

  if (hadSteam && hadPlatform && hadName) {
    JSONObject data = new JSONObject();
    JSONObject player = new JSONObject();
    SetPlayerIdentity(player, steamId, platformId);
    player.SetString("name", name);
    data.Set("player", player);
    EmitGameEvent("player-disconnected", data);
    delete data;
    delete player;
  }

  g_PlayerSteamIds.Remove(key);
  g_PlayerNames.Remove(key);
  g_PlayerPlatformIds.Remove(key);
}

Action Command_Say(int client, const char[] command, int argc)
{
  return HandleChatCommand(client, false);
}

Action Command_TeamSay(int client, const char[] command, int argc)
{
  return HandleChatCommand(client, true);
}

Action HandleChatCommand(int client, bool teamOnly)
{
  if (!IsTakaroClient(client)) {
    return Plugin_Continue;
  }

  char text[512];
  GetCmdArgString(text, sizeof(text));
  StripQuotes(text);
  TrimString(text);

  if (text[0] == '\0') {
    return Plugin_Continue;
  }

  JSONObject player = BuildPlayerObject(client);
  JSONObject data = new JSONObject();
  data.Set("player", player);
  if (teamOnly) {
    data.SetString("channel", "team");
  } else {
    data.SetString("channel", "global");
  }
  data.SetString("msg", text);
  EmitGameEvent("chat-message", data);
  delete data;
  delete player;

  return Plugin_Continue;
}

void Event_PlayerDeath(Event event, const char[] name, bool dontBroadcast)
{
  int victim = GetClientOfUserId(event.GetInt("userid"));
  int attacker = GetClientOfUserId(event.GetInt("attacker"));

  if (!IsTakaroClient(victim)) {
    return;
  }

  JSONObject data = new JSONObject();
  JSONObject victimPlayer = BuildPlayerObject(victim);
  data.Set("player", victimPlayer);
  delete victimPlayer;

  // A suicide (kill bind, class switch, fall) is not a kill: no attacker.
  if (attacker != victim && IsTakaroClient(attacker)) {
    JSONObject attackerPlayer = BuildPlayerObject(attacker);
    data.Set("attacker", attackerPlayer);
    delete attackerPlayer;
  } else {
    data.SetNull("attacker");
  }

  float origin[3];
  GetClientAbsOrigin(victim, origin);
  JSONObject position = new JSONObject();
  position.SetFloat("x", origin[0]);
  position.SetFloat("y", origin[1]);
  position.SetFloat("z", origin[2]);
  data.Set("position", position);
  delete position;

  EmitGameEvent("player-death", data);
  delete data;
}

void Event_ObjectDestroyed(Event event, const char[] name, bool dontBroadcast)
{
  int attacker = GetClientOfUserId(event.GetInt("attacker"));
  if (!IsTakaroClient(attacker)) {
    return;
  }

  int owner = GetClientOfUserId(event.GetInt("userid"));
  int objectType = event.GetInt("objecttype");
  int entityIndex = event.GetInt("index");

  char entity[128];
  GetTfObjectEntityName(objectType, entity, sizeof(entity));

  char weapon[128];
  event.GetString("weapon", weapon, sizeof(weapon));
  if (weapon[0] == '\0') {
    strcopy(weapon, sizeof(weapon), "unknown");
  }

  JSONObject data = new JSONObject();
  JSONObject player = BuildPlayerObject(attacker);
  data.Set("player", player);
  delete player;

  data.SetString("entity", entity);
  data.SetString("weapon", weapon);

  char msg[256];
  if (IsTakaroClient(owner)) {
    char ownerName[MAX_NAME_LENGTH];
    GetClientName(owner, ownerName, sizeof(ownerName));
    Format(msg, sizeof(msg), "Destroyed %s entity %d owned by %s", entity, entityIndex, ownerName);
  } else {
    Format(msg, sizeof(msg), "Destroyed %s entity %d", entity, entityIndex);
  }
  data.SetString("msg", msg);

  EmitGameEvent("entity-killed", data);
  delete data;
}

Action Command_Reconnect(int args)
{
  ConnectTakaro();
  return Plugin_Handled;
}

Action Command_State(int args)
{
  PrintToServer("[TakaroTF2] connected=%d identified=%d ws=%x", g_Connected, g_Identified, g_WebSocket);
  return Plugin_Handled;
}

void ConnectTakaro()
{
  char url[MAX_WS_URL_LENGTH];
  g_CvarWsUrl.GetString(url, sizeof(url));
  TrimString(url);

  if (url[0] == '\0') {
    EmitLogEvent("error", "takaro_tf2_ws_url is empty; connector will not start");
    return;
  }

  if (g_WebSocket != null) {
    if (g_WebSocket.ReadyState != CLOSED) {
      g_WebSocket.Disconnect();
    }
    delete g_WebSocket;
    g_WebSocket = null;
  }

  g_Connected = false;
  g_Identified = false;

  g_WebSocket = new WebSocket(url, WebSocket_JSON);
  g_WebSocket.SetOpenCallback(OnTakaroOpen);
  g_WebSocket.SetMessageCallback(OnTakaroMessage);
  g_WebSocket.SetCloseCallback(OnTakaroClose);
  g_WebSocket.SetErrorCallback(OnTakaroError);
  g_WebSocket.AutoReconnect = true;
  g_WebSocket.MinReconnectWait = g_CvarReconnectMinMs.IntValue;
  g_WebSocket.MaxReconnectWait = g_CvarReconnectMaxMs.IntValue;
  g_WebSocket.PingInterval = 30;
  g_WebSocket.Connect();

  EmitLogEvent("info", "Connecting to Takaro WebSocket");
}

void OnTakaroOpen(WebSocket ws)
{
  g_Connected = true;
  g_ProtocolVersion = 0;
  PrintToServer("[TakaroTF2] [info] Takaro WebSocket connected");
  SendIdentify();
}

void OnTakaroClose(WebSocket ws, int code, const char[] reason)
{
  g_Connected = false;
  g_Identified = false;
  LogMessage("[TakaroTF2] Takaro WebSocket closed: code=%d reason=%s", code, reason);
  EmitLogEvent("warn", "Takaro WebSocket closed");
}

void OnTakaroError(WebSocket ws, const char[] errMsg)
{
  EmitLogEvent("error", errMsg);
}

void OnTakaroMessage(WebSocket ws, const JSON message, int wireSize)
{
  char type[64];
  if (!message.PtrTryGetString("/type", type, sizeof(type))) {
    SendErrorResponse("", "malformed-message", "Incoming message is missing type");
    return;
  }

  if (StrEqual(type, "ping", false)) {
    SendPong(message);
    return;
  }

  if (StrEqual(type, "connected", false)) {
    PrintToServer("[TakaroTF2] [info] Takaro WebSocket acknowledged");
    return;
  }

  if (StrEqual(type, "identifyResponse", false)) {
    char gameServerId[128];
    if (message.PtrTryGetString("/payload/gameServerId", gameServerId, sizeof(gameServerId))) {
      if (!g_Identified) {
        g_Identified = true;
        // An older Takaro sends no protocolVersion: that is version 0.
        int version = 0;
        if (message.PtrTryGetInt("/payload/protocolVersion", version) && version == 1 && g_OfferedV1) {
          g_ProtocolVersion = 1;
        } else {
          g_ProtocolVersion = 0;
        }
        g_LastAckTime = GetGameTime();
        PrintToServer("[TakaroTF2] [info] Takaro identity accepted: gameServerId=%s protocol=%d", gameServerId, g_ProtocolVersion);
        FlushOutbox();
      }
      return;
    }

    char identifyError[256];
    if (message.PtrTryGetString("/payload/error/message", identifyError, sizeof(identifyError))
      || message.PtrTryGetString("/payload/error", identifyError, sizeof(identifyError))
      || message.PtrTryGetString("/payload/message", identifyError, sizeof(identifyError))) {
      PrintToServer("[TakaroTF2] [error] Takaro identify failed: %s", identifyError);
      return;
    }

    EmitLogEvent("error", "Takaro identifyResponse did not include gameServerId");
    return;
  }

  if (StrEqual(type, "request", false)) {
    HandleTakaroRequest(message);
    return;
  }

  if (StrEqual(type, "eventAck", false)) {
    HandleEventAck(message);
    return;
  }

  if (StrEqual(type, "error", false)) {
    char takaroError[256];
    if (message.PtrTryGetString("/payload/message", takaroError, sizeof(takaroError))
      || message.PtrTryGetString("/message", takaroError, sizeof(takaroError))
      || message.PtrTryGetString("/payload/error/message", takaroError, sizeof(takaroError))) {
      PrintToServer("[TakaroTF2] [error] Takaro returned an error message: %s", takaroError);
    } else {
      PrintToServer("[TakaroTF2] [error] Takaro returned an error message");
    }
    return;
  }
}

void SendIdentify()
{
  JSONObject message = new JSONObject();
  message.SetString("type", "identify");

  JSONObject data = new JSONObject();

  char serverName[MAX_SERVER_NAME_LENGTH];
  g_CvarServerName.GetString(serverName, sizeof(serverName));
  data.SetString("name", serverName);

  char identityToken[MAX_TOKEN_LENGTH];
  char registrationToken[MAX_TOKEN_LENGTH];
  g_CvarIdentityToken.GetString(identityToken, sizeof(identityToken));
  g_CvarRegistrationToken.GetString(registrationToken, sizeof(registrationToken));

  data.SetString("identityToken", identityToken);
  data.SetString("registrationToken", registrationToken);

  g_OfferedV1 = g_CvarProtocol.IntValue >= 1;
  if (g_OfferedV1) {
    JSONArray versions = new JSONArray();
    versions.PushInt(0);
    versions.PushInt(1);
    data.Set("protocolVersions", versions);
    delete versions;

    JSONArray capabilities = BuildCapabilityManifest();
    data.Set("capabilities", capabilities);
    delete capabilities;
  }

  message.Set("payload", data);
  SendJson(message);
  delete message;
  delete data;
}

// Capability Manifest: only live-supported actions and events. Fallback answers (map tiles, giveItem)
// are not declared, so Takaro does not rely on them. Shutdown is declared only when its cvar allows it.
JSONArray BuildCapabilityManifest()
{
  static const char names[][] = {
    "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
    "items.list", "entities.list", "locations.list", "chat.send", "console.execute",
    "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans",
    "events.player-connected", "events.player-disconnected", "events.chat-message",
    "events.player-death", "events.entity-killed", "events.log"
  };

  JSONArray capabilities = new JSONArray();
  for (int i = 0; i < sizeof(names); i++) {
    JSONObject capability = new JSONObject();
    capability.SetString("name", names[i]);
    capabilities.Push(capability);
    delete capability;
  }
  if (g_CvarShutdownEnabled.BoolValue) {
    JSONObject capability = new JSONObject();
    capability.SetString("name", "server.shutdown");
    capabilities.Push(capability);
    delete capability;
  }
  return capabilities;
}

void SendPong(const JSON ping)
{
  JSONObject pong = new JSONObject();
  pong.SetString("type", "pong");

  char requestId[128];
  if (ping.PtrTryGetString("/requestId", requestId, sizeof(requestId))) {
    pong.SetString("requestId", requestId);
  }

  SendJson(pong);
  delete pong;
}

void HandleTakaroRequest(const JSON request)
{
  char requestId[128];
  if (!request.PtrTryGetString("/requestId", requestId, sizeof(requestId))) {
    requestId[0] = '\0';
  }

  char action[96];
  if (!request.PtrTryGetString("/action", action, sizeof(action))) {
    request.PtrTryGetString("/payload/action", action, sizeof(action));
  }

  if (action[0] == '\0') {
    SendErrorResponse(requestId, "missing-action", "Request has no action");
    return;
  }

  if (StrEqual(action, "testReachability")) {
    RespondSuccess(requestId, HandleTestReachability());
  } else if (StrEqual(action, "getPlayers")) {
    RespondSuccess(requestId, HandleGetPlayers());
  } else if (StrEqual(action, "getPlayer")) {
    HandleGetPlayer(requestId, request);
  } else if (StrEqual(action, "sendMessage")) {
    HandleSendMessage(requestId, request);
  } else if (StrEqual(action, "executeConsoleCommand")) {
    HandleExecuteConsoleCommand(requestId, request);
  } else if (StrEqual(action, "getPlayerLocation")) {
    HandleGetPlayerLocation(requestId, request);
  } else if (StrEqual(action, "teleportPlayer")) {
    HandleTeleportPlayer(requestId, request);
  } else if (StrEqual(action, "kickPlayer")) {
    HandleKickPlayer(requestId, request);
  } else if (StrEqual(action, "banPlayer")) {
    HandleBanPlayer(requestId, request);
  } else if (StrEqual(action, "unbanPlayer")) {
    HandleUnbanPlayer(requestId, request);
  } else if (StrEqual(action, "listBans")) {
    RespondSuccess(requestId, HandleListBans());
  } else if (StrEqual(action, "shutdown")) {
    HandleShutdown(requestId);
  } else if (StrEqual(action, "getPlayerInventory")) {
    HandleGetPlayerInventory(requestId, request);
  } else if (StrEqual(action, "listItems")) {
    RespondSuccess(requestId, HandleListItems());
  } else if (StrEqual(action, "listEntities")) {
    RespondSuccess(requestId, HandleListEntities());
  } else if (StrEqual(action, "listLocations")) {
    RespondSuccess(requestId, HandleListLocations());
  } else if (StrEqual(action, "getMapInfo")) {
    RespondSuccess(requestId, HandleGetMapInfo());
  } else if (StrEqual(action, "getMapTile")) {
    RespondString(requestId, "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lP8X2wAAAABJRU5ErkJggg==");
  } else if (StrEqual(action, "giveItem")) {
    SendErrorResponse(requestId, "unsupported-action", "This TF2 SourceMod connector does not support item grants.");
  } else {
    SendErrorResponse(requestId, "unknown-action", "Unknown Takaro action");
  }
}

JSONObject HandleTestReachability()
{
  JSONObject result = new JSONObject();
  result.SetBool("connectable", true);
  result.SetNull("reason");
  result.SetString("connector", "takaro_tf2");
  result.SetString("version", PLUGIN_VERSION);
  result.SetBool("websocketConnected", g_Connected);
  result.SetBool("identified", g_Identified);
  result.SetInt("maxPlayers", MaxClients);
  result.SetInt("onlinePlayers", CountTakaroClients());
  result.SetBool("includeBots", g_CvarIncludeBots.BoolValue);
  result.SetBool("shutdownEnabled", g_CvarShutdownEnabled.BoolValue);

  char map[128];
  GetCurrentMap(map, sizeof(map));
  result.SetString("map", map);

  JSONObject extensions = new JSONObject();
  extensions.SetString("websocket", "required: ProjectSky sm-ext-websocket");
  extensions.SetString("json", "required: ProjectSky sm-ext-json");
  result.Set("extensions", extensions);
  delete extensions;

  return result;
}

JSONArray HandleGetPlayers()
{
  JSONArray players = new JSONArray();

  for (int client = 1; client <= MaxClients; client++) {
    if (!IsTakaroClient(client)) {
      continue;
    }

    JSONObject player = BuildPlayerObject(client);
    players.Push(player);
    delete player;
  }

  return players;
}

void HandleGetPlayer(const char[] requestId, const JSON request)
{
  int client = ResolveClientFromRequest(request);
  if (client == 0) {
    SendErrorResponse(requestId, "player-not-found", "No online TF2 player matched the supplied identifier");
    return;
  }

  RespondSuccess(requestId, BuildPlayerObject(client));
}

void HandleSendMessage(const char[] requestId, const JSON request)
{
  char message[512];
  if (!ExtractStringArg(request, "message", message, sizeof(message))) {
    SendErrorResponse(requestId, "invalid-args", "sendMessage requires message");
    return;
  }

  char senderName[64];
  if (!ExtractSenderNameOverride(request, senderName, sizeof(senderName))) {
    strcopy(senderName, sizeof(senderName), "Takaro");
  }

  int recipient = ResolveRecipientFromRequest(request);
  if (recipient < 0) {
    // A private message to a player who is not online is refused, never broadcast.
    SendErrorResponse(requestId, "player-not-found", "The sendMessage recipient is not an online TF2 player");
    return;
  }
  if (recipient > 0) {
    PrintToChat(recipient, "\x04[%s]\x01 %s", senderName, message);
  } else {
    PrintToChatAll("\x04[%s]\x01 %s", senderName, message);
  }

  RespondNull(requestId);
}

void HandleExecuteConsoleCommand(const char[] requestId, const JSON request)
{
  char command[512];
  if (!ExtractStringArg(request, "command", command, sizeof(command))) {
    SendErrorResponse(requestId, "invalid-args", "executeConsoleCommand requires command");
    return;
  }

  TrimString(command);
  if (!IsCommandAllowed(command)) {
    SendErrorResponse(requestId, "command-not-allowed", "Command is not in takaro_tf2 allowlist");
    return;
  }

  // ServerCommandEx runs the command now and returns what it printed to the console.
  char output[4096];
  ServerCommandEx(output, sizeof(output), "%s", command);
  TrimString(output);

  JSONObject result = new JSONObject();
  result.SetBool("success", true);
  result.SetString("rawResult", output);
  result.SetNull("errorMessage");
  RespondSuccess(requestId, result);
}

void HandleGetPlayerLocation(const char[] requestId, const JSON request)
{
  int client = ResolveClientFromRequest(request);
  // Version 1: a player without a position (offline or dead) answers null.
  if (g_ProtocolVersion == 1 && (client == 0 || !IsPlayerAlive(client))) {
    RespondNull(requestId);
    return;
  }
  if (client == 0) {
    SendErrorResponse(requestId, "player-not-found", "No online TF2 player matched the supplied identifier");
    return;
  }

  float origin[3];
  if (IsPlayerAlive(client)) {
    GetClientAbsOrigin(client, origin);
  } else {
    origin[0] = 0.0;
    origin[1] = 0.0;
    origin[2] = 0.0;
  }

  JSONObject location = new JSONObject();
  location.SetInt("x", RoundToNearest(origin[0]));
  location.SetInt("y", RoundToNearest(origin[1]));
  location.SetInt("z", RoundToNearest(origin[2]));
  // Same dimension as listLocations: the current map.
  char map[PLATFORM_MAX_PATH];
  GetCurrentMap(map, sizeof(map));
  location.SetString("dimension", map);
  RespondSuccess(requestId, location);
}

void HandleGetPlayerInventory(const char[] requestId, const JSON request)
{
  int client = ResolveClientFromRequest(request);
  if (client == 0) {
    SendErrorResponse(requestId, "player-not-found", "No online TF2 player matched the supplied identifier");
    return;
  }

  JSONArray inventory = new JSONArray();
  for (int slot = 0; slot <= 5; slot++) {
    int weapon = GetPlayerWeaponSlot(client, slot);
    if (weapon <= MaxClients || !IsValidEntity(weapon)) {
      continue;
    }

    // Same code as listItems (the item definition index), so Takaro can match the item; display name from the catalog.
    int defIndex = GetEntProp(weapon, Prop_Send, "m_iItemDefinitionIndex");
    char code[16];
    IntToString(defIndex, code, sizeof(code));

    char name[128];
    if (!g_ItemNames.GetString(code, name, sizeof(name))) {
      GetEntityClassname(weapon, name, sizeof(name));
    }

    char quality[32];
    Format(quality, sizeof(quality), "slot_%d", slot);
    PushItemDTO(inventory, code, name, "Equipped TF2 weapon", quality, 1);
  }

  RespondSuccess(requestId, inventory);
}

void BuildItemNameLookup()
{
  // Filled while the generated catalog pushes its DTOs instead of reading the JSON array back:
  // the read-back found no names on a Linux server, so inventory fell back to classnames.
  g_ItemNames = new StringMap();
  JSONArray items = new JSONArray();
  g_RecordItemNames = true;
  PopulateGeneratedTf2ItemCatalog(items);
  g_RecordItemNames = false;
  delete items;
}

JSONArray HandleListItems()
{
  JSONArray items = new JSONArray();
  PopulateGeneratedTf2ItemCatalog(items);
  return items;
}

// The destructible TF2 entities a player can kill: Engineer buildings and the Halloween/MvM bosses.
// The code is the entity classname, which entity-killed reports as `entity`.
JSONArray HandleListEntities()
{
  static const char codes[][] = {
    "obj_sentrygun", "obj_dispenser", "obj_teleporter", "obj_attachment_sapper",
    "tank_boss", "headless_hatman", "eyeball_boss", "merasmus", "tf_zombie"
  };
  static const char names[][] = {
    "Sentry Gun", "Dispenser", "Teleporter", "Sapper",
    "Tank", "Horseless Headless Horsemann", "MONOCULUS", "Merasmus", "Skeleton"
  };
  static const char types[][] = {
    "neutral", "neutral", "neutral", "neutral",
    "hostile", "hostile", "hostile", "hostile", "hostile"
  };

  JSONArray entities = new JSONArray();
  for (int i = 0; i < sizeof(codes); i++) {
    JSONObject entity = new JSONObject();
    entity.SetString("code", codes[i]);
    entity.SetString("name", names[i]);
    entity.SetString("type", types[i]);
    entities.Push(entity);
    delete entity;
  }
  return entities;
}

public void Event_RoundStart(Event event, const char[] name, bool dontBroadcast)
{
  CacheFlagHomes();
}

// Flags are at home at round start; a carried or dropped flag moves, so listLocations uses this cache.
void CacheFlagHomes()
{
  int flag = -1;
  while ((flag = FindEntityByClassname(flag, "item_teamflag")) != -1) {
    int team = GetEntProp(flag, Prop_Data, "m_iTeamNum");
    if (team < 2 || team > 3 || g_FlagHomeSet[team]) {
      continue;
    }
    GetEntPropVector(flag, Prop_Data, "m_vecAbsOrigin", g_FlagHome[team]);
    g_FlagHomeSet[team] = true;
  }
}

// Locations come from the running map's own entities, so every map gets real coordinates:
// intel home positions (CTF), team spawn points and control points (CP/KOTH/A-D).
JSONArray HandleListLocations()
{
  JSONArray locations = new JSONArray();
  char map[128];
  GetCurrentMap(map, sizeof(map));

  static const char teamNames[][] = { "", "", "RED", "BLU" };
  char code[192];
  char name[192];
  float origin[3];

  CacheFlagHomes();
  for (int team = 2; team <= 3; team++) {
    if (!g_FlagHomeSet[team]) {
      continue;
    }
    Format(code, sizeof(code), "%s:%s_intel", map, teamNames[team]);
    StringToLower(code);
    Format(name, sizeof(name), "%s intelligence", teamNames[team]);
    PushLocationDTO(locations, code, name, map, RoundToFloor(g_FlagHome[team][0]), RoundToFloor(g_FlagHome[team][1]), RoundToFloor(g_FlagHome[team][2]), 256);
  }

  bool spawnFound[4];
  int spawn = -1;
  while ((spawn = FindEntityByClassname(spawn, "info_player_teamspawn")) != -1) {
    int team = GetEntProp(spawn, Prop_Data, "m_iTeamNum");
    if (team < 2 || team > 3 || spawnFound[team] || (HasEntProp(spawn, Prop_Data, "m_bDisabled") && GetEntProp(spawn, Prop_Data, "m_bDisabled"))) {
      continue;
    }
    spawnFound[team] = true;
    GetEntPropVector(spawn, Prop_Data, "m_vecAbsOrigin", origin);
    Format(code, sizeof(code), "%s:%s_spawn", map, teamNames[team]);
    StringToLower(code);
    Format(name, sizeof(name), "%s spawn", teamNames[team]);
    PushLocationDTO(locations, code, name, map, RoundToFloor(origin[0]), RoundToFloor(origin[1]), RoundToFloor(origin[2]), 256);
  }

  int point = -1;
  int index = 0;
  while ((point = FindEntityByClassname(point, "team_control_point")) != -1) {
    index++;
    GetEntPropVector(point, Prop_Data, "m_vecAbsOrigin", origin);
    name[0] = '\0';
    if (HasEntProp(point, Prop_Data, "m_iszPrintName")) {
      GetEntPropString(point, Prop_Data, "m_iszPrintName", name, sizeof(name));
    }
    if (name[0] == '\0' || name[0] == '#') {
      Format(name, sizeof(name), "Control point %d", index);
    }
    Format(code, sizeof(code), "%s:cp_%d", map, index);
    StringToLower(code);
    PushLocationDTO(locations, code, name, map, RoundToFloor(origin[0]), RoundToFloor(origin[1]), RoundToFloor(origin[2]), 256);
  }

  return locations;
}

void StringToLower(char[] text)
{
  for (int i = 0; text[i] != '\0'; i++) {
    text[i] = CharToLower(text[i]);
  }
}

JSONObject HandleGetMapInfo()
{
  JSONObject info = new JSONObject();
  info.SetBool("enabled", false);
  info.SetInt("mapBlockSize", 0);
  info.SetInt("maxZoom", 0);
  info.SetInt("mapSizeX", 0);
  info.SetInt("mapSizeY", 0);
  info.SetInt("mapSizeZ", 0);
  return info;
}

void HandleBanPlayer(const char[] requestId, const JSON request)
{
  char identity[64];
  int client = 0;
  if (!ExtractBanIdentity(request, identity, sizeof(identity), client)) {
    SendErrorResponse(requestId, "invalid-args", "banPlayer requires a real SteamID64 identity; SOURCE_*/BOT_* fallback ids are not safe to ban.");
    return;
  }

  char reason[256];
  if (!ExtractStringArg(request, "reason", reason, sizeof(reason))) {
    strcopy(reason, sizeof(reason), "Banned by Takaro");
  }

  char expiresAt[64];
  int minutes = ExtractBanMinutes(request, expiresAt, sizeof(expiresAt));
  if (minutes < 0) {
    SendErrorResponse(requestId, "invalid-args", "banPlayer expiresAt must be ISO 8601 UTC, for example 2031-12-31T23:59:59Z.");
    return;
  }

  char authId[64];
  if (!SteamId64ToSteam2(identity, authId, sizeof(authId))) {
    SendErrorResponse(requestId, "invalid-args", "banPlayer could not convert SteamID64 to Source engine Steam2 auth format.");
    return;
  }

  ServerCommand("banid %d %s", minutes, authId);
  ServerExecute();

  // Also drops a matching client that is still loading (not in game yet, so not resolved above).
  char name[MAX_NAME_LENGTH];
  name[0] = '\0';
  int connected = FindConnectedClientBySteamId64(identity);
  if (connected > 0) {
    GetClientName(connected, name, sizeof(name));
    KickClient(connected, "%s", reason);
  }

  // listBans must report the player's real name: Takaro's ban sync renames the profile to it.
  if (name[0] == '\0' && !g_LastSeenNames.GetString(identity, name, sizeof(name))
    && !ExtractStringArg(request, "player/name", name, sizeof(name))) {
    strcopy(name, sizeof(name), identity);
  }

  g_TakaroBans.SetString(identity, identity);
  g_TakaroBanAuthIds.SetString(identity, authId);
  g_TakaroBanReasons.SetString(identity, reason);
  g_TakaroBanExpiresAt.SetString(identity, expiresAt);
  g_TakaroBanNames.SetString(identity, name);
  SaveBans();
  RespondNull(requestId);
}

void HandleUnbanPlayer(const char[] requestId, const JSON request)
{
  char identity[64];
  int client = 0;
  if (!ExtractBanIdentity(request, identity, sizeof(identity), client)) {
    SendErrorResponse(requestId, "invalid-args", "unbanPlayer requires a real SteamID64 identity.");
    return;
  }

  char authId[64];
  if (!g_TakaroBanAuthIds.GetString(identity, authId, sizeof(authId)) && !SteamId64ToSteam2(identity, authId, sizeof(authId))) {
    SendErrorResponse(requestId, "invalid-args", "unbanPlayer could not convert SteamID64 to Source engine Steam2 auth format.");
    return;
  }

  ServerCommand("removeid %s", authId);
  // Rewrite banned_user.cfg so a permanent ban made outside Takaro is lifted for good, too.
  ServerCommand("writeid");
  ServerExecute();

  g_TakaroBans.Remove(identity);
  g_TakaroBanAuthIds.Remove(identity);
  g_TakaroBanReasons.Remove(identity);
  g_TakaroBanExpiresAt.Remove(identity);
  g_TakaroBanNames.Remove(identity);
  SaveBans();
  RespondNull(requestId);
}

JSONArray HandleListBans()
{
  JSONArray bans = new JSONArray();
  StringMapSnapshot snapshot = g_TakaroBans.Snapshot();

  for (int i = 0; i < snapshot.Length; i++) {
    char identity[64];
    snapshot.GetKey(i, identity, sizeof(identity));

    char reason[256];
    if (!g_TakaroBanReasons.GetString(identity, reason, sizeof(reason))) {
      strcopy(reason, sizeof(reason), "Banned by Takaro");
    }

    char expiresAt[64];
    if (!g_TakaroBanExpiresAt.GetString(identity, expiresAt, sizeof(expiresAt))) {
      strcopy(expiresAt, sizeof(expiresAt), "9999-12-31T23:59:59Z");
    }
    if (BanMinutesLeft(expiresAt) <= 0) {
      continue; // the engine already lifted it
    }

    char name[MAX_NAME_LENGTH];
    if (!g_TakaroBanNames.GetString(identity, name, sizeof(name)) && !g_LastSeenNames.GetString(identity, name, sizeof(name))) {
      strcopy(name, sizeof(name), identity);
    }

    PushBanDTO(bans, identity, name, reason, expiresAt);
  }

  delete snapshot;
  PushEngineBans(bans);
  return bans;
}

// Permanent bans made outside Takaro (sm_ban, banid + writeid) live in cfg/banned_user.cfg as
// "banid 0 STEAM_0:Y:Z". Report them too so Takaro's ban list matches what the server enforces.
void PushEngineBans(JSONArray bans)
{
  File file = OpenFile("cfg/banned_user.cfg", "r", true);
  if (file == null) {
    return;
  }

  char line[128];
  while (file.ReadLine(line, sizeof(line))) {
    TrimString(line);
    char parts[3][64];
    if (ExplodeString(line, " ", parts, sizeof(parts), sizeof(parts[])) != 3 || !StrEqual(parts[0], "banid", false)) {
      continue;
    }

    char identity[32];
    if (!Steam2ToSteamId64(parts[2], identity, sizeof(identity)) || g_TakaroBans.ContainsKey(identity)) {
      continue;
    }

    char name[MAX_NAME_LENGTH];
    if (!g_LastSeenNames.GetString(identity, name, sizeof(name))) {
      strcopy(name, sizeof(name), identity);
    }
    PushBanDTO(bans, identity, name, "Banned on the server", "9999-12-31T23:59:59Z");
  }
  delete file;
}

// STEAM_X:Y:Z -> SteamID64 (76561197960265728 + Z * 2 + Y), with decimal string addition
// because the result does not fit a 32-bit cell.
bool Steam2ToSteamId64(const char[] steam2, char[] steamId64, int maxlen)
{
  if (StrContains(steam2, "STEAM_", false) != 0) {
    return false;
  }
  char parts[3][16];
  if (ExplodeString(steam2[6], ":", parts, sizeof(parts), sizeof(parts[])) != 3) {
    return false;
  }
  int accountId = StringToInt(parts[2]) * 2 + StringToInt(parts[1]);
  if (accountId <= 0) {
    return false;
  }

  char result[18] = "76561197960265728";
  int carry = accountId;
  for (int i = 16; i >= 0 && carry > 0; i--) {
    int digit = (result[i] - '0') + (carry % 10);
    carry /= 10;
    if (digit >= 10) {
      digit -= 10;
      carry++;
    }
    result[i] = digit + '0';
  }
  if (carry > 0) {
    return false;
  }
  strcopy(steamId64, maxlen, result);
  return true;
}

void HandleTeleportPlayer(const char[] requestId, const JSON request)
{
  int client = ResolveClientFromRequest(request);
  if (client == 0) {
    SendErrorResponse(requestId, "player-not-found", "No online TF2 player matched the supplied identifier");
    return;
  }

  float target[3];
  if (!ExtractPosition(request, target)) {
    SendErrorResponse(requestId, "invalid-args", "teleportPlayer requires x, y, and z");
    return;
  }

  TeleportEntity(client, target, NULL_VECTOR, NULL_VECTOR);

  RespondNull(requestId);
}

void HandleKickPlayer(const char[] requestId, const JSON request)
{
  int client = ResolveClientFromRequest(request);
  if (client == 0) {
    SendErrorResponse(requestId, "player-not-found", "No online TF2 player matched the supplied identifier");
    return;
  }

  char reason[256];
  if (!ExtractStringArg(request, "reason", reason, sizeof(reason))) {
    strcopy(reason, sizeof(reason), "Kicked by Takaro");
  }

  KickClient(client, "%s", reason);

  RespondNull(requestId);
}

void HandleShutdown(const char[] requestId)
{
  if (!g_CvarShutdownEnabled.BoolValue) {
    SendErrorResponse(requestId, "shutdown-disabled", "Shutdown is disabled. Set takaro_tf2_shutdown_enabled 1 to allow it.");
    return;
  }

  RespondNull(requestId);

  ServerCommand("quit");
}

void RespondNull(const char[] requestId)
{
  JSONObject response = new JSONObject();
  response.SetString("type", "response");
  response.SetString("requestId", requestId);
  response.SetNull("payload");
  SendJson(response);
  delete response;
}

void RespondString(const char[] requestId, const char[] payload)
{
  JSONObject response = new JSONObject();
  response.SetString("type", "response");
  response.SetString("requestId", requestId);
  response.SetString("payload", payload);
  SendJson(response);
  delete response;
}

void RespondSuccess(const char[] requestId, JSON payload)
{
  JSONObject response = new JSONObject();
  response.SetString("type", "response");
  response.SetString("requestId", requestId);
  response.Set("payload", payload);
  SendJson(response);
  delete response;
  delete payload;
}

void SendErrorResponse(const char[] requestId, const char[] code, const char[] message)
{
  if (g_ProtocolVersion == 1) {
    SendErrorResponseV1(requestId, code, message);
    EmitLogEvent("warn", message);
    return;
  }

  JSONObject payload = new JSONObject();
  payload.SetBool("success", false);
  payload.SetString("errorCode", code);
  payload.SetString("message", message);
  payload.SetNull("payload");

  JSONObject response = new JSONObject();
  response.SetString("type", "response");
  response.SetString("requestId", requestId);
  response.Set("payload", payload);

  SendJson(response);
  delete response;
  delete payload;

  EmitLogEvent("warn", message);
}

// Version 1 answers failures as { error: { code, message } } with Takaro's standard codes. A refused console
// command is not a failure but a valid CommandOutput with success false.
void SendErrorResponseV1(const char[] requestId, const char[] code, const char[] message)
{
  JSONObject response = new JSONObject();
  response.SetString("type", "response");
  response.SetString("requestId", requestId);

  if (StrEqual(code, "command-not-allowed")) {
    JSONObject output = new JSONObject();
    output.SetBool("success", false);
    output.SetString("rawResult", "");
    output.SetString("errorMessage", message);
    response.Set("payload", output);
    delete output;
  } else {
    char v1Code[16];
    if (StrEqual(code, "unknown-action") || StrEqual(code, "unsupported-action") || StrEqual(code, "shutdown-disabled")) {
      strcopy(v1Code, sizeof(v1Code), "unsupported");
    } else if (StrEqual(code, "invalid-args") || StrEqual(code, "missing-action") || StrEqual(code, "malformed-message")) {
      strcopy(v1Code, sizeof(v1Code), "invalid_args");
    } else if (StrEqual(code, "player-not-found")) {
      strcopy(v1Code, sizeof(v1Code), "not_found");
    } else {
      strcopy(v1Code, sizeof(v1Code), "game_error");
    }

    JSONObject error = new JSONObject();
    error.SetString("code", v1Code);
    error.SetString("message", message);
    response.Set("error", error);
    delete error;
  }

  SendJson(response);
  delete response;
}

void EmitGameEvent(const char[] eventType, JSON data)
{
  char text[OUTBOX_DATA_BYTES];
  if (data.ToString(text, sizeof(text)) >= sizeof(text) - 1) {
    PrintToServer("[TakaroTF2] [warn] %s event dropped: payload larger than %d bytes", eventType, OUTBOX_DATA_BYTES);
    return;
  }

  if (g_OutboxSeq.Length >= OUTBOX_MAX_EVENTS) {
    RemoveOutboxEntry(0);
    PrintToServer("[TakaroTF2] [warn] event queue full; oldest unconfirmed event dropped");
  }

  g_OutboxSeq.Push(g_NextSeq++);
  g_OutboxType.PushString(eventType);
  g_OutboxData.PushString(text);

  if (g_Identified) {
    int index = g_OutboxSeq.Length - 1;
    SendOutboxEntry(index);
    if (g_ProtocolVersion == 0) {
      RemoveOutboxEntry(index);
    }
  }
}

void SendOutboxEntry(int index)
{
  if (g_WebSocket == null || !g_WebSocket.Connected) {
    return;
  }

  char eventType[32];
  char data[OUTBOX_DATA_BYTES];
  g_OutboxType.GetString(index, eventType, sizeof(eventType));
  g_OutboxData.GetString(index, data, sizeof(data));

  char frame[OUTBOX_DATA_BYTES + 192];
  if (g_ProtocolVersion == 1) {
    if (index == 0) {
      g_LastAckTime = GetEngineTime();
    }
    Format(frame, sizeof(frame), "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"%s\",\"data\":%s,\"streamId\":\"%s\",\"seq\":%d}}",
      eventType, data, g_StreamId, g_OutboxSeq.Get(index));
  } else {
    Format(frame, sizeof(frame), "{\"type\":\"gameEvent\",\"payload\":{\"type\":\"%s\",\"data\":%s}}", eventType, data);
  }
  g_WebSocket.WriteString(frame);
}

void RemoveOutboxEntry(int index)
{
  g_OutboxSeq.Erase(index);
  g_OutboxType.Erase(index);
  g_OutboxData.Erase(index);
}

// Sends everything queued before identify (and, on version 1, everything Takaro has not confirmed yet).
void FlushOutbox()
{
  for (int i = 0; i < g_OutboxSeq.Length; i++) {
    SendOutboxEntry(i);
  }
  if (g_ProtocolVersion == 0) {
    g_OutboxSeq.Clear();
    g_OutboxType.Clear();
    g_OutboxData.Clear();
  }
}

// Version 1 eventAck { streamId, upTo }: cumulative, drops every queued event with seq <= upTo.
void HandleEventAck(const JSON message)
{
  char streamId[48];
  if (!message.PtrTryGetString("/payload/streamId", streamId, sizeof(streamId)) || !StrEqual(streamId, g_StreamId)) {
    return;
  }

  int upTo;
  if (!message.PtrTryGetInt("/payload/upTo", upTo)) {
    char error[256];
    if (message.PtrTryGetString("/payload/error/message", error, sizeof(error))) {
      PrintToServer("[TakaroTF2] [warn] Takaro rejected an event: %s", error);
    }
    return;
  }

  while (g_OutboxSeq.Length > 0 && g_OutboxSeq.Get(0) <= upTo) {
    RemoveOutboxEntry(0);
  }
  g_LastAckTime = GetEngineTime();
}

// Version 1: no eventAck for ACK_TIMEOUT_SECONDS while events are pending -> reconnect, which resends them.
Action Timer_CheckEventAck(Handle timer)
{
  if (g_ProtocolVersion == 1 && g_Identified && g_OutboxSeq.Length > 0
    && GetEngineTime() - g_LastAckTime > ACK_TIMEOUT_SECONDS) {
    PrintToServer("[TakaroTF2] [warn] no eventAck for %.0f s; reconnecting to resend %d event(s)",
      ACK_TIMEOUT_SECONDS, g_OutboxSeq.Length);
    ConnectTakaro();
  }
  return Plugin_Continue;
}

void EmitLogEvent(const char[] level, const char[] message)
{
  JSONObject data = new JSONObject();
  data.SetString("msg", message);
  EmitGameEvent("log", data);
  delete data;
  PrintToServer("[TakaroTF2] [%s] %s", level, message);
}

void SendJson(JSON message)
{
  if (g_WebSocket == null || !g_WebSocket.Connected) {
    return;
  }

  g_WebSocket.WriteJSON(message);
}

void PushItemDTO(JSONArray items, const char[] code, const char[] name, const char[] description, const char[] quality, int amount)
{
  if (g_RecordItemNames) {
    g_ItemNames.SetString(code, name);
  }
  JSONObject item = new JSONObject();
  item.SetString("code", code);
  item.SetString("name", name);
  item.SetString("description", description);
  item.SetInt("amount", amount);
  item.SetString("quality", quality);
  items.Push(item);
  delete item;
}

void PushLocationDTO(JSONArray locations, const char[] code, const char[] name, const char[] dimension, int x, int y, int z, int radius)
{
  JSONObject location = new JSONObject();
  location.SetString("code", code);
  location.SetString("name", name);
  location.SetInt("radius", radius);

  JSONObject position = new JSONObject();
  position.SetInt("x", x);
  position.SetInt("y", y);
  position.SetInt("z", z);
  position.SetString("dimension", dimension);
  location.Set("position", position);
  delete position;

  JSONObject metadata = new JSONObject();
  metadata.SetString("source", "TF2 map entities");
  location.Set("metadata", metadata);
  delete metadata;

  locations.Push(location);
  delete location;
}

void PushBanDTO(JSONArray bans, const char[] identity, const char[] name, const char[] reason, const char[] expiresAt)
{
  JSONObject ban = new JSONObject();

  JSONObject player = new JSONObject();
  player.SetString("gameId", identity);
  player.SetString("steamId", identity);

  char platformId[80];
  Format(platformId, sizeof(platformId), "steam:%s", identity);
  player.SetString("platformId", platformId);

  player.SetString("name", name);

  ban.Set("player", player);
  delete player;

  ban.SetString("reason", reason);
  ban.SetString("expiresAt", expiresAt);
  bans.Push(ban);
  delete ban;
}

void GetTfObjectEntityName(int objectType, char[] buffer, int maxlen)
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

JSONObject BuildPlayerObject(int client)
{
  CacheClientIdentity(client);

  char gameId[64];
  char steamId[64];
  char platformId[80];
  ResolveClientIdentity(client, gameId, sizeof(gameId), steamId, sizeof(steamId), platformId, sizeof(platformId));

  char name[MAX_NAME_LENGTH];
  GetClientName(client, name, sizeof(name));

  JSONObject player = new JSONObject();
  SetPlayerIdentity(player, gameId, platformId);
  player.SetString("name", name);

  int ping = IsFakeClient(client) ? 0 : RoundToNearest(GetClientAvgLatency(client, NetFlow_Outgoing) * 1000.0);
  player.SetInt("ping", ping);

  return player;
}

bool IsTakaroClient(int client)
{
  if (client <= 0 || client > MaxClients) {
    return false;
  }

  if (!IsClientInGame(client)) {
    return false;
  }

  if (!g_CvarIncludeBots.BoolValue && IsFakeClient(client)) {
    return false;
  }

  return true;
}

void CacheClientIdentity(int client)
{
  char key[16];
  ClientKey(client, key, sizeof(key));

  char gameId[64];
  char steamId[64];
  char platformId[80];
  ResolveClientIdentity(client, gameId, sizeof(gameId), steamId, sizeof(steamId), platformId, sizeof(platformId));

  char name[MAX_NAME_LENGTH];
  GetClientName(client, name, sizeof(name));

  g_PlayerSteamIds.SetString(key, gameId);
  g_PlayerNames.SetString(key, name);
  g_PlayerPlatformIds.SetString(key, platformId);
  if (IsSteamId64(gameId)) {
    g_LastSeenNames.SetString(gameId, name);
  }
}

void ResolveClientIdentity(int client, char[] gameId, int gameIdMaxLen, char[] steamId, int steamIdMaxLen, char[] platformId, int platformIdMaxLen)
{
  if (GetClientSteamId64(client, steamId, steamIdMaxLen)) {
    strcopy(gameId, gameIdMaxLen, steamId);
    Format(platformId, platformIdMaxLen, "steam:%s", steamId);
    return;
  }

  // Bots and unauthenticated clients have no SteamID64, so steamId stays empty.
  steamId[0] = '\0';

  if (IsFakeClient(client)) {
    Format(gameId, gameIdMaxLen, "BOT_%d", client);
    Format(platformId, platformIdMaxLen, "tf2:bot_%d", client);
    return;
  }

  int userid = GetClientUserId(client);
  Format(gameId, gameIdMaxLen, "SOURCE_%d", userid);
  Format(platformId, platformIdMaxLen, "source:userid_%d", userid);
}

void SetPlayerIdentity(JSONObject player, const char[] gameId, const char[] platformId)
{
  player.SetString("gameId", gameId);
  if (IsSteamId64(gameId)) {
    player.SetString("steamId", gameId);
  }
  player.SetString("platformId", platformId);
}

bool GetClientSteamId64(int client, char[] steamId, int maxlen)
{
  if (!GetClientAuthId(client, AuthId_SteamID64, steamId, maxlen, true)) {
    steamId[0] = '\0';
    return false;
  }

  return steamId[0] != '\0';
}

void ClientKey(int client, char[] buffer, int maxlen)
{
  IntToString(GetClientUserId(client), buffer, maxlen);
}

int CountTakaroClients()
{
  int count = 0;
  for (int client = 1; client <= MaxClients; client++) {
    if (IsTakaroClient(client)) {
      count++;
    }
  }
  return count;
}

// 0 = no recipient (broadcast), -1 = a recipient that is not online, else the client index.
int ResolveRecipientFromRequest(const JSON request)
{
  // Pointers checked: /args/opts/recipient/gameId, steamId, platformId (and /payload/args/...).
  static const char fields[][] = { "gameId", "steamId", "platformId" };
  char identifier[128];
  char pointer[96];
  for (int i = 0; i < sizeof(fields); i++) {
    Format(pointer, sizeof(pointer), "opts/recipient/%s", fields[i]);
    // Takaro sends args either as an object or as a JSON-encoded string.
    if (PtrTryGetArg(request, "/args/", pointer, identifier, sizeof(identifier))
      || PtrTryGetArg(request, "/payload/args/", pointer, identifier, sizeof(identifier))
      || ExtractStringArgFromArgsString(request, "/args", pointer, identifier, sizeof(identifier))
      || ExtractStringArgFromArgsString(request, "/payload/args", pointer, identifier, sizeof(identifier))) {
      int client = ResolveClientByIdentifier(identifier);
      return client > 0 ? client : -1;
    }
  }
  return 0;
}

bool PtrTryGetArg(const JSON request, const char[] prefix, const char[] path, char[] buffer, int maxlen)
{
  char pointer[160];
  Format(pointer, sizeof(pointer), "%s%s", prefix, path);
  return request.PtrTryGetString(pointer, buffer, maxlen);
}

bool ExtractSenderNameOverride(const JSON request, char[] buffer, int maxlen)
{
  // Takaro sends args either as an object or as a JSON-encoded string.
  bool found = request.PtrTryGetString("/args/opts/senderNameOverride", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/opts/senderNameOverride", buffer, maxlen)
    || ExtractStringArgFromArgsString(request, "/args", "opts/senderNameOverride", buffer, maxlen)
    || ExtractStringArgFromArgsString(request, "/payload/args", "opts/senderNameOverride", buffer, maxlen);

  if (!found) {
    buffer[0] = '\0';
    return false;
  }

  TrimString(buffer);
  return buffer[0] != '\0';
}

int ResolveClientFromRequest(const JSON request)
{
  char identifier[128];
  if (!ExtractPlayerIdentifier(request, identifier, sizeof(identifier))) {
    return 0;
  }

  return ResolveClientByIdentifier(identifier);
}

bool ExtractPlayerIdentifier(const JSON request, char[] buffer, int maxlen)
{
  if (request.PtrTryGetString("/gameId", buffer, maxlen)
    || request.PtrTryGetString("/steamId", buffer, maxlen)
    || request.PtrTryGetString("/platformId", buffer, maxlen)
    || request.PtrTryGetString("/name", buffer, maxlen)
    || request.PtrTryGetString("/args/gameId", buffer, maxlen)
    || request.PtrTryGetString("/args/steamId", buffer, maxlen)
    || request.PtrTryGetString("/args/platformId", buffer, maxlen)
    || request.PtrTryGetString("/args/name", buffer, maxlen)
    || request.PtrTryGetString("/args/player/gameId", buffer, maxlen)
    || request.PtrTryGetString("/args/player/steamId", buffer, maxlen)
    || request.PtrTryGetString("/args/player/platformId", buffer, maxlen)
    || request.PtrTryGetString("/args/player/name", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/gameId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/steamId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/platformId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/name", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/player/gameId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/player/steamId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/player/platformId", buffer, maxlen)
    || request.PtrTryGetString("/payload/args/player/name", buffer, maxlen)) {
    return buffer[0] != '\0';
  }

  if (ExtractPlayerIdentifierFromArgsString(request, "/args", buffer, maxlen)
    || ExtractPlayerIdentifierFromArgsString(request, "/payload/args", buffer, maxlen)) {
    return buffer[0] != '\0';
  }

  return false;
}

int FindConnectedClientBySteamId64(const char[] steamId)
{
  char clientSteamId[32];
  for (int client = 1; client <= MaxClients; client++) {
    if (IsClientConnected(client) && !IsFakeClient(client)
      && GetClientAuthId(client, AuthId_SteamID64, clientSteamId, sizeof(clientSteamId), false)
      && StrEqual(clientSteamId, steamId)) {
      return client;
    }
  }
  return 0;
}

bool ExtractBanIdentity(const JSON request, char[] identity, int maxlen, int &client)
{
  client = 0;

  char rawIdentifier[128];
  if (!ExtractPlayerIdentifier(request, rawIdentifier, sizeof(rawIdentifier))) {
    return false;
  }

  client = ResolveClientByIdentifier(rawIdentifier);
  if (client > 0 && GetClientSteamId64(client, identity, maxlen)) {
    return true;
  }

  StripSteamPlatformPrefix(rawIdentifier, sizeof(rawIdentifier));
  if (!IsSteamId64(rawIdentifier)) {
    identity[0] = '\0';
    return false;
  }

  strcopy(identity, maxlen, rawIdentifier);
  return true;
}

bool IsSteamId64(const char[] value)
{
  int length = strlen(value);
  if (length != 17) {
    return false;
  }

  if (StrContains(value, "7656", false) != 0) {
    return false;
  }

  for (int i = 0; i < length; i++) {
    if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }

  return true;
}

bool SteamId64ToSteam2(const char[] steamId64, char[] authId, int maxlen)
{
  if (!IsSteamId64(steamId64)) {
    authId[0] = '\0';
    return false;
  }

  char base[18] = "76561197960265728";
  char accountDigits[18];
  accountDigits[17] = '\0';
  int borrow = 0;

  for (int i = 16; i >= 0; i--) {
    int digit = steamId64[i] - base[i] - borrow;
    if (digit < 0) {
      digit += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    accountDigits[i] = digit + '0';
  }

  if (borrow != 0) {
    authId[0] = '\0';
    return false;
  }

  int start = 0;
  while (start < 16 && accountDigits[start] == '0') {
    start++;
  }

  char accountString[18];
  strcopy(accountString, sizeof(accountString), accountDigits[start]);
  int accountId = StringToInt(accountString);
  if (accountId <= 0) {
    authId[0] = '\0';
    return false;
  }

  int y = accountId % 2;
  int z = accountId / 2;
  Format(authId, maxlen, "STEAM_1:%d:%d", y, z);
  return true;
}

int ExtractBanMinutes(const JSON request, char[] expiresAt, int maxlen)
{
  if (!ExtractStringArg(request, "expiresAt", expiresAt, maxlen) || expiresAt[0] == '\0') {
    strcopy(expiresAt, maxlen, "9999-12-31T23:59:59Z");
    return 0;
  }
  if (IsPermanentBan(expiresAt)) {
    return 0;
  }

  int minutes = BanMinutesLeft(expiresAt);
  return minutes > 0 ? minutes : -1;
}

bool IsPermanentBan(const char[] expiresAt)
{
  return StrContains(expiresAt, "9999-") == 0;
}

// Minutes until an ISO 8601 UTC time (Takaro sends "...T10:50:00.000Z"); a permanent ban never runs out.
int BanMinutesLeft(const char[] expiresAt)
{
  if (IsPermanentBan(expiresAt)) {
    return 2147483647;
  }

  int expiresAtUnix = ParseIsoUtc(expiresAt);
  if (expiresAtUnix <= 0) {
    return -1;
  }
  return (expiresAtUnix - GetTime() + 59) / 60;
}

// SourceMod's ParseTime reads the string as UTC (verified live: adding the local offset made a
// 33-minute ban last 153 minutes on a UTC+2 server).
int ParseIsoUtc(const char[] iso)
{
  if (strlen(iso) < 19) {
    return -1;
  }
  char head[20];
  strcopy(head, sizeof(head), iso);
  return ParseTime(head, "%Y-%m-%dT%H:%M:%S");
}

// The ban ledger is kept in data/takaro_tf2_bans.txt so listBans and the engine ban survive a restart
// (banid alone only lives in memory).
void SaveBans()
{
  KeyValues kv = new KeyValues("takaro_tf2_bans");
  StringMapSnapshot snapshot = g_TakaroBans.Snapshot();
  char identity[64];
  char value[256];
  for (int i = 0; i < snapshot.Length; i++) {
    snapshot.GetKey(i, identity, sizeof(identity));
    kv.JumpToKey(identity, true);
    if (g_TakaroBanAuthIds.GetString(identity, value, sizeof(value))) kv.SetString("authId", value);
    if (g_TakaroBanReasons.GetString(identity, value, sizeof(value))) kv.SetString("reason", value);
    if (g_TakaroBanExpiresAt.GetString(identity, value, sizeof(value))) kv.SetString("expiresAt", value);
    if (g_TakaroBanNames.GetString(identity, value, sizeof(value))) kv.SetString("name", value);
    kv.GoBack();
  }
  delete snapshot;

  char path[PLATFORM_MAX_PATH];
  BuildPath(Path_SM, path, sizeof(path), "data/takaro_tf2_bans.txt");
  kv.Rewind();
  kv.ExportToFile(path);
  delete kv;
}

// Loads the ledger and re-applies every ban that has not run out yet.
void LoadAndApplyBans()
{
  char path[PLATFORM_MAX_PATH];
  BuildPath(Path_SM, path, sizeof(path), "data/takaro_tf2_bans.txt");
  if (!FileExists(path)) {
    return;
  }

  KeyValues kv = new KeyValues("takaro_tf2_bans");
  if (!kv.ImportFromFile(path) || !kv.GotoFirstSubKey()) {
    delete kv;
    return;
  }

  char identity[64];
  char authId[64];
  char reason[256];
  char expiresAt[64];
  char name[MAX_NAME_LENGTH];
  do {
    kv.GetSectionName(identity, sizeof(identity));
    kv.GetString("authId", authId, sizeof(authId));
    kv.GetString("reason", reason, sizeof(reason), "Banned by Takaro");
    kv.GetString("expiresAt", expiresAt, sizeof(expiresAt), "9999-12-31T23:59:59Z");
    kv.GetString("name", name, sizeof(name), identity);

    int minutes = BanMinutesLeft(expiresAt);
    if (authId[0] == '\0' || minutes <= 0) {
      continue;
    }

    ServerCommand("banid %d %s", IsPermanentBan(expiresAt) ? 0 : minutes, authId);
    g_TakaroBans.SetString(identity, identity);
    g_TakaroBanAuthIds.SetString(identity, authId);
    g_TakaroBanReasons.SetString(identity, reason);
    g_TakaroBanExpiresAt.SetString(identity, expiresAt);
    g_TakaroBanNames.SetString(identity, name);
  } while (kv.GotoNextKey());
  delete kv;

  SaveBans(); // drops expired entries from the file
}

int ResolveClientByIdentifier(const char[] rawIdentifier)
{
  char identifier[128];
  strcopy(identifier, sizeof(identifier), rawIdentifier);
  StripSteamPlatformPrefix(identifier, sizeof(identifier));
  if (identifier[0] == '\0') {
    return 0;
  }

  for (int client = 1; client <= MaxClients; client++) {
    if (!IsTakaroClient(client)) {
      continue;
    }

    char gameId[64];
    char steamId[64];
    char platformId[80];
    ResolveClientIdentity(client, gameId, sizeof(gameId), steamId, sizeof(steamId), platformId, sizeof(platformId));

    if (StrEqual(gameId, identifier, false)
      || (steamId[0] != '\0' && StrEqual(steamId, identifier, false))
      || StrEqual(platformId, rawIdentifier, false)) {
      return client;
    }

    if (IsFakeClient(client)) {
      char botId[64];
      Format(botId, sizeof(botId), "BOT_%d", client);
      if (StrEqual(botId, identifier, false)) {
        return client;
      }
    }

    char name[MAX_NAME_LENGTH];
    GetClientName(client, name, sizeof(name));
    if (StrEqual(name, rawIdentifier, false)) {
      return client;
    }
  }

  return 0;
}

void StripSteamPlatformPrefix(char[] value, int maxlen)
{
  if (StrContains(value, "steam:", false) == 0) {
    char stripped[128];
    strcopy(stripped, sizeof(stripped), value[6]);
    strcopy(value, maxlen, stripped);
  }
}

bool ExtractStringArg(const JSON request, const char[] key, char[] buffer, int maxlen)
{
  char pointer[128];

  Format(pointer, sizeof(pointer), "/%s", key);
  if (request.PtrTryGetString(pointer, buffer, maxlen)) {
    return true;
  }

  Format(pointer, sizeof(pointer), "/args/%s", key);
  if (request.PtrTryGetString(pointer, buffer, maxlen)) {
    return true;
  }

  Format(pointer, sizeof(pointer), "/payload/args/%s", key);
  if (request.PtrTryGetString(pointer, buffer, maxlen)) {
    return true;
  }

  if (ExtractStringArgFromArgsString(request, "/args", key, buffer, maxlen)
    || ExtractStringArgFromArgsString(request, "/payload/args", key, buffer, maxlen)) {
    return true;
  }

  return false;
}

bool ExtractPosition(const JSON request, float target[3])
{
  float x;
  float y;
  float z;

  if ((request.PtrTryGetFloat("/x", x) || request.PtrTryGetFloat("/args/x", x) || request.PtrTryGetFloat("/payload/args/x", x) || request.PtrTryGetFloat("/args/position/x", x) || request.PtrTryGetFloat("/payload/args/position/x", x))
    && (request.PtrTryGetFloat("/y", y) || request.PtrTryGetFloat("/args/y", y) || request.PtrTryGetFloat("/payload/args/y", y) || request.PtrTryGetFloat("/args/position/y", y) || request.PtrTryGetFloat("/payload/args/position/y", y))
    && (request.PtrTryGetFloat("/z", z) || request.PtrTryGetFloat("/args/z", z) || request.PtrTryGetFloat("/payload/args/z", z) || request.PtrTryGetFloat("/args/position/z", z) || request.PtrTryGetFloat("/payload/args/position/z", z))) {
    target[0] = x;
    target[1] = y;
    target[2] = z;
    return true;
  }

  if (ExtractPositionFromArgsString(request, "/args", target)
    || ExtractPositionFromArgsString(request, "/payload/args", target)) {
    return true;
  }

  return false;
}

bool ExtractStringArgFromArgsString(const JSON request, const char[] pointer, const char[] key, char[] buffer, int maxlen)
{
  char rawArgs[MAX_TAKARO_MESSAGE_LENGTH];
  if (!request.PtrTryGetString(pointer, rawArgs, sizeof(rawArgs))) {
    return false;
  }

  JSON args = JSON.Parse(rawArgs, false, false, JSON_READ_STOP_WHEN_DONE);
  if (args == null) {
    return false;
  }

  char keyPointer[128];
  Format(keyPointer, sizeof(keyPointer), "/%s", key);
  bool found = args.PtrTryGetString(keyPointer, buffer, maxlen);
  delete args;
  return found;
}

bool ExtractPlayerIdentifierFromArgsString(const JSON request, const char[] pointer, char[] buffer, int maxlen)
{
  char rawArgs[MAX_TAKARO_MESSAGE_LENGTH];
  if (!request.PtrTryGetString(pointer, rawArgs, sizeof(rawArgs))) {
    return false;
  }

  JSON args = JSON.Parse(rawArgs, false, false, JSON_READ_STOP_WHEN_DONE);
  if (args == null) {
    return false;
  }

  bool found = args.PtrTryGetString("/gameId", buffer, maxlen)
    || args.PtrTryGetString("/steamId", buffer, maxlen)
    || args.PtrTryGetString("/platformId", buffer, maxlen)
    || args.PtrTryGetString("/name", buffer, maxlen)
    || args.PtrTryGetString("/player/gameId", buffer, maxlen)
    || args.PtrTryGetString("/player/steamId", buffer, maxlen)
    || args.PtrTryGetString("/player/platformId", buffer, maxlen)
    || args.PtrTryGetString("/player/name", buffer, maxlen);

  delete args;
  return found && buffer[0] != '\0';
}

bool ExtractPositionFromArgsString(const JSON request, const char[] pointer, float target[3])
{
  char rawArgs[MAX_TAKARO_MESSAGE_LENGTH];
  if (!request.PtrTryGetString(pointer, rawArgs, sizeof(rawArgs))) {
    return false;
  }

  JSON args = JSON.Parse(rawArgs, false, false, JSON_READ_STOP_WHEN_DONE);
  if (args == null) {
    return false;
  }

  float x;
  float y;
  float z;
  bool found = (args.PtrTryGetFloat("/x", x) || args.PtrTryGetFloat("/position/x", x))
    && (args.PtrTryGetFloat("/y", y) || args.PtrTryGetFloat("/position/y", y))
    && (args.PtrTryGetFloat("/z", z) || args.PtrTryGetFloat("/position/z", z));

  if (found) {
    target[0] = x;
    target[1] = y;
    target[2] = z;
  }

  delete args;
  return found;
}

bool IsCommandAllowed(const char[] command)
{
  if (command[0] == '\0') {
    return false;
  }

  char exactPolicy[MAX_COMMAND_POLICY_LENGTH];
  g_CvarCommandAllowlistExact.GetString(exactPolicy, sizeof(exactPolicy));
  if (PolicyContainsExact(exactPolicy, command)) {
    return true;
  }

  char prefixPolicy[MAX_COMMAND_POLICY_LENGTH];
  g_CvarCommandAllowlistPrefixes.GetString(prefixPolicy, sizeof(prefixPolicy));
  if (PolicyContainsPrefix(prefixPolicy, command)) {
    return true;
  }

  return false;
}

bool PolicyContainsExact(const char[] policy, const char[] command)
{
  char entries[32][128];
  int count = ExplodeString(policy, ";", entries, sizeof(entries), sizeof(entries[]));

  for (int i = 0; i < count; i++) {
    TrimString(entries[i]);
    if (entries[i][0] != '\0' && StrEqual(entries[i], command, false)) {
      return true;
    }
  }

  return false;
}

bool PolicyContainsPrefix(const char[] policy, const char[] command)
{
  char entries[32][128];
  int count = ExplodeString(policy, ";", entries, sizeof(entries), sizeof(entries[]));

  for (int i = 0; i < count; i++) {
    TrimString(entries[i]);
    if (entries[i][0] == '\0') {
      continue;
    }

    if (StrContains(command, entries[i], false) == 0) {
      return true;
    }
  }

  return false;
}
