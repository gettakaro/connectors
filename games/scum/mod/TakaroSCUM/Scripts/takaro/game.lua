-- SCUM game adapter: the ONLY module that touches the game (UE4SS reflection). All of this mod's Lua runs on the
-- game thread (main.lua ticks with LoopInGameThreadWithDelay; hooks fire on the game thread), so UObjects are used
-- directly; nothing here may block for long. Names below were verified against SCUMServer 1.3.4.2.161892 (UE 4.27.2)
-- with a UE4SS UHT dump and live probes.
--
-- Contract for every action function: fn(self, params, done) returns { ok = true, value = ... } or
-- { ok = false, reason = "..." } (or calls done(result) later). Shapes: see actions.lua.
local util = require("takaro.util")
local json = require("takaro.json")
local scumlog = require("takaro.scumlog")

local M = {}

M.NOT_IMPLEMENTED_REASON = "not_implemented"

-- Engine names used by the adapter (SCUM module is /Script/SCUM).
M.UE = {
  GameMode = "ConZGameMode",                                 -- FindFirstOf: BP_ConZGameMode_C
  PlayerController = "ConZPlayerController",                 -- the only IAdminCommandExecutor
  MiscStatics = "/Script/SCUM.Default__MiscStatics",         -- BroadcastChatLine, SendChatLineToPlayer
  KismetSystem = "/Script/Engine.Default__KismetSystemLibrary",
  ProjectSettings = "/Script/EngineSettings.Default__GeneralProjectSettings", -- ProjectVersion "1.3.4.2.161892+0"
  GameplayStatics = "/Script/Engine.Default__GameplayStatics",
  ChatRpc = "/Script/SCUM.PlayerRpcChannel:Chat_Server_BroadcastChatMessage",
}
M.CHAT = { Default = 0, Local = 1, Global = 2, Squad = 3, Admin = 4, CommandsOnly = 5, ServerMessage = 6, Error = 7 }
-- Takaro has only global/team/friends/whisper. SCUM Local is proximity chat (players within _localChatDistance), so
-- it maps to "friends" (a non-global audience) and never reaches chatBridge's global-only relay; Squad is "team";
-- Admin-channel lines stay "global" so module commands typed there still work.
M.CHAT_TO_TAKARO = { [0] = "global", [1] = "friends", [2] = "global", [3] = "team", [4] = "global" }
M.ENTITY_TYPES = { Zombie = "hostile", ArmedNPC = "hostile", Animal = "neutral" }
M.HOSTILE_ANIMALS = { Bear = true, Wolf = true, Boar = true, Razor = true }
M.CATALOG_BUDGET_US = 4000      -- game-thread time per 100 ms tick for building the item/entity catalogue
M.TAIL_EVERY_MS = 1000
M.BAN_CHECK_EVERY_MS = 10000
M.CATEGORIES = { "login", "chat", "kill", "admin" }
M.MAX_GIVE = 50
M.HANDS_DELAY_MS = 1500
M.DEATH_POLL_MS = 1000
M.DEATH_HOLD_MS = 3000 -- wait for a kill-log line (it names the attacker) before reporting a plain death
M.CATALOG_SCHEMA = 2 -- bump when catalogue entries change, so caches rebuild
M.ABSTRACT = { BP_BTBaseAnimal = true } -- base classes listed as primary assets, never spawned
M.KICK_MESSAGE_DELAY_MS = 2000 -- the reason is shown in chat first; KickPlayer's own screen has no reason text
M.NPC_SCAN_MS = 5000           -- idle NPC liveness scan for entity-killed (FindAllOf ConZCharacter: ~1 ms for 50)
M.NPC_SCAN_FAST_MS = 500       -- while a player attacked recently
M.ATTACK_WINDOW_MS = 15000     -- a kill is credited to a player who attacked within this window
M.KILL_RADIUS = 30000          -- 300 m: the credited player must be this close to the NPC
M.ATTACK_HOOKS = {
  "/Script/SCUM.PlayerRpcChannel:Server_ProcessProjectileDamageEvents",
  "/Script/SCUM.PlayerRpcChannel:Server_ApplyBaseDamageInteractionEventToZombie",
  "/Script/SCUM.PlayerRpcChannel:Server_ApplyBaseDamageInteractionEventToArmedNPC",
  "/Script/SCUM.PlayerRpcChannel:Server_OnExplosiveProjectileHit",
  "/Script/SCUM.ConZCharacter:Server_HandleMeleeAttackCapsuleHit",
}

local function not_implemented()
  return { ok = false, reason = M.NOT_IMPLEMENTED_REASON }
end

-- FString/FText/FName results come back as userdata with ToString; plain strings in tests.
local function str(v)
  if v == nil then return nil end
  if type(v) == "string" then return v end
  local ok, s = pcall(function() return v:ToString() end)
  if ok and type(s) == "string" then return s end
  return nil
end
M.str = str

local function valid(o)
  if o == nil then return false end
  local ok, v = pcall(function() return o:IsValid() end)
  return ok and v == true
end
M.valid = valid

-- "BP_Zombie_Civilian_Fat_Female" -> "Zombie Civilian Fat Female"; "BP_Drifter_Lvl_1" -> "Drifter Lvl 1"
function M.humanize(code)
  local s = tostring(code or ""):gsub("_C$", ""):gsub("^BPC?_", ""):gsub("^BP", "")
  s = s:gsub("_", " "):gsub("(%l)(%u)", "%1 %2"):gsub("%s+", " ")
  return util.trim(s)
end

local Game = {}
Game.__index = Game

-- env: UE4SS globals plus native helpers (list_dir, now_us); log: logger; opts: { mod_dir, fs, events }
function M.new(env, log, opts)
  opts = opts or {}
  local g = setmetatable({ env = env or {}, log = log, hooks = {}, online = {}, ips = {}, bans = {},
    recentChat = {}, perf = { ticks = 0, totalUs = 0, maxUs = 0, catalogUs = 0, loads = 0 },
    catalogs = {}, lastTail = 0, lastBanCheck = 0, kickQueue = {}, handQueue = {}, alive = {}, lastPos = {},
    pendingDeaths = {}, recentKills = {}, lastDeathPoll = 0, banned = {}, gameLoadedBans = {}, pendingUnbans = {},
    npcs = {}, lastNpcScan = -1e9, attacks = {}, delayedKicks = {} }, Game)
  g.fs = opts.fs or M.default_fs(g.env)
  g.paths = M.paths(opts.mod_dir or ".")
  g.modDir = opts.mod_dir or "."
  return g
end

-- <SCUM>/Binaries/Win64/ue4ss/Mods/TakaroSCUM -> <SCUM>/Saved/...
function M.paths(modDir)
  local d = tostring(modDir):gsub("\\", "/")
  local root = d:match("^(.*)/[Bb]inaries/[Ww]in64/") or (d .. "/../../../../..")
  return { root = root, logs = root .. "/Saved/SaveFiles/Logs", config = root .. "/Saved/Config/WindowsServer" }
end

