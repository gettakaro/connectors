-- Coverage registry: one record per Takaro action and event. capabilities.json is generated from this
-- file (scripts/gen-capabilities.lua) and tests/test_coverage.lua fails when the two drift.
-- Statuses: live-supported | schema-fallback | unsupported. Only a live run may promote to live-supported.
local M = {}

M.ACTIONS = {
  "getPlayer", "getPlayers", "getPlayerLocation", "getPlayerInventory", "giveItem", "listItems", "listEntities",
  "listLocations", "executeConsoleCommand", "sendMessage", "teleportPlayer", "kickPlayer", "banPlayer",
  "unbanPlayer", "listBans", "testReachability", "shutdown", "getMapInfo", "getMapTile",
}

M.EVENTS = { "player-connected", "player-disconnected", "chat-message", "player-death", "entity-killed", "log" }

M.STATUSES = { ["live-supported"] = true, ["schema-fallback"] = true, ["unsupported"] = true }

local function live(what) return "live 2026-10-13 on SCUM 1.3.4.2 through Takaro: " .. what end
local NO_LIVE = "not live-tested; fake-Takaro harness only"
local function live4(what) return "live 2026-10-14 on SCUM 1.3.4.2 through Takaro: " .. what end

M.actions = {
  getPlayer = {
    status = "live-supported",
    responseShape = "IGamePlayer { gameId, name, steamId, platformId, ip?, ping? }",
    liveVerification = live("getPlayer frame for a SteamID64 player with ip"),
    reason = "ConZPlayerController:GetUserId/GetUserProfileName; offline players answer the last-known record.",
  },
  getPlayers = {
    status = "live-supported",
    responseShape = "IGamePlayer[] (bare array)",
    liveVerification = live("[] with nobody online, then the test player with steamId/platformId steam:<id>/ip 127.0.0.1/ping"),
    reason = "FindAllOf(ConZPlayerController) with a SteamID64; IP from the login log, ping = PlayerState.Ping * 4.",
  },
  getPlayerLocation = {
    status = "live-supported",
    responseShape = "{ x, y, z } (SCUM world units, cm)",
    liveVerification = live("x/y/z frames; values match the post-teleport position read in game"),
    reason = "Prisoner K2_GetActorLocation; offline or dead players answer the last position seen.",
  },
  getPlayerInventory = {
    status = "live-supported",
    responseShape = "IItemDTO[] { code, name, amount, quality } (bare array; quality = durability %)",
    liveVerification = live4("3 client comparisons: worn clothes, backpack/holster contents, item in hands, "
      .. "stacks 13x/20x/4x, durability 50%/75%/99%; Takaro history + player-inventory-changed match"),
    reason = "SCUM's inventory is not reflected; the prisoner's AActor.Children tree holds every carried item (worn, "
      .. "hands, container contents, nested). amount = UStackableItemComponent._repQuantity for stacks, quality = "
      .. "GetHealth/GetMaxHealth in whole percent. A failed read answers an error, never [] (Takaro would record "
      .. "removals). Rounds loaded in a weapon or magazine are not items and are not listed.",
  },
  giveItem = {
    status = "live-supported",
    responseShape = "{ success, rawResult }",
    liveVerification = live4("2 Apples into shirt+pants pockets, Katana into holster 1, 5 x .50 AE merged into one stack "
      .. "(5x -> 10x), full pockets -> Apple into empty hands; shop deliveries in the inventory (client screenshots)"),
    reason = "Loads the Item primary asset by code, spawns it at the player; clothes go onto a free body slot "
      .. "(Server_CharacterInventoryComponent_EquipClothes), quality (1-100) sets the durability; else the controller's "
      .. "InventoryUserComponent:Server_CharacterInventoryComponent_PickupItem(prisoner inventory, item) = the player's own "
      .. "pickup; read back by Owner chain (stackables merge: stack quantity 0 / destroyed = delivered). Items that fit nowhere: "
      .. "the first goes into empty hands, the rest stays at the player's feet; rawResult reports the split.",
  },
  listItems = {
    status = "live-supported",
    responseShape = "IItemDTO[] { code, name, description } (bare array)",
    liveVerification = live("2854 items in Takaro itemSearch with human names (TEC01 M9, M9 Magazine, Tooth Necklace)"),
    reason = "All 'Item' primary assets; names from the CDO GetInteractableName, descriptions from GetDescription. "
      .. "Built once on the game thread under a 4 ms/tick budget (~3.5 min, single loads up to ~1 s) and cached per "
      .. "engine build in items-cache.json; 26 codes without an engine name get a humanized one.",
  },
  listEntities = {
    status = "live-supported",
    responseShape = "IEntityDTO[] { code, name, type } (bare array)",
    liveVerification = live("65 entities in Takaro entitySearch (Bear hostile, Deer neutral, Drifter Lvl 1 hostile)"),
    reason = "Zombie, ArmedNPC and Animal primary assets. Animals use SpeciesName; zombies and NPCs have no engine "
      .. "display name, so their names are humanized from the asset code (Zombie Civilian, Drifter Lvl 1).",
  },
  listLocations = {
    status = "schema-fallback",
    responseShape = "[] (bare array)",
    liveVerification = NO_LIVE,
    reason = "SCUM exposes no named location registry on the server; the connector answers [] and does not declare "
      .. "locations.list on protocol 1.",
  },
  executeConsoleCommand = {
    status = "live-supported",
    responseShape = "{ success, rawResult, errorMessage? }",
    liveVerification = live("command_not_allowed refusal; 'announce' with nobody online; 'location'/'ListPlayers' "
      .. "run through the online admin (admin log line, output in the admin's chat)"),
    reason = "COMMAND_ALLOWLIST gate. SCUM runs admin commands only through a player controller (the only "
      .. "IAdminCommandExecutor), so allowlisted verbs go through an online admin's PlayerRpcChannel:"
      .. "Chat_Server_ProcessAdminCommand (output only in that admin's chat) and fail with no_executor when no admin "
      .. "is online. announce and listplayers are implemented natively and work with nobody online. A server-owned "
      .. "executor is not possible from UE4SS Lua: SCUM ignores a server-spawned controller (and crashed ~60 s later), "
      .. "and running through a non-admin's channel would act in that player's name.",
  },
  sendMessage = {
    status = "live-supported",
    responseShape = "{ success }",
    liveVerification = live("directed and global lines seen in the client chat as 'Takaro: ...'"),
    reason = "MiscStatics:BroadcastChatLine (global) and MiscStatics:SendChatLineToPlayer (directed), EChatType "
      .. "ServerMessage; senderNameOverride becomes a 'Name: ' prefix.",
  },
  teleportPlayer = {
    status = "live-supported",
    responseShape = "{ success, rawResult }",
    liveVerification = live("player moved 20 m (client view changed, server position matches); +300 m caused a fall"),
    reason = "AConZPlayerController:Teleport(FTeleportRequest) with K2_TeleportTo as fallback.",
  },
  kickPlayer = {
    status = "live-supported",
    responseShape = "{ success }",
    liveVerification = live4("reason line 'You are kicked: …' in the client chat, KickPlayer 2 s later, kick dialog"),
    reason = "The reason is sent to the player's chat first (SCUM's kick screen has no reason text), then "
      .. "AConZGameMode:KickPlayer(controller, reason) 2 s later; Takaro gets the answer after the kick.",
  },
  banPlayer = {
    status = "live-supported",
    responseShape = "{ success }",
    liveVerification = live4("online: reason line then kick; rejoin refused 0.65 s after the join request, before spawn; "
      .. "4-minute ban lifted by the connector at expiresAt, rejoin without unban"),
    reason = "Writes the SteamID64 to BannedUsers.ini plus takaro-bans.json (reason, expiresAt, Takaro's name). The "
      .. "connector enforces it: a banned id is kicked as soon as its PlayerController exists (1 Hz poll), before the "
      .. "prisoner spawns. expiresAt is lifted by the connector (Takaro sends no unban), also after downtime.",
  },
  unbanPlayer = {
    status = "live-supported",
    responseShape = "{ success }",
    liveVerification = live4("unban of a ban SCUM had loaded at server start: rejoin worked 1 min later, no restart"),
    reason = "Removes the SteamID64 from BannedUsers.ini and the ban store at once; works offline. For a ban that was in "
      .. "the file at server start #UnbanPlayer also runs through the next online admin.",
  },
  listBans = {
    status = "live-supported",
    responseShape = "IBanDTO[] { player, reason, expiresAt? } (bare array)",
    liveVerification = live("Takaro syncBans created a ban from a BannedUsers.ini entry"),
    reason = "BannedUsers.ini plus the connector's ban store; reason always a string (Takaro BanDTO requires it).",
  },
  testReachability = {
    status = "live-supported",
    responseShape = "{ connectable, reason }",
    liveVerification = live("connectable true via REST /gameserver/{id}/reachability"),
    reason = "Checks the live world (ConZGameMode and MiscStatics found).",
  },
  shutdown = {
    status = "live-supported",
    responseShape = "{ success, errorMessage? }",
    liveVerification = live("refused by default; with ENABLE_DESTRUCTIVE_COMMANDS=true answered success and the "
      .. "server exited cleanly 15 s later"),
    reason = "Gated by ENABLE_DESTRUCTIVE_COMMANDS; KismetSystemLibrary:QuitGame after the response is sent.",
  },
  getMapInfo = {
    status = "schema-fallback",
    responseShape = "{ enabled: false, mapBlockSize: 0, maxZoom: 0, mapSizeX: 0, mapSizeY: 0, mapSizeZ: 0 }",
    liveVerification = NO_LIVE,
    reason = "Protocol 0 only (Takaro does not ask for map info on protocol 1, where the connector declares the map "
      .. "capability instead: Dimension the_island, source none, bounds from the server's ConZWorldSettings.WorldBounds); "
      .. "a disabled map DTO keeps validation happy.",
  },
  getMapTile = {
    status = "unsupported",
    responseShape = "{ success: false, errorMessage }",
    liveVerification = NO_LIVE,
    reason = "The server has no map art to render tiles from; on protocol 1 the map Dimension is source none and the "
      .. "operator uploads the in-game map image in Takaro.",
  },
}

M.events = {
  ["player-connected"] = {
    status = "live-supported",
    payloadShape = "{ player: IGamePlayer }",
    liveVerification = live("the test player with steamId, platformId steam:<id>, ip; Takaro player-connected record"),
    reason = "SCUM login log line (has IP) plus getPlayers reconciliation; dedup by online state (~1 s log latency). "
      .. "A Lua reload marks players already online silently (no duplicate, live 2026-10-14). A banned login is "
      .. "refused, not announced.",
  },
  ["player-disconnected"] = {
    status = "live-supported",
    payloadShape = "{ player: IGamePlayer }",
    liveVerification = live("after kick and ban kick; Takaro record"),
    reason = "SCUM login log 'logged out' line (partial: log-derived, ~1 s latency).",
  },
  ["chat-message"] = {
    status = "live-supported",
    payloadShape = "{ player: IGamePlayer, channel: global|team|friends, msg }",
    liveVerification = live4("LOCAL line -> channel friends, GLOBAL line -> global"),
    reason = "Hook on PlayerRpcChannel:Chat_Server_BroadcastChatMessage (Global/Admin -> global, Local (proximity) -> "
      .. "friends, Squad -> team); "
      .. "the chat log is a fallback, deduplicated. #admin commands are not chat. Server messages have no player, so "
      .. "no echo.",
  },
  ["player-death"] = {
    status = "live-supported",
    payloadShape = "{ player: IGamePlayer, attacker?: IGamePlayer, position?: { x, y, z } }",
    liveVerification = live("fall death after a +300 m teleport: player-death with position; Takaro record"),
    reason = "AConZCharacter:IsAlive polled at 1 Hz on the existing prisoner (a kick/logout without a prisoner is not a "
      .. "death); PvP kills from the kill log add the attacker (kill-log path unit-tested, not yet seen live: needs a "
      .. "second player).",
  },
  ["entity-killed"] = {
    status = "live-supported",
    payloadShape = "{ player: IGamePlayer, entity: string, weapon: string }",
    liveVerification = live4("spawned zombies killed with a Katana (melee) and a DEagle 50 (firearm) -> entity 'Zombie "
      .. "Civilian Skinny Male', weapon 'Katana' / 'DEagle 50'"),
    reason = "Attack-RPC hooks (projectile damage, melee capsule hit, base-damage, explosive) record which player "
      .. "attacked; a liveness scan of non-player AConZCharacters (2 s, 0.5 s after an attack) sees the death; the weapon "
      .. "is the NPC's _weaponClassLastHitBy, both named from the catalogues. No attacker within 300 m / 15 s or no "
      .. "weapon recorded = no event (despawned animals are never credited).",
  },
  log = {
    status = "live-supported",
    payloadShape = "{ msg: string }",
    liveVerification = live("admin log line 'Command: location' as a log gameEvent (wire only; Takaro has no log search)"),
    reason = "SCUM admin log lines (low rate).",
  },
}

return M