function M.default_fs(env)
  return {
    list = function(dir)
      local text = env.list_dir and env.list_dir(dir)
      local out = {}
      if type(text) == "string" then for n in text:gmatch("[^\n]+") do out[#out + 1] = n end end
      return out
    end,
    size = function(p)
      local f = io.open(p, "rb")
      if not f then return nil end
      local n = f:seek("end")
      f:close()
      return n
    end,
    read_at = function(p, off, n)
      local f = io.open(p, "rb")
      if not f then return nil end
      f:seek("set", off)
      local s = f:read(n)
      f:close()
      return s
    end,
    read = function(p)
      local f = io.open(p, "rb")
      if not f then return nil end
      local s = f:read("a")
      f:close()
      return s
    end,
    write = function(p, text)
      local f = io.open(p, "wb")
      if not f then return false end
      f:write(text)
      f:close()
      return true
    end,
  }
end

function Game:now_ms()
  if self.env.now_ms then return self.env.now_ms() end
  return math.floor(os.clock() * 1000)
end

-- A given item goes into the player's empty hands once it exists for a moment; otherwise it stays at their feet.
function Game:process_hand_queue(nowMs)
  local keep = {}
  for _, h in ipairs(self.handQueue) do
    if nowMs < h.due then
      keep[#keep + 1] = h
    elseif valid(h.item) then
      local pc = self:find_pc(h.id)
      local prisoner = pc and self:pawn(pc)
      if prisoner then
        local okH, inHands = pcall(function() return prisoner:GetItemInHands() end)
        if not (okH and valid(inHands)) then
          local ok, took = pcall(function() return prisoner:TakeItemInHands(h.item) end)
          self.log:info("giveItem: TakeItemInHands for %s -> %s", h.id, tostring(ok and took))
        end
      end
    end
  end
  self.handQueue = keep
end

function Game:now_us()
  if self.env.now_us then return self.env.now_us() end
  return math.floor(os.clock() * 1e6)
end

-- Cached singletons, re-resolved when stale.
function Game:obj(key, finder)
  local o = self[key]
  if valid(o) then return o end
  o = finder()
  if valid(o) then self[key] = o return o end
  return nil
end

function Game:game_mode()
  return self:obj("_gm", function() return self.env.FindFirstOf and self.env.FindFirstOf(M.UE.GameMode) end)
end

function Game:static(key, path)
  return self:obj(key, function() return self.env.StaticFindObject and self.env.StaticFindObject(path) end)
end

-- Online players: one entry per ConZPlayerController that has a Steam user id.
function Game:controllers()
  local list = self.env.FindAllOf and self.env.FindAllOf(M.UE.PlayerController) or nil
  local out = {}
  for _, pc in ipairs(list or {}) do
    if valid(pc) then
      local id = str(pc:GetUserId())
      if id and id:match("^7656%d+$") then out[#out + 1] = { pc = pc, id = id } end
    end
  end
  return out
end

function Game:player_of(pc, id)
  local name = str(pc:GetUserProfileName())
  if not name or name == "" then
    local ok, ps = pcall(function() return pc.PlayerState end)
    if ok and valid(ps) then name = str(ps:GetPlayerName()) end
  end
  local ping
  local ok, ps = pcall(function() return pc.PlayerState end)
  if ok and valid(ps) then
    -- APlayerState.Ping is the compressed ping (ms / 4, uint8); UE4SS cannot read ExactPing on this build.
    local okp, p = pcall(function() return ps.Ping end)
    if okp and type(p) == "number" and p > 0 then ping = math.floor(p * 4) end
  end
  return { steamId = id, name = name, ip = self.ips[id], ping = ping }
end

function Game:find_pc(id)
  for _, c in ipairs(self:controllers()) do
    if c.id == id then return c.pc end
  end
  return nil
end

function Game:pawn(pc)
  local ok, p = pcall(function() return pc:GetPrisoner() end)
  if ok and valid(p) then return p end
  ok, p = pcall(function() return pc.Pawn end)
  if ok and valid(p) then return p end
  return nil
end

-- Actions ---------------------------------------------------------------------------------------------------

function Game:reachability()
  local gm = self:game_mode()
  if not gm then return { ok = true, value = { connectable = false, reason = "SCUM world is not loaded (no ConZGameMode)" } } end
  if not self:static("_ms", M.UE.MiscStatics) then
    return { ok = true, value = { connectable = false, reason = "MiscStatics not found" } }
  end
  return { ok = true, value = { connectable = true } }
end

function Game:list_players()
  local out = {}
  for _, c in ipairs(self:controllers()) do out[#out + 1] = self:player_of(c.pc, c.id) end
  self:reconcile_online(out)
  return { ok = true, value = out }
end

function Game:get_player(params)
  local pc = self:find_pc(params.gameId)
  if not pc then return { ok = false, reason = "player is offline" } end
  return { ok = true, value = self:player_of(pc, params.gameId) }
end

-- Takaro polls positions right after a logout too and rejects anything but an IPosition, so an offline or dead
-- player answers with the last position we saw (1 Hz poll, login/logout log lines).
function Game:get_player_location(params)
  local pc = self:find_pc(params.gameId)
  local pawn = pc and self:pawn(pc)
  if pawn then
    local v = pawn:K2_GetActorLocation()
    local pos = { x = v.X, y = v.Y, z = v.Z }
    self.lastPos[params.gameId] = pos
    return { ok = true, value = pos }
  end
  local last = self.lastPos[params.gameId]
  if last then return { ok = true, value = last } end
  return { ok = false, reason = pc and "player has no character (dead or in the spawn menu)" or "player is offline" }
end

function Game:teleport_player(params)
  local pc = self:find_pc(params.gameId)
  if not pc then return { ok = false, reason = "player is offline" } end
  local pawn = self:pawn(pc)
  if not pawn then return { ok = false, reason = "player has no character" } end
  local target = { X = params.x, Y = params.y, Z = params.z }
  -- AConZPlayerController:Teleport(FTeleportRequest) streams the world in for the client first.
  local ok, err = pcall(function()
    pc:Teleport({ Subject = pawn, TargetLocation = target, TargetRotation = { Pitch = 0, Yaw = 0, Roll = 0 } })
  end)
  if ok then return { ok = true, value = "teleport requested (ConZPlayerController:Teleport)" } end
  self.log:warn("ConZPlayerController:Teleport failed (%s); falling back to K2_TeleportTo", tostring(err))
  local ok2, moved = pcall(function() return pawn:K2_TeleportTo(target, { Pitch = 0, Yaw = 0, Roll = 0 }) end)
  if ok2 and moved ~= false then return { ok = true, value = "teleported (K2_TeleportTo)" } end
  return { ok = false, reason = "teleport failed: " .. tostring(ok2 and "blocked" or moved) }
end

-- SCUM's kick screen says only "You have been kicked off the server!", so the reason goes to the player's chat
-- (and HUD) first and the kick follows KICK_MESSAGE_DELAY_MS later; Takaro gets its answer after the kick.
function Game:tell(pc, text)
  local ms = self:static("_ms", M.UE.MiscStatics)
  if ms then pcall(function() ms:SendChatLineToPlayer(pc, text, M.CHAT.ServerMessage, false) end) end
end

function Game:schedule_kick(id, reason, prefix, done)
  local pc = self:find_pc(id)
  if not pc then return { ok = false, reason = "player is offline" } end
  if not self:game_mode() then return { ok = false, reason = "no game mode" } end
  self:tell(pc, prefix .. reason)
  self.delayedKicks[#self.delayedKicks + 1] = { id = id, reason = reason, due = self:now_ms() + M.KICK_MESSAGE_DELAY_MS,
    done = done }
  return nil
end

function Game:process_delayed_kicks(nowMs)
  local keep = {}
  for _, k in ipairs(self.delayedKicks) do
    if nowMs < k.due then
      keep[#keep + 1] = k
    else
      local pc, gm = self:find_pc(k.id), self:game_mode()
      local res
      if pc and gm then
        local ok, err = pcall(function() gm:KickPlayer(pc, k.reason) end)
        res = ok and { ok = true } or { ok = false, reason = "KickPlayer failed: " .. tostring(err) }
      else
        res = { ok = true, value = "player left before the kick" }
      end
      if k.done then k.done(res) end
    end
  end
  self.delayedKicks = keep
end

function Game:kick_player(params, done)
  return self:schedule_kick(params.gameId, params.reason or "Kicked by Takaro", "You are kicked: ", done)
end

function Game:send_message(params)
  local ms = self:static("_ms", M.UE.MiscStatics)
  local gm = self:game_mode()
  if not (ms and gm) then return { ok = false, reason = "chat statics not available" } end
  local text = params.message
  if params.senderName and params.senderName ~= "" then text = params.senderName .. ": " .. text end
  if params.recipientGameId then
    local pc = self:find_pc(params.recipientGameId)
    if not pc then return { ok = false, reason = "recipient is offline" } end
    ms:SendChatLineToPlayer(pc, text, M.CHAT.ServerMessage, false)
  else
    ms:BroadcastChatLine(gm, text, M.CHAT.ServerMessage)
  end
  self.log:info("sendMessage %s: %s", params.recipientGameId and ("to " .. params.recipientGameId) or "global", text)
  return { ok = true }
end

-- Item classes are primary assets of type "Item" (2854 on 1.3.4.2); codes are their asset names (Weapon_M9).
function Game:load_class(assetType, code)
  local ksl = self:static("_ksl", M.UE.KismetSystem)
  if not ksl then return nil, "KismetSystemLibrary not found" end
  local sc = ksl:GetSoftClassReferenceFromPrimaryAssetId({ PrimaryAssetType = { Name = self.env.FName(assetType) },
    PrimaryAssetName = self.env.FName(code) })
  local cls = ksl:LoadClassAsset_Blocking(sc)
  if not valid(cls) then return nil, "unknown " .. assetType .. " code '" .. tostring(code) .. "'" end
  return cls
end

function Game:give_item(params)
  local pc = self:find_pc(params.gameId)
  if not pc then return { ok = false, reason = "player is offline" } end
  local prisoner = self:pawn(pc)
  if not prisoner then return { ok = false, reason = "player has no character" } end
  local cls, why = self:load_class("Item", params.item)
  if not cls then return { ok = false, reason = why } end
  local gps = self:static("_gps", M.UE.GameplayStatics)
  local at = prisoner:K2_GetActorLocation()
  local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = at.X, Y = at.Y, Z = at.Z + 50 },
    Scale3D = { X = 1, Y = 1, Z = 1 } }
  local n = math.min(params.amount or 1, M.MAX_GIVE)
  -- Inventory: UInventoryUserComponent (on the controller) :Server_CharacterInventoryComponent_PickupItem(prisoner
  -- inventory, item) is the server side of the player's own "pick up"; called on the server it runs locally and puts
  -- the item into a free clothes/backpack slot (live 2026-10-14: 2 apples -> shirt + pants pockets, ground empty).
  local iuc, inv = self:inventory_components(pc, prisoner)
  local me = str(prisoner:GetFullName())
  local inInv, onGround, toHands, wornItems = 0, 0, nil, {}
  local quality = M.quality_percent(params.quality)
  for i = 1, n do
    local a = gps:BeginDeferredActorSpawnFromClass(prisoner, cls, xf, 1, nil)
    local item = valid(a) and gps:FinishSpawningActor(a, xf) or nil
    if not valid(item) then
      return { ok = false, reason = string.format("spawning %s failed after %d (%d in inventory, %d on the ground)",
        params.item, i - 1, inInv, onGround) }
    end
    if quality then pcall(function() item:SetHealth(item:GetMaxHealth() * quality / 100) end) end
    local stored = false
    if iuc and inv then
      -- Clothes (backpack, vest, holster, ...) go onto a free body slot first, like the player's own "wear"; a
      -- taken slot leaves the item unowned and it is picked up into a container like anything else.
      if self:is_a(item, "_clothesC", M.UE.ClothesClass) then
        pcall(function() iuc:Server_CharacterInventoryComponent_EquipClothes(inv, item) end)
        if valid(item) and self:owned_by(item, me) then
          stored = true
          wornItems[#wornItems + 1] = item
        end
      end
      if not stored then
        pcall(function() iuc:Server_CharacterInventoryComponent_PickupItem(inv, item) end)
        -- Stackables (ammo, bandages) merge into a stack already in the inventory: the picked-up actor is left with a
        -- quantity of 0 and the game destroys it a moment later (live 2026-10-14: 5 x Cal_50_AE -> one "5x" stack;
        -- 2 x Emergency_bandage -> one "2x" stack). Never GetAmmoCount alone: an empty weapon or magazine reports 0.
        stored = not valid(item) or self:owned_by(item, me) or self:merged(item)
      end
    end
    if stored then
      inInv = inInv + 1
    else
      onGround = onGround + 1
      -- A full inventory (or a 2-handed item that fits nowhere): the first such item goes into empty hands a moment
      -- later (TakeItemInHands right after the spawn is ignored), the rest stays at the player's feet.
      if not toHands then toHands = item end
    end
  end
  if toHands then
    self.handQueue[#self.handQueue + 1] = { id = params.gameId, item = toHands, due = self:now_ms() + M.HANDS_DELAY_MS }
  end
  -- Worn = on the body (direct child of the prisoner) when the give is done: a later EquipClothes on a taken slot
  -- swaps, and the earlier one moves into a container (live 2026-10-14: 2 caps -> one on the head, one in the backpack).
  local worn = 0
  for _, it in ipairs(wornItems) do
    local ok, o = pcall(function() return it:GetOwner() end)
    if ok and valid(it) and valid(o) and str(o:GetFullName()) == me then worn = worn + 1 end
  end
  local msg = string.format("gave %d x %s to %s: %d in inventory%s, %d at the player's feet%s%s", n, params.item,
    params.gameId, inInv, worn > 0 and string.format(" (%d worn)", worn) or "", onGround,
    toHands and " (first one into empty hands)" or "", quality and string.format(", durability %d%%", quality) or "")
  self.log:info("giveItem: %s", msg)
  return { ok = true, value = msg }
end

function Game:inventory_components(pc, prisoner)
  local iuc, inv
  local me, mePc = str(prisoner:GetFullName()), str(pc:GetFullName())
  for _, c in ipairs(self.env.FindAllOf and self.env.FindAllOf("InventoryUserComponent") or {}) do
    if valid(c) and str(c:GetOuter():GetFullName()) == mePc then iuc = c break end
  end
  for _, c in ipairs(self.env.FindAllOf and self.env.FindAllOf("PrisonerInventoryComponent") or {}) do
    if valid(c) and str(c:GetOuter():GetFullName()) == me then inv = c break end
  end
  if not (iuc and inv) then self.log:warn("giveItem: inventory components not found; items go to the ground") end
  return iuc, inv
end

function Game:merged(item)
  return self:stack_quantity(item) == 0
end

-- Takaro's giveItem quality (a string; modules send null or "") -> durability percent 1..100, or nil to keep the
-- game's default. SCUM has no quality tiers; durability is the matching per-item number (getPlayerInventory reports it).
function M.quality_percent(q)
  local n = tonumber(type(q) == "string" and util.trim(q) or q)
  if not n or n ~= n then return nil end
  n = math.floor(n + 0.5)
  if n < 1 or n > 100 then return nil end
  return n
end

function Game:owned_by(item, me)
  local o = item:GetOwner()
  local depth = 0
  while valid(o) and depth < M.OWNER_DEPTH do
    if str(o:GetFullName()) == me then return true end
    o = o:GetOwner()
    depth = depth + 1
  end
  return false
end

-- Inventory entries are not reflected in SCUM (UInventoryComponent exposes only _repHasEntries). Every carried item is
-- an AItem actor in the prisoner's AActor.Children tree: worn clothes, holsters and the item in hands are children of
-- BP_Prisoner_C, container contents children of the clothing they sit in (backpack -> ammo, holster -> pistol), and so
-- on down. Walking that tree costs O(carried items) (live 2026-10-14: 16 items, < 0.1 ms) instead of scanning every
-- item in the world. A stack is one actor (13x .50 AE = one Cal_50_AE_C with quantity 13, see stack_quantity); a
-- weapon's or magazine's loaded rounds are not items and not a stack, so amount stays 1 for those. quality = durability in percent (GetHealth / GetMaxHealth, rounded), the number the client shows.
M.OWNER_DEPTH = 5
M.INVENTORY_MAX_DEPTH = 8
M.INVENTORY_MAX_ITEMS = 1000
M.UE.ItemClass = "/Script/SCUM.Item"
M.UE.AmmoClass = "/Script/SCUM.AmmunitionItem"
M.UE.ClothesClass = "/Script/SCUM.ClothesItem"

function Game:is_a(obj, key, path)
  local cls = self:static(key, path)
  if not cls then return nil end
  local ok, r = pcall(function() return obj:IsA(cls) end)
  if ok then return r == true end
  return nil
end

function M.durability(it)
  local okH, h = pcall(function() return it:GetHealth() end)
  local okM, m = pcall(function() return it:GetMaxHealth() end)
  if not (okH and okM) or type(h) ~= "number" or type(m) ~= "number" or m <= 0 then return nil end
  local pct = math.floor(h / m * 100 + 0.5)
  if pct < 0 then pct = 0 elseif pct > 100 then pct = 100 end
  return tostring(pct)
end

M.UE.StackableComponent = "/Script/SCUM.StackableItemComponent"

-- Stack size of a stackable item (ammo, bandages, rags, ...): its UStackableItemComponent._repQuantity, the "13x" the
-- client shows; nil for items that do not stack. Uses (Apple "1/1") live on UDiscreteUsageItemComponent and are not a
-- stack. Ammunition also answers GetAmmoCount with the same number (fallback).
function Game:stack_quantity(it)
  local okL, comps = pcall(function() return it._itemComponents end)
  if okL and comps ~= nil then
    local okN, n = pcall(function() return #comps end)
    for i = 1, (okN and n or 0) do
      local c = comps[i]
      if valid(c) and self:is_a(c, "_stackC", M.UE.StackableComponent) then
        local okQ, q = pcall(function() return c._repQuantity end)
        if okQ and type(q) == "number" then return math.floor(q) end
      end
    end
  end
  if self:is_a(it, "_ammoC", M.UE.AmmoClass) then
    local ok, q = pcall(function() return it:GetAmmoCount() end)
    if ok and type(q) == "number" then return math.floor(q) end
  end
  return nil
end

function Game:item_amount(it)
  local q = self:stack_quantity(it)
  if q and q >= 1 then return q end
  return 1
end

-- The prisoner's carried items, depth first (container before its contents). nil when Children is not readable.
function Game:carried_items(prisoner)
  local out, seen = {}, {}
  local function children(a)
    local ok, ch = pcall(function() return a.Children end)
    if not ok or ch == nil then return nil end
    local list = {}
    local okN, n = pcall(function() return #ch end)
    if not okN then return nil end
    for i = 1, n do
      local c = ch[i]
      list[#list + 1] = c
    end
    return list
  end
  local top = children(prisoner)
  if not top then return nil end
  local function walk(list, depth)
    for _, c in ipairs(list) do
      if #out >= M.INVENTORY_MAX_ITEMS then return end
      if valid(c) then
        local key = str(c:GetFullName()) or tostring(c)
        if not seen[key] then
          seen[key] = true
          if self:is_a(c, "_itemC", M.UE.ItemClass) ~= false then out[#out + 1] = c end
          if depth < M.INVENTORY_MAX_DEPTH then
            local sub = children(c)
            if sub then walk(sub, depth + 1) end
          end
        end
      end
    end
  end
  walk(top, 1)
  return out
end

-- Fallback when AActor.Children cannot be read: every item in the world whose Owner chain reaches the prisoner.
function Game:owned_items_by_scan(prisoner)
  local me = str(prisoner:GetFullName())
  local out = {}
  for _, it in ipairs(self.env.FindAllOf and self.env.FindAllOf("Item") or {}) do
    if valid(it) and self:owned_by(it, me) then out[#out + 1] = it end
  end
  return out
end

-- Item code = the catalogue's primary-asset name. The class is "<asset>_C" for all but a few assets whose class
-- differs in case (live 2026-10-14, 2/2854: Weapon_DEagle_357 -> Weapon_Deagle_357_C, Weapon_Parts_Deagle_50 ->
-- Weapon_Parts_DEagle_50_C); Takaro matches inventory codes to listItems codes exactly, so those map back here.
function Game:item_code(className)
  local code = tostring(className):gsub("_C$", "")
  local c = self.catalogs.items
  if not c or #c.entries == 0 then return code end
  if c.lowerIndexSize ~= #c.entries then
    c.lowerIndex = {}
    for _, e in ipairs(c.entries) do c.lowerIndex[e.code:lower()] = e.code end
    c.lowerIndexSize = #c.entries
  end
  return c.lowerIndex[code:lower()] or code
end

function Game:get_player_inventory(params)
  local pc = self:find_pc(params.gameId)
  if not pc then return { ok = false, reason = "player is offline" } end
  local prisoner = self:pawn(pc)
  -- No character (dead, respawn screen, still loading): the state is unknown, so no answer rather than [] (Takaro
  -- would record every item as removed and re-added). The next read after the respawn diffs the real change.
  if not prisoner then return { ok = false, reason = "player has no character right now" } end
  local items = self:carried_items(prisoner)
  if items == nil then
    items = self:owned_items_by_scan(prisoner)
  elseif #items == 0 then
    -- Guard: an empty walk is cross-checked with the world scan; items found there mean the walk failed, and an
    -- error (not []) keeps Takaro from recording removals. Both empty = the player really carries nothing.
    local scan = self:owned_items_by_scan(prisoner)
    if #scan > 0 then
      return { ok = false, reason = string.format("inventory walk found nothing but %d items are owned by the player", #scan) }
    end
  end
  -- The item in hands is normally a child of the prisoner; add it if it is not.
  local okH, inHands = pcall(function() return prisoner:GetItemInHands() end)
  if okH and valid(inHands) then
    local k = str(inHands:GetFullName())
    local found = false
    for _, it in ipairs(items) do if str(it:GetFullName()) == k then found = true break end end
    if not found then items[#items + 1] = inHands end
  end
  local rows, order = {}, {}
  for _, it in ipairs(items) do
    local code = self:item_code(str(it:GetClass():GetFName()) or "?")
    local quality = M.durability(it)
    local key = code .. "|" .. (quality or "")
    local row = rows[key]
    if not row then
      local name
      pcall(function() name = str(it:GetInteractableName(nil)) end)
      row = { code = code, name = (name and name ~= "") and util.trim(name) or M.humanize(code), amount = 0,
        quality = quality }
      rows[key] = row
      order[#order + 1] = key
    end
    row.amount = row.amount + self:item_amount(it)
  end
  local out = {}
  for _, key in ipairs(order) do out[#out + 1] = rows[key] end
  return { ok = true, value = out }
end

-- Catalogues: built incrementally on the game thread (each class load can block for up to ~1 s), cached on disk
-- per engine build so later starts cost nothing.
function Game:catalog(kind)
  local c = self.catalogs[kind]
  if c then return c end
  c = { kind = kind, entries = {}, queue = {}, done = false, started = false, failed = 0 }
  self.catalogs[kind] = c
  return c
end

function Game:catalog_types(kind)
  if kind == "items" then return { "Item" } end
  return { "Zombie", "ArmedNPC", "Animal" }
end

function Game:build_key()
  local ksl = self:static("_ksl", M.UE.KismetSystem)
  local v = ksl and str(ksl:GetEngineVersion()) or "unknown"
  return v
end

function Game:catalog_start(kind)
  local c = self:catalog(kind)
  if c.started then return c end
  local ksl = self:static("_ksl", M.UE.KismetSystem)
  if not ksl or not self:game_mode() then return c end
  c.started = true
  local codes = {}
  for _, t in ipairs(self:catalog_types(kind)) do
    local ids = {}
    ksl:GetPrimaryAssetIdList({ Name = self.env.FName(t) }, ids)
    for i = 1, #ids do
      local id = ids[i]
      if type(id) == "userdata" and id.get then id = id:get() end
      local code = str(id.PrimaryAssetName)
      if code and not M.ABSTRACT[code] then codes[#codes + 1] = { type = t, code = code } end
    end
  end
  c.key = self:build_key() .. "|" .. #codes .. "|v" .. M.CATALOG_SCHEMA
  local cachePath = self.modDir .. "/" .. kind .. "-cache.json"
  local cached = self.fs.read and self.fs.read(cachePath)
  if cached then
    local ok, data = pcall(json.decode, cached)
    if ok and type(data) == "table" and data.key == c.key and type(data.entries) == "table" then
      c.entries, c.done = data.entries, true
      self.log:info("%s catalogue: %d entries from cache (%s)", kind, #c.entries, c.key)
      return c
    end
  end
  c.queue, c.total, c.cachePath = codes, #codes, cachePath
  self.log:info("%s catalogue: building %d entries on the game thread (budget %d us per tick)", kind, #codes,
    M.CATALOG_BUDGET_US)
  return c
end

function Game:catalog_entry(kind, t, code, cls)
  local cdo = cls:GetCDO()
  local name, desc
  pcall(function() name = str(cdo:GetInteractableName(nil)) end)
  if kind == "items" then pcall(function() desc = str(cdo:GetDescription()) end) end
  if (not name or name == "") and t == "Animal" then pcall(function() name = str(cdo.SpeciesName) end) end
  local humanized = false
  if not name or util.trim(name) == "" then name, humanized = M.humanize(code), true end
  if t == "Animal" and code:find("Mutant") and not name:find("Mutant") then name = name .. " (Mutant)" end
  local e = { code = code, name = name }
  if desc and desc ~= "" then e.description = desc end
  if kind ~= "items" then
    local et = M.ENTITY_TYPES[t] or "neutral"
    if t == "Animal" and M.HOSTILE_ANIMALS[(name:gsub(" %(Mutant%)$", ""))] then et = "hostile" end
    e.type = et
  end
  return e, humanized
end

function Game:catalog_step(kind, budgetUs)
  local c = self.catalogs[kind]
  if not c or c.done or not c.started then return end
  local t0 = self:now_us()
  while #c.queue > 0 and self:now_us() - t0 < budgetUs do
    local q = table.remove(c.queue, 1)
    local ok, cls = pcall(self.load_class, self, q.type, q.code)
    self.perf.loads = self.perf.loads + 1
    if ok and cls then
      local ok2, e, humanized = pcall(self.catalog_entry, self, kind, q.type, q.code, cls)
      if ok2 and e then
        c.entries[#c.entries + 1] = e
        if humanized then c.humanized = (c.humanized or 0) + 1 end
      else
        c.failed = c.failed + 1
      end
    else
      c.failed = c.failed + 1
    end
  end
  self.perf.catalogUs = self.perf.catalogUs + (self:now_us() - t0)
  if #c.queue == 0 then
    c.done = true
    table.sort(c.entries, function(a, b) return a.code < b.code end)
    self.log:info("%s catalogue built: %d entries (%d humanized names, %d failed loads, %d ms game thread)", kind,
      #c.entries, c.humanized or 0, c.failed, math.floor(self.perf.catalogUs / 1000))
    if self.fs.write then
      self.fs.write(c.cachePath, json.encode({ key = c.key, entries = c.entries }))
    end
    if self.on_catalog_done then pcall(self.on_catalog_done, kind) end
  end
end

function Game:list_items()
  local c = self:catalog_start("items")
  if not c.done then self.log:warn("listItems: catalogue still building (%d/%d)", #c.entries, c.total or 0) end
  return { ok = true, value = c.entries }
end

function Game:list_entities()
  local c = self:catalog_start("entities")
  if not c.done then self.log:warn("listEntities: catalogue still building (%d/%d)", #c.entries, c.total or 0) end
  return { ok = true, value = c.entries }
end

function Game:list_locations()
  return { ok = false, reason = "SCUM has no named location registry the server exposes" }
end

-- The running game build: SCUM sets GeneralProjectSettings.ProjectVersion from its release config at start.
function Game:game_version()
  local gps = self:static("_projectSettings", M.UE.ProjectSettings)
  local v = gps and str(gps.ProjectVersion)
  if not v or v == "" then return { ok = false, reason = "ProjectVersion not set" } end
  return { ok = true, value = v }
end

-- AConZWorldSettings.WorldBounds (FBox2D, world cm): the box the island map is centred in (see map.lua).
function Game:world_bounds()
  local ws = self.env.FindFirstOf and self.env.FindFirstOf("ConZWorldSettings")
  if not valid(ws) then return { ok = false, reason = "world not loaded" } end
  local b = ws.WorldBounds
  if not b or not b.bIsValid then return { ok = false, reason = "WorldBounds not set" } end
  return { ok = true, value = { minX = b.Min.X, maxX = b.Max.X, minY = b.Min.Y, maxY = b.Max.Y } }
end

-- Bans. Two stores: BannedUsers.ini (the game's own, one SteamID64 per line) and takaro-bans.json (reason, expiresAt,
-- name). SCUM reads BannedUsers.ini ONCE at start into memory and refuses those players at join itself; the list is
-- not reflected, so the connector cannot edit it. Hence:
--  * ban: written to both files (the game enforces it after the next restart even without the connector) and kept in
--    an in-memory set that the connector enforces itself: a banned id is kicked as soon as its PlayerController
--    exists (1 Hz controller poll; login takes ~10 s longer), before the prisoner reaches the world;
--  * unban: removed from both files and the set at once; the player can rejoin immediately (live 2026-10-14, also for
--    a ban SCUM loaded at server start: SCUM re-reads the file ~9 s after a change, and its own join path never refused
--    a listed id on this build; the connector's controller-time kick is the enforcement). For a ban that was in the
--    file at server start the connector additionally runs #UnbanPlayer through an online admin (now or later).
function Game:ban_file() return self.paths.config .. "/BannedUsers.ini" end

function Game:read_ban_file()
  local text = self.fs.read and self.fs.read(self:ban_file()) or ""
  local ids, lines = {}, {}
  for line in (text .. "\n"):gmatch("([^\r\n]*)\r?\n") do
    lines[#lines + 1] = line
    local id = line:match("^%s*(7656%d%d%d%d%d%d%d%d%d%d%d%d%d)")
    if id then ids[id] = true end
  end
  while #lines > 0 and lines[#lines] == "" do lines[#lines] = nil end
  return ids, lines
end

function Game:write_ban_file(lines)
  return self.fs.write(self:ban_file(), table.concat(lines, "\r\n") .. (#lines > 0 and "\r\n" or ""))
end

function Game:load_bans()
  if self.bansLoaded then return end
  self.bansLoaded = true
  local text = self.fs.read and self.fs.read(self.modDir .. "/takaro-bans.json")
  if text then
    local ok, data = pcall(json.decode, text)
    if ok and type(data) == "table" then
      if type(data.bans) == "table" then
        self.bans = data.bans
        self.gameLoadedBans = type(data.gameLoaded) == "table" and data.gameLoaded or {}
        self.pendingUnbans = type(data.pendingUnbans) == "table" and data.pendingUnbans or {}
        self.banBootKey = data.bootKey
      else
        self.bans = data -- v1 file: the map itself
      end
    end
  end
  self:refresh_banned()
end

-- Called once per server start (the first time the world exists): whatever BannedUsers.ini holds now is what the game
-- loaded into memory. A Lua hot reload keeps the saved set (the game did not re-read the file).
function Game:note_game_loaded_bans(serverStartKey)
  self:load_bans()
  if self.banBootKey == serverStartKey then return end
  self.banBootKey = serverStartKey
  local ids = self:read_ban_file()
  self.gameLoadedBans = {}
  for id in pairs(ids) do self.gameLoadedBans[id] = true end
  self.pendingUnbans = {}
  self:save_bans()
end

function Game:refresh_banned()
  local ids = self:read_ban_file()
  local set = {}
  for id in pairs(ids) do set[id] = true end
  for id in pairs(self.bans) do set[id] = true end
  self.banned = set
end

function Game:save_bans()
  if self.fs.write then
    self.fs.write(self.modDir .. "/takaro-bans.json", json.encode({ bans = self.bans, gameLoaded = self.gameLoadedBans,
      pendingUnbans = self.pendingUnbans, bootKey = self.banBootKey }))
  end
end

function Game:ban_player(params, done)
  self:load_bans()
  local id = params.gameId
  if not tostring(id):match("^7656%d+$") then return { ok = false, reason = "SCUM bans need a SteamID64" } end
  local ids, lines = self:read_ban_file()
  if not ids[id] then
    lines[#lines + 1] = id
    if not self:write_ban_file(lines) then return { ok = false, reason = "cannot write BannedUsers.ini" } end
  end
  self.bans[id] = { reason = params.reason, expiresAt = params.expiresAt, at = os.time(), name = params.name }
  self.pendingUnbans[id] = nil
  self:save_bans()
  self:refresh_banned()
  if self:find_pc(id) then
    return self:schedule_kick(id, params.reason or "Banned", "You are banned: ", done)
  end
  return { ok = true }
end

function Game:admin_command(cmd, exceptId)
  for _, c in ipairs(self:controllers()) do
    if c.id ~= exceptId then
      local okA, isAdmin = pcall(function() return c.pc:IsUserAdmin() end)
      if okA and isAdmin then
        local ch = c.pc:GetPlayerRpcChannel()
        if valid(ch) then
          ch:Chat_Server_ProcessAdminCommand(cmd)
          return c.id
        end
      end
    end
  end
  return nil
end

function Game:unban_player(params)
  self:load_bans()
  local id = params.gameId
  local ids, lines = self:read_ban_file()
  if ids[id] then
    local keep = {}
    for _, l in ipairs(lines) do
      if l:match("^%s*(7656%d+)") ~= id then keep[#keep + 1] = l end
    end
    if not self:write_ban_file(keep) then return { ok = false, reason = "cannot write BannedUsers.ini" } end
  end
  self.bans[id] = nil
  local note = nil
  if self.gameLoadedBans[id] then
    local via = self:admin_command("UnbanPlayer " .. id, id)
    if via then
      self.gameLoadedBans[id] = nil
      note = "SCUM's in-memory ban lifted with #UnbanPlayer through admin " .. via
    else
      self.pendingUnbans[id] = true
      note = "this ban was in BannedUsers.ini at server start; #UnbanPlayer will also run through the next online "
        .. "admin (belt and braces: live 2026-10-14 the player could rejoin right after the unban without it)"
    end
    self.log:info("unban %s: %s", id, note)
  end
  self:save_bans()
  self:refresh_banned()
  return { ok = true, value = note }
end

function Game:process_pending_unbans()
  for id in pairs(self.pendingUnbans) do
    local via = self:admin_command("UnbanPlayer " .. id, id)
    if not via then return end
    self.log:info("unban %s: #UnbanPlayer ran through admin %s", id, via)
    self.pendingUnbans[id] = nil
    self.gameLoadedBans[id] = nil
    self:save_bans()
  end
end

function Game:is_banned(id)
  self:load_bans()
  return self.banned[id] == true
end

function Game:list_bans()
  self:load_bans()
  self:refresh_banned()
  local out = {}
  for id in pairs(self.banned) do
    local b = self.bans[id] or {}
    local name = (self.names and self.names[id]) or b.name
    out[#out + 1] = { player = { steamId = id, name = name }, reason = b.reason, expiresAt = b.expiresAt }
  end
  table.sort(out, function(a, b) return a.player.steamId < b.player.steamId end)
  return { ok = true, value = out }
end

-- "2026-10-14T01:02:03.000Z" -> epoch seconds (UTC)
function M.iso_to_epoch(s)
  local y, mo, d, h, mi, se = tostring(s):match("^(%d+)%-(%d+)%-(%d+)T?(%d*):?(%d*):?(%d*)")
  if not y then return nil end
  local t = { year = tonumber(y), month = tonumber(mo), day = tonumber(d), hour = tonumber(h) or 0,
    min = tonumber(mi) or 0, sec = tonumber(se) or 0, isdst = false }
  local localT = os.time(t)
  local utcOffset = os.time(os.date("*t", localT)) - os.time(os.date("!*t", localT))
  return localT + utcOffset
end

function Game:expire_bans(nowEpoch)
  self:load_bans()
  for id, b in pairs(self.bans) do
    local exp = b.expiresAt and M.iso_to_epoch(b.expiresAt)
    if exp and exp <= nowEpoch then
      self.log:info("timed ban of %s expired (%s); lifting it", id, b.expiresAt)
      self:unban_player({ gameId = id })
    end
  end
end

-- Console commands. SCUM runs #admin commands only through a player controller (AConZPlayerController is the only
-- IAdminCommandExecutor); MiscStatics:Test_ProcessAdminCommand is a no-op without a player. So:
--  * verbs the connector implements with direct UFunctions work with nobody online (announce, listplayers);
--  * every other allowlisted verb is sent through an online admin's PlayerRpcChannel:Chat_Server_ProcessAdminCommand.
-- Dead ends (live 2026-10-14): a server-spawned BP_ConZPlayerController (no connection, no user) is ignored by SCUM's
-- command handler and crashed the server ~60 s later; borrowing a non-admin player's channel with the command CDO's
-- _requiredExecutorLevel lowered would run the command in that player's name (admin log, output in their chat).
function Game:execute_command(params)
  local cmd = util.trim(params.command):gsub("^#", "")
  local verb = (cmd:match("^(%S+)") or ""):lower()
  local rest = util.trim(cmd:sub(#verb + 1))
  if verb == "announce" then
    if rest == "" then return { ok = false, reason = "announce needs a message" } end
    local gm = self:game_mode()
    if not gm then return { ok = false, reason = "no game mode" } end
    gm:SendHUDMessageToAll(rest, false)
    local ms = self:static("_ms", M.UE.MiscStatics)
    if ms then ms:BroadcastChatLine(gm, rest, M.CHAT.ServerMessage) end
    return { ok = true, value = "announced: " .. rest }
  elseif verb == "listplayers" then
    local res = self:list_players()
    local lines = {}
    for i, p in ipairs(res.value) do
      lines[#lines + 1] = string.format("%d. %s (%s) ping %s", i, tostring(p.name), p.steamId, tostring(p.ping or "?"))
    end
    return { ok = true, value = #lines > 0 and table.concat(lines, "\n") or "no players online" }
  end
  for _, c in ipairs(self:controllers()) do
    local okA, isAdmin = pcall(function() return c.pc:IsUserAdmin() end)
    if okA and isAdmin then
      local ch = c.pc:GetPlayerRpcChannel()
      if valid(ch) then
        -- The channel takes the command WITHOUT '#' ("#Location" -> "Unrecognized command.", live 2026-10-13).
        ch:Chat_Server_ProcessAdminCommand(cmd)
        return { ok = true, value = "ran '" .. cmd .. "' through admin " .. c.id
          .. "; SCUM shows the output in that admin's chat" }
      end
    end
  end
  return { ok = false, reason = "no_executor: SCUM runs #" .. verb .. " only through an online admin "
    .. "(AdminUsers.ini); connector-native verbs without an admin: announce, listplayers" }
end

function Game:shutdown()
  local ksl = self:static("_ksl", M.UE.KismetSystem)
  local gm = self:game_mode()
  if not (ksl and gm) then return { ok = false, reason = "world not loaded" } end
  ksl:QuitGame(gm, nil, 0, false)
  return { ok = true }
end

-- Events ----------------------------------------------------------------------------------------------------

function Game:mark_online(p)
  if self.online[p.steamId] then return false end
  self.online[p.steamId] = true
  return true
end

-- getPlayers is the authoritative online list: a player we never announced gets player-connected here.
function Game:reconcile_online(list)
  if not self.events then return end
  for _, p in ipairs(list) do
    if self:mark_online(p) then self.events:player_connected(p) end
  end
end

function Game:on_login(e)
  self.ips[e.steamId] = e.ip
  if e.position then self.lastPos[e.steamId] = e.position end
  self.names = self.names or {}
  self.names[e.steamId] = e.name
  if e.kind == "login" then
    if self:is_banned(e.steamId) then
      self.log:info("banned player %s logged in; kicking", e.steamId)
      self.kickQueue[#self.kickQueue + 1] = { id = e.steamId, tries = 0 }
      return -- a refused login is not a session: no player-connected
    end
    if self:mark_online({ steamId = e.steamId }) then
      self.events:player_connected({ steamId = e.steamId, name = e.name, ip = e.ip })
    end
  else
    self.pendingDeaths[e.steamId] = nil
    self.alive[e.steamId] = nil
    if self.online[e.steamId] then
      self.online[e.steamId] = nil
      self.events:player_disconnected({ steamId = e.steamId, name = e.name, ip = e.ip })
    end
  end
end

function Game:chat_seen(id, msg, now)
  for k, t in pairs(self.recentChat) do if now - t > 10000 then self.recentChat[k] = nil end end
  local key = id .. "|" .. msg
  if self.recentChat[key] then return true end
  self.recentChat[key] = now
  return false
end

function Game:on_chat(id, name, msg, channel, now)
  if msg:sub(1, 1) == "#" then return end -- SCUM admin command, not chat
  if self:chat_seen(id, msg, now) then return end
  self.events:chat_message({ player = { steamId = id, name = name, ip = self.ips[id] }, msg = msg, channel = channel })
end

-- Deaths without a kill-log line (falls, zombies, animals, bleeding): AConZCharacter:IsAlive() polled at 1 Hz per
-- online prisoner (SCUM has no reflected death event; the kill log only records PvP kills).
function Game:poll_deaths(nowMs)
  local seen = {}
  for _, c in ipairs(self:controllers()) do
    seen[c.id] = true
    self:enforce_ban(c.id, c.pc, nowMs)
    local prisoner = self:pawn(c.pc)
    local okA, alive = false, nil
    if prisoner then okA, alive = pcall(function() return prisoner:IsAlive() end) end
    if okA and alive == true then
      local okL, v = pcall(function() return prisoner:K2_GetActorLocation() end)
      if okL and v then self.lastPos[c.id] = { x = v.X, y = v.Y, z = v.Z } end
      self.alive[c.id] = true
    elseif not prisoner then
      -- No prisoner: kicked, logging out or between characters. A dead prisoner stays as a corpse with
      -- IsAlive() == false, so only that counts as a death (a kick at 22:54:42Z produced a phantom death before).
      self.alive[c.id] = nil
    elseif okA and alive == false and self.alive[c.id] then
      self.alive[c.id] = false
      local name = str(c.pc:GetUserProfileName())
      self.pendingDeaths[c.id] = { at = nowMs, name = name, pos = self.lastPos[c.id] }
    end
  end
  for id in pairs(self.alive) do if not seen[id] then self.alive[id] = nil end end
  for id, d in pairs(self.pendingDeaths) do
    if self.recentKills[id] and nowMs - self.recentKills[id] < 15000 then
      self.pendingDeaths[id] = nil
    elseif nowMs - d.at >= M.DEATH_HOLD_MS then
      self.pendingDeaths[id] = nil
      self.events:player_death({ player = { steamId = id, name = d.name, ip = self.ips[id] }, position = d.pos })
    end
  end
end

-- A banned id is kicked as soon as its PlayerController exists (well before the prisoner spawns), once per 5 s.
function Game:enforce_ban(id, pc, nowMs)
  if not self.banned[id] then return end
  -- A ban of an online player is already scheduled: it shows the reason first (live 22:58:24Z this poll kicked 5 ms
  -- after the reason line, so the player never saw it).
  for _, k in ipairs(self.delayedKicks) do if k.id == id then return end end
  self.banKicks = self.banKicks or {}
  if self.banKicks[id] and nowMs - self.banKicks[id] < 5000 then return end
  self.banKicks[id] = nowMs
  local gm = self:game_mode()
  if not gm then return end
  local b = self.bans[id] or {}
  self.log:info("banned player %s has a controller; kicking before spawn", id)
  pcall(function() gm:KickPlayer(pc, "You are banned from this server" .. (b.reason and (": " .. b.reason) or "")) end)
end

function Game:on_kill(e, nowMs)
  self.recentKills[e.victimId] = nowMs or 0
  self.pendingDeaths[e.victimId] = nil
  local data = { player = { steamId = e.victimId, name = e.victimName }, position = e.victimPos }
  if e.killerId then data.attacker = { steamId = e.killerId, name = e.killerName } end
  self.events:player_death(data)
end

-- entity-killed. SCUM has no reflected NPC death event and the kill log records PvP only, so:
--  * attack hooks (server RPCs the client sends when it shoots, melees or blows something up) record who attacked
--    and when (hook body: two reads);
--  * a liveness scan of every non-player AConZCharacter (FindAllOf: ~1 ms for 50 NPCs) every 2 s, every 0.5 s
--    while someone attacked recently, sees alive -> dead transitions;
--  * a death is credited to the nearest player who attacked within ATTACK_WINDOW_MS and is within KILL_RADIUS. The
--    weapon is the NPC's _weaponClassLastHitBy (the game's own record), as a display name from the item catalogue.
--    Without a credited player or a weapon there is no event (never a placeholder).
function Game:on_attack_rpc(ctx, fn)
  local obj = ctx:get()
  local pc = fn:find("PlayerRpcChannel", 1, true) and obj:GetOuter() or obj.Controller
  if not valid(pc) then return end
  local id = str(pc:GetUserId())
  if not id or not id:match("^7656%d+$") then return end
  local now = self:now_ms()
  self.attacks[id] = now
  -- First attack after a quiet spell: scan on the next tick so the target's alive state is known before it dies.
  if not self.lastAttackAt or now - self.lastAttackAt > M.ATTACK_WINDOW_MS then self.lastNpcScan = -1e9 end
  self.lastAttackAt = now
end

function Game:entity_name(code)
  for _, kind in ipairs({ "entities", "items" }) do
    local c = self.catalogs[kind]
    if c and c.done then
      c.byCode = c.byCode or {}
      if not c.indexed then
        for _, e in ipairs(c.entries) do c.byCode[e.code] = e.name end
        c.indexed = true
      end
      if c.byCode[code] then return c.byCode[code] end
    end
  end
  return nil
end

function Game:scan_npcs(nowMs)
  local list = self.env.FindAllOf and self.env.FindAllOf("ConZCharacter") or {}
  local seen = {}
  self.npcClass = self.npcClass or {}
  for _, ch in ipairs(list) do
    if valid(ch) then
      local addr = ch:GetAddress()
      -- The class name is read once per object (UE4SS string calls dominate the scan cost).
      local cls = self.npcClass[addr]
      if not cls then
        cls = str(ch:GetClass():GetFName()) or ""
        self.npcClass[addr] = cls
      end
      seen[addr] = true
      if not cls:find("Prisoner", 1, true) then
        local key = addr
        seen[key] = true
        local okA, alive = pcall(function() return ch:IsAlive() end)
        if okA then
          local prev = self.npcs[key]
          if alive then
            self.npcs[key] = true
          elseif prev == true then
            self.npcs[key] = false
            self:on_npc_death(ch, cls, nowMs)
          elseif prev == nil then
            self.npcs[key] = false -- first seen dead: an old corpse
          end
        end
      end
    end
  end
  for k in pairs(self.npcs) do if not seen[k] then self.npcs[k] = nil end end
  for k in pairs(self.npcClass) do if not seen[k] then self.npcClass[k] = nil end end
end

function Game:on_npc_death(ch, cls, nowMs)
  local code = cls:gsub("_C$", "")
  local entity = self:entity_name(code) or M.humanize(code)
  local okW, w = pcall(function() return ch._weaponClassLastHitBy end)
  local weaponCode = okW and valid(w) and self:item_code(str(w:GetFName()) or "") or nil
  local okP, at = pcall(function() return ch:K2_GetActorLocation() end)
  local best, bestD
  for _, c in ipairs(self:controllers()) do
    local t = self.attacks[c.id]
    if t and nowMs - t <= M.ATTACK_WINDOW_MS then
      local pawn = self:pawn(c.pc)
      local d = 0
      if pawn and okP and at then
        local v = pawn:K2_GetActorLocation()
        d = math.sqrt((v.X - at.X) ^ 2 + (v.Y - at.Y) ^ 2 + (v.Z - at.Z) ^ 2)
      end
      if d <= M.KILL_RADIUS and (not bestD or d < bestD) then best, bestD = c, d end
    end
  end
  if not best then
    -- Streamed-out / despawned animals also flip IsAlive (dozens at once): log only deaths the game tied to a weapon.
    if weaponCode then
      self.log:info("npc died: %s (%s), weapon %s, no attacking player nearby; no entity-killed", entity, code, weaponCode)
    end
    return
  end
  if not weaponCode or weaponCode == "" then
    self.log:info("npc died: %s killed near %s but the game recorded no weapon; no entity-killed", entity, best.id)
    return
  end
  local weapon = self:entity_name(weaponCode) or M.humanize(weaponCode)
  self.log:info("entity-killed: %s killed %s (%s) with %s (%s), %.0f m", best.id, entity, code, weapon, weaponCode,
    (bestD or 0) / 100)
  local name = str(best.pc:GetUserProfileName())
  self.events:entity_killed({ player = { steamId = best.id, name = name, ip = self.ips[best.id] }, entity = entity,
    weapon = weapon })
end

function Game:on_admin_line(line)
  self.events:log_line("[SCUM admin] " .. line)
end

-- Installs the chat RPC hook (live, with channel) and the log tailers (login, chat fallback, kill, admin).
-- Idempotent: called once per Lua start.
function Game:start_events(events)
  if self.eventsStarted then return { ok = true } end
  self.eventsStarted = true
  self.events = events
  self.tailers = {}
  for _, cat in ipairs(M.CATEGORIES) do
    self.tailers[cat] = scumlog.tailer({ dir = self.paths.logs, category = cat, fs = self.fs })
  end
  local hooked = false
  if type(self.env.RegisterHook) == "function" then
    local ok, err = pcall(self.env.RegisterHook, M.UE.ChatRpc, function(ctx, message, channel)
      local okc, e = pcall(function()
        local ch = ctx:get()
        local pc = ch:GetOuter()
        local id = str(pc:GetUserId())
        if not id then return end
        local msg = str(message:get()) or ""
        local chan = channel:get()
        local name = str(pc:GetUserProfileName())
        self.chatHookFired = (self.chatHookFired or 0) + 1
        if self.chatHookFired == 1 then self.log:info("chat RPC hook fired (first message from %s)", id) end
        self:on_chat(id, name, msg, M.CHAT_TO_TAKARO[chan] or "global", self.env.now_ms and self.env.now_ms() or 0)
      end)
      if not okc then self.log:warn("chat hook: %s", tostring(e)) end
    end)
    hooked = ok
    if not ok then self.log:warn("chat hook not installed: %s", tostring(err)) end
  end
  -- A Lua hot reload (or a connector restart) with players already online: they are known to Takaro already, so
  -- they are marked online silently instead of being re-announced by the next getPlayers.
  local okS, list = pcall(self.controllers, self)
  if okS then
    for _, c in ipairs(list) do self.online[c.id] = true end
    if #list > 0 then self.log:info("start: %d player(s) already online; not re-announcing them", #list) end
  end
  self.attackHooks = 0
  if type(self.env.RegisterHook) == "function" then
    for _, fn in ipairs(M.ATTACK_HOOKS) do
      local okH = pcall(self.env.RegisterHook, fn, function(ctx)
        pcall(self.on_attack_rpc, self, ctx, fn)
      end)
      if okH then self.attackHooks = self.attackHooks + 1 else self.log:warn("attack hook not installed: %s", fn) end
    end
  end
  self.log:info("events: chat RPC hook %s; %d/%d attack hooks; log tailers %s in %s", hooked and "installed" or "missing",
    self.attackHooks, #M.ATTACK_HOOKS, table.concat(M.CATEGORIES, ","), self.paths.logs)
  return { ok = true }
end

-- Called every app tick (game thread, ~10 Hz). Keeps game-thread work small and measured.
function Game:tick(nowMs)
  local t0 = self:now_us()
  -- Build the catalogues once the world exists, so Takaro's item/entity sync finds them ready.
  if not self.catalogKicked and nowMs - (self.lastWorldCheck or 0) >= 1000 then
    self.lastWorldCheck = nowMs
    if self:game_mode() then
      self.catalogKicked = true
      pcall(self.catalog_start, self, "items")
      pcall(self.catalog_start, self, "entities")
    end
  end
  for _, kind in ipairs({ "items", "entities" }) do
    local c = self.catalogs[kind]
    if c and c.started and not c.done then self:catalog_step(kind, M.CATALOG_BUDGET_US) end
  end
  if self.tailers and nowMs - self.lastTail >= M.TAIL_EVERY_MS then
    self.lastTail = nowMs
    self:poll_logs(nowMs)
  end
  if #self.handQueue > 0 then self:process_hand_queue(nowMs) end
  if #self.delayedKicks > 0 then self:process_delayed_kicks(nowMs) end
  if self.events then
    local recent = self.lastAttackAt and nowMs - self.lastAttackAt < M.ATTACK_WINDOW_MS
    if nowMs - self.lastNpcScan >= (recent and M.NPC_SCAN_FAST_MS or M.NPC_SCAN_MS) then
      self.lastNpcScan = nowMs
      local okN, errN = pcall(self.scan_npcs, self, nowMs)
      if not okN then self.log:warn("npc scan: %s", tostring(errN)) end
    end
  end
  if self.events and nowMs - self.lastDeathPoll >= M.DEATH_POLL_MS then
    self.lastDeathPoll = nowMs
    local okD, errD = pcall(self.poll_deaths, self, nowMs)
    if not okD then self.log:warn("death poll: %s", tostring(errD)) end
  end
  if #self.kickQueue > 0 then
    local keep = {}
    for _, k in ipairs(self.kickQueue) do
      local pc = self:find_pc(k.id)
      local gm = self:game_mode()
      if pc and gm then
        gm:KickPlayer(pc, "You are banned from this server")
      elseif k.tries < 30 then
        k.tries = k.tries + 1
        keep[#keep + 1] = k
      end
    end
    self.kickQueue = keep
  end
  if nowMs - self.lastBanCheck >= M.BAN_CHECK_EVERY_MS then
    self.lastBanCheck = nowMs
    local gm = self:game_mode()
    if gm then
      local okK, key = pcall(function() return tostring(gm:GetAddress()) end)
      pcall(self.note_game_loaded_bans, self, okK and key or "?")
    end
    self:expire_bans(os.time())
    self:refresh_banned()
    if next(self.pendingUnbans) then pcall(self.process_pending_unbans, self) end
  end
  local dt = self:now_us() - t0
  self.perf.ticks = self.perf.ticks + 1
  self.perf.totalUs = self.perf.totalUs + dt
  if dt > self.perf.maxUs then self.perf.maxUs = dt end
end

function Game:poll_logs(nowMs)
  for _, cat in ipairs(M.CATEGORIES) do
    local okp, lines = pcall(self.tailers[cat].poll, self.tailers[cat], 2)
    if okp then
      for _, line in ipairs(lines) do
        local ok, err = pcall(function()
          if cat == "login" then
            local e = scumlog.parse_login(line)
            if e then self:on_login(e) end
          elseif cat == "chat" then
            local e = scumlog.parse_chat(line)
            if e then self:on_chat(e.steamId, e.name, e.msg, e.channel, nowMs) end
          elseif cat == "kill" then
            local e = scumlog.parse_kill(line)
            if e then self:on_kill(e, nowMs) end
          elseif cat == "admin" then
            if scumlog.parse_admin(line) then self:on_admin_line(line) end
          end
        end)
        if not ok then self.log:warn("%s log line failed: %s", cat, tostring(err)) end
      end
    else
      self.log:warn("%s tailer: %s", cat, tostring(lines))
    end
  end
end

function Game:perf_summary()
  local p = self.perf
  local avg = p.ticks > 0 and math.floor(p.totalUs / p.ticks) or 0
  return string.format("gameThread ticks=%d avgUs=%d maxUs=%d catalogMs=%d classLoads=%d", p.ticks, avg, p.maxUs,
    math.floor(p.catalogUs / 1000), p.loads)
end

M.ACTION_FUNCTIONS = {
  "reachability", "list_players", "get_player", "get_player_location", "get_player_inventory", "list_items",
  "list_entities", "list_locations", "list_bans", "execute_command", "send_message", "give_item",
  "teleport_player", "kick_player", "ban_player", "unban_player", "shutdown",
}

M.Game = Game
M.not_implemented = not_implemented

return M
