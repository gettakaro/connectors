-- SCUM game adapter against a fake UE4SS world (fake ConZPlayerController, MiscStatics, GameMode, KSL, logs).
local t = require("lib.t")
local game = require("takaro.game")
local json = require("takaro.json")

local MOD = "D:/scum/SCUM/Binaries/Win64/ue4ss/Mods/TakaroSCUM"
local LOGS = "D:/scum/SCUM/Saved/SaveFiles/Logs"
local CFG = "D:/scum/SCUM/Saved/Config/WindowsServer"
local ID1, ID2, ID3 = "76561198000000001", "76561198000000002", "76561198000000003"

local function u16(s)
  local out = {}
  for _, c in utf8.codes(s) do out[#out + 1] = string.char(c & 0xFF, c >> 8) end
  return table.concat(out)
end

local function obj(fields)
  fields = fields or {}
  fields.IsValid = fields.IsValid or function() return true end
  return fields
end

local function world(opts)
  opts = opts or {}
  local W = { calls = {}, files = {}, now = 0, hooks = {}, spawned = {}, loads = 0, dead = {} }
  local function rec(name, ...) W.calls[#W.calls + 1] = { name = name, args = { ... } } end
  W.gm = obj({
    KickPlayer = function(_, pc, reason) rec("KickPlayer", pc.id, reason) end,
    SendHUDMessageToAll = function(_, text, beep) rec("SendHUDMessageToAll", text, beep) end,
  })
  W.ms = obj({
    BroadcastChatLine = function(_, ctx, text, chatType) rec("BroadcastChatLine", ctx == W.gm, text, chatType) end,
    SendChatLineToPlayer = function(_, pc, text, chatType, clip) rec("SendChatLineToPlayer", pc.id, text, chatType, clip) end,
  })
  W.pcs = {}
  W.npcs = {}
  W.iucs, W.invs = {}, {}
  W.invFull = false
  function W.add_npc(cls, x)
    local n = { cls = cls, alive = true, weapon = nil, addr = 5000 + #W.npcs }
    n.o = obj({
      GetClass = function() return { GetFName = function() return cls end } end,
      GetAddress = function() return n.addr end,
      IsAlive = function() return n.alive end,
      K2_GetActorLocation = function() return { X = x or 0, Y = 0, Z = 0 } end,
    })
    setmetatable(n.o, { __index = function(_, k)
      if k == "_weaponClassLastHitBy" then
        return n.weapon and obj({ GetFName = function() return n.weapon end }) or obj({ IsValid = function() return false end })
      end
    end })
    W.npcs[#W.npcs + 1] = n
    return n
  end
  function W.add_pc(id, name, admin)
    local pc
    local pawn
    pawn = obj({
      GetFullName = function() return "BP_Prisoner_C_" .. id end,
      GetClass = function() return { GetFName = function() return "BP_Prisoner_C" end } end,
      GetAddress = function() return tonumber(id:sub(-4)) end,
      K2_GetActorLocation = function() return { X = 10, Y = 20, Z = 30 } end,
      PlaceItemInInventoryOrHolster = function(_, item) rec("Place", item.code) end,
      GetItemInHands = function() return nil end,
      TakeItemInHands = function(_, item) rec("TakeItemInHands", item.code) return true end,
      GetAttachedActors = function() end,
      IsAlive = function() return W.dead[id] ~= true end,
    })
    local channel = obj({ Chat_Server_ProcessAdminCommand = function(_, text) rec("ProcessAdminCommand", id, text) end })
    pc = obj({
      id = id,
      GetUserId = function() return id end,
      GetUserProfileName = function() return name end,
      PlayerState = obj({ Ping = 7, GetPlayerName = function() return name end }),
      GetPrisoner = function() return pawn end,
      IsUserAdmin = function() return admin == true end,
      GetPlayerRpcChannel = function() return channel end,
      Teleport = function(_, req) rec("Teleport", id, req.TargetLocation.X, req.TargetLocation.Y, req.TargetLocation.Z) end,
      GetFullName = function() return "BP_ConZPlayerController_C_" .. id end,
    })
    pawn.Controller = pc
    W.pcs[#W.pcs + 1] = pc
    local inv = obj({ GetOuter = function() return pawn end })
    W.invs[#W.invs + 1] = inv
    W.iucs[#W.iucs + 1] = obj({
      GetOuter = function() return pc end,
      Server_CharacterInventoryComponent_PickupItem = function(_, i, item)
        rec("PickupItem", id, item.code, i == inv)
        if not W.invFull and i == inv then item.owner = pawn end
      end,
    })
    return pc
  end
  local assets = opts.assets or {
    Item = { { "Weapon_M9", "TEC01 M9", "A pistol" }, { "Christmas_Paper_01", "Decoration Paper" }, { "Base_Item", "" } },
    Zombie = { { "BP_Zombie_Civilian", "" } },
    ArmedNPC = { { "BP_Drifter_Lvl_1", "" } },
    Animal = { { "BP_Bear", "", nil, "Bear" }, { "BP_Deer", "", nil, "Deer" }, { "BP_Wolf_Mutant", "", nil, "Wolf" },
      { "BP_BTBaseAnimal", "" } },
  }
  W.ksl = obj({
    GetEngineVersion = function() return "4.27.2-161892+++scum+release-1.3.4" end,
    GetPrimaryAssetIdList = function(_, typ, out)
      for _, a in ipairs(assets[typ.Name] or {}) do out[#out + 1] = { PrimaryAssetName = a[1] } end
    end,
    GetSoftClassReferenceFromPrimaryAssetId = function(_, id) return { type = id.PrimaryAssetType.Name, code = id.PrimaryAssetName } end,
    LoadClassAsset_Blocking = function(_, sc)
      W.loads = W.loads + 1
      W.now = W.now + 1500 -- each load costs 1.5 ms of fake time
      for _, a in ipairs(assets[sc.type] or {}) do
        if a[1] == sc.code then
          return obj({ code = a[1], GetCDO = function()
            return { GetInteractableName = function() return a[2] end, GetDescription = function() return a[3] or "" end,
              SpeciesName = a[4] }
          end })
        end
      end
      return obj({ IsValid = function() return false end })
    end,
    QuitGame = function() rec("QuitGame") end,
  })
  W.gps = obj({
    BeginDeferredActorSpawnFromClass = function(_, _ctx, cls)
      local it
      it = obj({ code = cls.code, GetOwner = function() return it.owner end })
      return it
    end,
    FinishSpawningActor = function(_, a) W.spawned[#W.spawned + 1] = a.code; return a end,
  })
  W.env = {
    FindFirstOf = function(n) if n == "ConZGameMode" and not opts.noWorld then return W.gm end end,
    FindAllOf = function(n)
      if n == "ConZPlayerController" then return W.pcs end
      if n == "InventoryUserComponent" then return W.iucs end
      if n == "PrisonerInventoryComponent" then return W.invs end
      if n == "ConZCharacter" then
        local out = {}
        for _, pc in ipairs(W.pcs) do out[#out + 1] = pc:GetPrisoner() end
        for _, npc in ipairs(W.npcs) do out[#out + 1] = npc.o end
        return out
      end
    end,
    StaticFindObject = function(p)
      local cls = p:match("^/Script/SCUM%.(%w+)$")
      if cls then return obj({ name = cls }) end
      if p:find("MiscStatics") then return W.ms end
      if p:find("KismetSystemLibrary") then return W.ksl end
      if p:find("GameplayStatics") then return W.gps end
      return nil
    end,
    RegisterHook = function(path, fn) W.hooks[path] = fn end,
    FName = function(s) return s end,
    now_us = function() return W.now end,
    now_ms = function() return W.now // 1000 end,
  }
  W.fs = {
    list = function(dir)
      local out = {}
      for p in pairs(W.files) do
        local d, n = p:match("^(.*)/([^/]+)$")
        if d == dir then out[#out + 1] = n end
      end
      return out
    end,
    size = function(p) return W.files[p] and #W.files[p] or nil end,
    read_at = function(p, off, n) return W.files[p] and W.files[p]:sub(off + 1, off + n) end,
    read = function(p) return W.files[p] end,
    write = function(p, s) W.files[p] = s; return true end,
  }
  W.logs = {}
  W.log = {}
  for _, lvl in ipairs({ "info", "warn", "error", "debug" }) do
    W.log[lvl] = function(_, fmt, ...) W.logs[#W.logs + 1] = lvl .. " " .. string.format(fmt, ...) end
  end
  W.events = { got = {} }
  for _, e in ipairs({ "player_connected", "player_disconnected", "chat_message", "player_death", "entity_killed",
    "log_line" }) do
    W.events[e] = function(self, data) self.got[#self.got + 1] = { e, data } end
  end
  W.g = game.new(W.env, W.log, { mod_dir = MOD, fs = W.fs })
  function W.find(name)
    local out = {}
    for _, c in ipairs(W.calls) do if c.name == name then out[#out + 1] = c.args end end
    return out
  end
  return W
end

t.test("paths are derived from the mod folder (Saved/SaveFiles/Logs, Saved/Config/WindowsServer)", function()
  local p = game.paths("D:\\scum\\SCUM\\Binaries\\Win64\\ue4ss\\Mods\\TakaroSCUM")
  t.eq(p.logs, LOGS)
  t.eq(p.config, CFG)
end)

t.test("testReachability is the real in-process state (world loaded or not)", function()
  t.deep(world().g:reachability().value, { connectable = true })
  t.deep(world({ noWorld = true }).g:reachability().value,
    { connectable = false, reason = "SCUM world is not loaded (no ConZGameMode)" })
end)

t.test("getPlayers lists controllers with a SteamID64, name, ping and the login-log IP", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  W.pcs[#W.pcs + 1] = obj({ GetUserId = function() return "" end }) -- a controller without a user (drone/menu)
  W.g:start_events(W.events)
  W.g:on_login({ kind = "login", steamId = ID1, name = "Alice", ip = "10.0.0.5" })
  local r = W.g:list_players()
  t.deep(r.value, { { steamId = ID1, name = "Alice", ping = 28, ip = "10.0.0.5" } })
end)

t.test("sendMessage: global via BroadcastChatLine(ServerMessage), directed via SendChatLineToPlayer, sender prefix", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  t.ok(W.g:send_message({ message = "hello all" }).ok)
  t.deep(W.find("BroadcastChatLine")[1], { true, "hello all", 6 })
  t.ok(W.g:send_message({ message = "psst", recipientGameId = ID1, senderName = "Takaro" }).ok)
  t.deep(W.find("SendChatLineToPlayer")[1], { ID1, "Takaro: psst", 6, false })
  t.eq(#W.find("BroadcastChatLine"), 1, "directed message never broadcasts")
  local off = W.g:send_message({ message = "x", recipientGameId = ID2 })
  t.deep(off, { ok = false, reason = "recipient is offline" })
end)

t.test("kick: the reason is shown in chat first, KickPlayer follows 2 s later, then Takaro gets the answer", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  local answer
  local r = W.g:kick_player({ gameId = ID1, reason = "bye" }, function(res) answer = res end)
  t.eq(r, nil, "answered later")
  t.deep(W.find("SendChatLineToPlayer")[1], { ID1, "You are kicked: bye", 6, false })
  t.eq(#W.find("KickPlayer"), 0)
  W.g:tick(W.g:now_ms() + 1000)
  t.eq(#W.find("KickPlayer"), 0, "still showing the reason")
  W.g:tick(W.g:now_ms() + 2100)
  t.deep(W.find("KickPlayer")[1], { ID1, "bye" })
  t.deep(answer, { ok = true })
  t.eq(W.g:kick_player({ gameId = ID2 }).ok, false)
end)

t.test("location and teleport go through the prisoner and ConZPlayerController:Teleport", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  t.deep(W.g:get_player_location({ gameId = ID1 }).value, { x = 10, y = 20, z = 30 })
  t.ok(W.g:teleport_player({ gameId = ID1, x = 1, y = 2, z = 3 }).ok)
  t.deep(W.find("Teleport")[1], { ID1, 1, 2, 3 })
end)

t.test("executeConsoleCommand: announce/listplayers work with nobody online; other verbs need an online admin", function()
  local W = world()
  local r = W.g:execute_command({ command = "#announce Server restart soon" })
  t.deep(r, { ok = true, value = "announced: Server restart soon" })
  t.deep(W.find("SendHUDMessageToAll")[1], { "Server restart soon", false })
  t.eq(W.g:execute_command({ command = "listplayers" }).value, "no players online")
  local no = W.g:execute_command({ command = "SpawnItem Weapon_M9 1" })
  t.eq(no.ok, false)
  t.ok(no.reason:find("^no_executor"), no.reason)
  W.add_pc(ID1, "Player")
  W.add_pc(ID2, "Admin", true)
  local yes = W.g:execute_command({ command = "SpawnItem Weapon_M9 1" })
  t.ok(yes.ok)
  t.deep(W.find("ProcessAdminCommand")[1], { ID2, "SpawnItem Weapon_M9 1" }, "the channel wants no '#'")
end)

t.test("ban writes BannedUsers.ini + ban store and kicks; unban keeps other lines; listBans has reason and expiry", function()
  local W = world()
  W.files[CFG .. "/BannedUsers.ini"] = ID3 .. "\r\n"
  W.add_pc(ID1, "Alice")
  W.g.names = { [ID1] = "Alice" }
  local answer
  t.eq(W.g:ban_player({ gameId = ID1, reason = "cheating", expiresAt = "2099-01-01T00:00:00.000Z" },
    function(res) answer = res end), nil)
  t.eq(W.files[CFG .. "/BannedUsers.ini"], ID3 .. "\r\n" .. ID1 .. "\r\n")
  t.deep(W.find("SendChatLineToPlayer")[1], { ID1, "You are banned: cheating", 6, false })
  W.g:tick(W.g:now_ms() + 2100)
  t.deep(W.find("KickPlayer")[1], { ID1, "cheating" })
  t.ok(answer.ok)
  t.ok(W.g:ban_player({ gameId = ID2, name = "Bob (from Takaro)" }).ok, "offline ban")
  local bans = W.g:list_bans().value
  t.eq(#bans, 3)
  t.deep(bans[1], { player = { steamId = ID1, name = "Alice" }, reason = "cheating", expiresAt = "2099-01-01T00:00:00.000Z" })
  t.deep(bans[2].player, { steamId = ID2, name = "Bob (from Takaro)" }, "Takaro's own name, never the raw id")
  t.ok(W.g:unban_player({ gameId = ID1 }).ok)
  t.eq(W.files[CFG .. "/BannedUsers.ini"], ID3 .. "\r\n" .. ID2 .. "\r\n")
  t.ok(W.g:unban_player({ gameId = ID2 }).ok, "offline unban")
  t.eq(W.files[CFG .. "/BannedUsers.ini"], ID3 .. "\r\n")
  local store = json.decode(W.files[MOD .. "/takaro-bans.json"])
  t.eq(next(store.bans), nil)
end)

t.test("banning an online player: the controller poll does not pre-empt the reason line", function()
  local W = world()
  W.g:start_events(W.events)
  W.add_pc(ID1, "Alice")
  W.g:tick(1000)
  W.now = 1000 * 1000
  W.g:ban_player({ gameId = ID1, reason = "spam" }, function() end) -- kick due at 3000 ms
  W.g:tick(2100) -- the 1 Hz controller/ban poll runs here: it must not kick yet
  t.eq(#W.find("KickPlayer"), 0)
  W.g:tick(W.g:now_ms() + 2100)
  t.deep(W.find("KickPlayer")[1], { ID1, "spam" })
end)

t.test("a banned id is kicked as soon as its controller exists (before spawn), once per 5 s", function()
  local W = world()
  W.g:start_events(W.events)
  t.ok(W.g:ban_player({ gameId = ID2, reason = "griefing" }).ok)
  W.add_pc(ID2, "Bob")
  W.g:tick(1000)
  t.deep(W.find("KickPlayer")[1], { ID2, "You are banned from this server: griefing" })
  W.g:tick(2000)
  t.eq(#W.find("KickPlayer"), 1, "not every second")
  t.eq(#W.events.got, 0, "a refused player is never announced")
end)

t.test("unban: a ban SCUM loaded at server start is lifted with #UnbanPlayer through another online admin", function()
  local W = world()
  W.files[CFG .. "/BannedUsers.ini"] = ID1 .. "\r\n"
  W.g:start_events(W.events)
  W.g:tick(10000) -- first ban check after the world exists records what SCUM loaded
  t.ok(W.g.gameLoadedBans[ID1])
  local r = W.g:unban_player({ gameId = ID1 })
  t.ok(r.ok)
  t.ok(r.value:find("UnbanPlayer will also run"), r.value)
  t.eq(W.files[CFG .. "/BannedUsers.ini"], "")
  t.ok(not W.g:is_banned(ID1), "the connector stops refusing at once")
  W.add_pc(ID3, "Admin", true)
  W.g:tick(20000)
  t.deep(W.find("ProcessAdminCommand")[1], { ID3, "UnbanPlayer " .. ID1 })
  t.eq(W.g.pendingUnbans[ID1], nil)
  -- a ban added while the server runs is not in SCUM's memory: no admin command needed
  W.g:ban_player({ gameId = ID2 })
  local r2 = W.g:unban_player({ gameId = ID2 })
  t.eq(r2.value, nil)
  t.eq(#W.find("ProcessAdminCommand"), 1)
  -- a hot reload keeps the server-start set (same game mode)
  local W2 = world()
  W2.files, W2.g.fs = W.files, W.fs
  W2.gm = W.gm
  W2.g:load_bans()
  t.eq(W2.g.banBootKey, W.g.banBootKey)
end)

t.test("a timed ban is lifted by the connector when expiresAt passes (Takaro sends no unban)", function()
  local W = world()
  W.g:ban_player({ gameId = ID1, expiresAt = "2026-10-13T10:00:00.000Z" })
  W.g:expire_bans(game.iso_to_epoch("2026-10-13T09:59:59Z"))
  t.ok(W.files[CFG .. "/BannedUsers.ini"]:find(ID1), "not yet")
  W.g:expire_bans(game.iso_to_epoch("2026-10-13T10:00:01Z"))
  t.eq(W.files[CFG .. "/BannedUsers.ini"]:find(ID1), nil)
  t.eq(game.iso_to_epoch("1970-01-01T00:01:00Z"), 60)
end)

t.test("catalogues: built within a per-tick budget, human names, humanized fallback, entity types, disk cache", function()
  local W = world()
  W.g:catalog_start("items")
  local c = W.g.catalogs.items
  t.eq(c.total, 3)
  W.g:catalog_step("items", 2000) -- 2 ms budget, loads cost 1.5 ms: two loads (the check is after each load)
  t.eq(#c.entries, 2)
  W.g:catalog_step("items", 2000)
  t.ok(c.done)
  t.deep(c.entries, {
    { code = "Base_Item", name = "Base Item" },
    { code = "Christmas_Paper_01", name = "Decoration Paper" },
    { code = "Weapon_M9", name = "TEC01 M9", description = "A pistol" },
  })
  t.eq(c.humanized, 1)
  W.g:catalog_start("entities")
  W.g:catalog_step("entities", 100000)
  local byCode = {}
  for _, e in ipairs(W.g.catalogs.entities.entries) do byCode[e.code] = e end
  t.deep(byCode.BP_Zombie_Civilian, { code = "BP_Zombie_Civilian", name = "Zombie Civilian", type = "hostile" })
  t.deep(byCode.BP_Drifter_Lvl_1, { code = "BP_Drifter_Lvl_1", name = "Drifter Lvl 1", type = "hostile" })
  t.deep(byCode.BP_Bear, { code = "BP_Bear", name = "Bear", type = "hostile" })
  t.deep(byCode.BP_Deer, { code = "BP_Deer", name = "Deer", type = "neutral" })
  t.deep(byCode.BP_Wolf_Mutant, { code = "BP_Wolf_Mutant", name = "Wolf (Mutant)", type = "hostile" })
  t.eq(byCode.BP_BTBaseAnimal, nil, "abstract base class excluded")
  -- second start: cache hit, no class loads
  local W2 = world()
  W2.files = W.files
  W2.g.fs = W.fs
  local loads = W.loads
  W2.g:catalog_start("items")
  t.ok(W2.g.catalogs.items.done)
  t.eq(#W2.g.catalogs.items.entries, 3)
  t.eq(W2.loads, 0)
  t.eq(W.loads, loads)
end)

t.test("catalogue entries never carry blueprint class names as names", function()
  local W = world()
  W.g:catalog_start("items"); W.g:catalog_step("items", 1e9)
  W.g:catalog_start("entities"); W.g:catalog_step("entities", 1e9)
  for _, kind in ipairs({ "items", "entities" }) do
    for _, e in ipairs(W.g.catalogs[kind].entries) do
      t.ok(e.name ~= e.code and not e.name:find("^BP") and not e.name:find("_C$") and not e.name:find("_"), e.name)
    end
  end
end)

t.test("giveItem: every item goes into the inventory through the player's own pickup RPC, read back by owner", function()
  local W = world()
  W.add_pc(ID2, "Bob")
  W.add_pc(ID1, "Alice")
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 3 })
  t.ok(r.ok, r.reason)
  t.deep(W.spawned, { "Weapon_M9", "Weapon_M9", "Weapon_M9" })
  t.eq(#W.find("PickupItem"), 3)
  t.deep(W.find("PickupItem")[1], { ID1, "Weapon_M9", true }, "Alice's controller component + Alice's inventory")
  t.ok(r.value:find("3 in inventory, 0 at the player's feet"), r.value)
  W.g:tick(5000)
  t.eq(#W.find("TakeItemInHands"), 0, "nothing for the hands")
end)

-- Fake UE classes: an object "is a" class when its isa set names it (IsA takes the class object from StaticFindObject).
local function isa(o, ...)
  o.isa = {}
  for _, n in ipairs({ ... }) do o.isa[n] = true end
  o.IsA = function(self, cls) return self.isa[cls.name] == true end
  return o
end

local function stack(q)
  local c = isa(obj({ _repQuantity = q, get = "TrivialObject" }), "StackableItemComponent", "DiscreteAmountItemComponent")
  return { c }
end

t.test("giveItem: a stackable merging into one stack (picked-up actor left at quantity 0) counts as delivered", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  W.invFull = true
  local orig = W.gps.BeginDeferredActorSpawnFromClass
  W.gps.BeginDeferredActorSpawnFromClass = function(...)
    local it = isa(orig(...), "Item")
    it._itemComponents = stack(0)
    return it
  end
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 5 })
  t.ok(r.value:find("5 in inventory, 0 at the player's feet"), r.value)
end)

t.test("giveItem: an empty weapon or magazine (GetAmmoCount 0, no stack) is NOT mistaken for a merged stack", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  W.invFull = true
  local orig = W.gps.BeginDeferredActorSpawnFromClass
  W.gps.BeginDeferredActorSpawnFromClass = function(...)
    local it = isa(orig(...), "Item")
    it.GetAmmoCount = function() return 0 end
    it._itemComponents = {}
    return it
  end
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 1 })
  t.ok(r.value:find("0 in inventory, 1 at the player's feet %(first one into empty hands%)"), r.value)
end)

t.test("giveItem: clothes go onto a free body slot (EquipClothes) before the pickup", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local pawn = pc:GetPrisoner()
  W.invFull = true
  local orig = W.gps.BeginDeferredActorSpawnFromClass
  W.gps.BeginDeferredActorSpawnFromClass = function(...) return isa(orig(...), "Item", "ClothesItem") end
  W.iucs[1].Server_CharacterInventoryComponent_EquipClothes = function(_, _inv, item)
    W.calls[#W.calls + 1] = { name = "EquipClothes", args = { item.code } }
    if not W.slotTaken then item.owner = pawn; W.slotTaken = true end
  end
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 2 })
  t.ok(r.value:find("1 in inventory %(1 worn%), 1 at the player's feet"), r.value)
  t.eq(#W.find("EquipClothes"), 2, "tried for both; the second slot was taken")
  t.eq(#W.find("PickupItem"), 1, "only the one that was not worn is picked up")
end)

t.test("giveItem: an EquipClothes swap (the earlier item moves into a container) counts one worn, both delivered", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local pawn = pc:GetPrisoner()
  local backpack = obj({ GetFullName = function() return "Backpack_1" end, GetOwner = function() return pawn end })
  local orig = W.gps.BeginDeferredActorSpawnFromClass
  W.gps.BeginDeferredActorSpawnFromClass = function(...) return isa(orig(...), "Item", "ClothesItem") end
  W.iucs[1].Server_CharacterInventoryComponent_EquipClothes = function(_, _inv, item)
    if W.wornNow then W.wornNow.owner = backpack end
    item.owner = pawn
    W.wornNow = item
  end
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 2 })
  t.ok(r.value:find("2 in inventory %(1 worn%), 0 at the player's feet"), r.value)
end)

t.test("giveItem: Takaro quality (durability percent) is applied with SetHealth; null/''/junk keeps the default", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  local set = {}
  local orig = W.gps.BeginDeferredActorSpawnFromClass
  W.gps.BeginDeferredActorSpawnFromClass = function(...)
    local it = orig(...)
    it.GetMaxHealth = function() return 200 end
    it.SetHealth = function(_, h) set[#set + 1] = h end
    return it
  end
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 1, quality = "50" })
  t.deep(set, { 100 })
  t.ok(r.value:find("durability 50%%"), r.value)
  for _, q in ipairs({ nil, "", "abc", "0", "101" }) do W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 1, quality = q }) end
  t.deep(set, { 100 }, "no SetHealth without a usable percent")
  t.eq(game.quality_percent(" 75 "), 75)
  t.eq(game.quality_percent(99.6), 100)
end)

t.test("giveItem with a full inventory: items stay at the feet, the first goes into empty hands later", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  W.invFull = true
  local r = W.g:give_item({ gameId = ID1, item = "Weapon_M9", amount = 2 })
  t.ok(r.ok, r.reason)
  t.ok(r.value:find("0 in inventory, 2 at the player's feet"), r.value)
  t.eq(#W.find("TakeItemInHands"), 0, "not right after the spawn (SCUM ignores it then)")
  W.now = W.now + 1000 * 1000 -- 1 s later (fake clock is in microseconds)
  W.g:tick(W.now // 1000)
  t.eq(#W.find("TakeItemInHands"), 0, "still waiting")
  W.now = W.now + 1000 * 1000
  W.g:tick(W.now // 1000)
  t.eq(#W.find("TakeItemInHands"), 1, "first item into empty hands, the rest at the player's feet")
  t.eq(W.g:give_item({ gameId = ID1, item = "Nope", amount = 1 }).ok, false)
end)

t.test("events: login/logout from the login log, state dedup, IP kept, banned login queued for a kick", function()
  local W = world()
  W.g:start_events(W.events)
  local f = LOGS .. "/login_20261013210000.log"
  W.files[f] = ""
  W.g:tick(1000) -- opens the file at its end
  W.files[f] = W.files[f] .. u16("2026.10.13-21.00.05: '10.0.0.5 " .. ID1 .. ":Alice(1)' logged in at: X=1 Y=2 Z=3\r\n")
  W.g:tick(3000)
  t.deep(W.events.got[1], { "player_connected", { steamId = ID1, name = "Alice", ip = "10.0.0.5" } })
  -- getPlayers sees the same player: no second connect
  W.add_pc(ID1, "Alice")
  W.g:list_players()
  t.eq(#W.events.got, 1)
  W.files[f] = W.files[f] .. u16("2026.10.13-21.10.05: '10.0.0.5 " .. ID1 .. ":Alice(1)' logged out at: X=1 Y=2 Z=3\r\n")
  W.g:tick(5000)
  t.eq(W.events.got[2][1], "player_disconnected")
  -- banned player logs in -> kicked once the controller exists
  W.g:ban_player({ gameId = ID2, reason = "x" })
  W.add_pc(ID2, "Bob")
  W.files[f] = W.files[f] .. u16("2026.10.13-21.20.05: '10.0.0.6 " .. ID2 .. ":Bob(2)' logged in at: X=1 Y=2 Z=3\r\n")
  W.g:tick(7000)
  W.g:tick(7100)
  local kicks = W.find("KickPlayer")
  t.deep(kicks[#kicks], { ID2, "You are banned from this server" })
end)

t.test("chat: the RPC hook and the chat log report one message once; # admin commands are not chat", function()
  local W = world()
  W.g:start_events(W.events)
  local hook = W.hooks["/Script/SCUM.PlayerRpcChannel:Chat_Server_BroadcastChatMessage"]
  t.ok(hook, "hook registered")
  local pc = W.add_pc(ID1, "Alice")
  local ch = obj({ GetOuter = function() return pc end })
  local function p(v) return { get = function() return v end } end
  hook(p(ch), p("hello"), p(3))
  t.deep(W.events.got[1], { "chat_message", { player = { steamId = ID1, name = "Alice" }, msg = "hello", channel = "team" } })
  hook(p(ch), p("#SpawnItem Weapon_M9 1"), p(2))
  t.eq(#W.events.got, 1)
  hook(p(ch), p("near me"), p(1))
  t.eq(W.events.got[2][2].channel, "friends", "SCUM Local (proximity) is not global")
  hook(p(ch), p("everyone"), p(2))
  t.eq(W.events.got[3][2].channel, "global")
  table.remove(W.events.got); table.remove(W.events.got)
  local f = LOGS .. "/chat_20261013210000.log"
  W.files[f] = ""
  W.g:tick(1000)
  W.files[f] = u16("2026.10.13-21.00.05: '" .. ID1 .. ":Alice(1)' 'Squad: hello'\r\n")
  W.g:tick(2500)
  t.eq(#W.events.got, 1, "log copy of a hooked message is dropped")
  W.files[f] = W.files[f] .. u16("2026.10.13-21.00.06: '" .. ID1 .. ":Alice(1)' 'Global: only in the log'\r\n")
  W.g:tick(4000)
  t.deep(W.events.got[2][2].msg, "only in the log")
end)

t.test("kill log -> player-death with attacker (players only) and victim position", function()
  local W = world()
  W.g:start_events(W.events)
  local f = LOGS .. "/kill_20261013210000.log"
  W.files[f] = ""
  W.g:tick(1000)
  W.files[f] = u16("2026.10.13-21.00.05: Died: Bob (" .. ID2 .. "), Killer: Alice (" .. ID1
    .. ") Weapon: Weapon_M9_C [Projectile] S:[KillerLoc : 1, 2, 3 VictimLoc: 4, 5, 6, Distance: 2.96 m]\r\n")
  W.g:tick(2500)
  t.deep(W.events.got[1], { "player_death", { player = { steamId = ID2, name = "Bob" },
    attacker = { steamId = ID1, name = "Alice" }, position = { x = 4, y = 5, z = 6 } } })
end)

-- A carried item: class name, display name, children (contents), durability and item components.
local function citem(cls, name, opts)
  opts = opts or {}
  local it = isa(obj({ GetFullName = function() return opts.full or (cls .. "_" .. tostring(opts.n or 1)) end,
    GetClass = function() return { GetFName = function() return cls end } end,
    GetInteractableName = function() return name end,
    GetHealth = function() return opts.hp or 100 end, GetMaxHealth = function() return opts.max or 100 end,
    GetOwner = function() return opts.owner end,
    Children = opts.children or {}, _itemComponents = opts.comps or {},
    -- UE4SS answers any unknown member of a UObject with a non-callable TrivialObject (live 2026-10-14: an
    -- `elem.get and elem:get()` unwrap raised on every child and the answer was [])
    get = "TrivialObject" }), table.unpack(opts.isa or { "Item" }))
  if opts.ammo then it.GetAmmoCount = function() return opts.ammo end end
  return it
end

t.test("inventory: the prisoner's Children tree = worn clothes, holsters, container contents, with stacks and durability", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local prisoner = pc:GetPrisoner()
  local backpack = citem("Backpack_02_01_C", "Backpack", { isa = { "Item", "ClothesItem" }, children = {
    citem("Cal_50_AE_C", ".50 AE Ammo", { comps = stack(13), isa = { "Item", "AmmunitionItem" }, ammo = 13 }),
    citem("Cal_9mm_C", "9mm Ammo", { comps = stack(20), isa = { "Item", "AmmunitionItem" } }),
    citem("Magazine_M9_C", "M9 Magazine", { ammo = 0 }),
    citem("Emergency_bandage_C", "Emergency Bandage", { comps = stack(2) }),
    citem("Apple_C", "Apple", { comps = { isa(obj({ _repQuantity = 1 }), "DiscreteAmountItemComponent") } }),
    citem("Weapon_M9_C", "TEC01 M9", { ammo = 0 }),
    citem("Spade_C", "Spade Shovel ", { hp = 37.4 }),
  } })
  local holster = citem("Belt_Pistol_Holster_C", "Leather Belt Pistol Holster", { children = {
    citem("Weapon_M1911_C", "M1911", { ammo = 7 }) } })
  local fx = obj({ GetFullName = function() return "Emitter_1" end, Children = {}, IsA = function() return false end })
  prisoner.Children = { citem("Inmate_shirt_01_C", "Shirt", { hp = 99.99 }), backpack, holster,
    citem("Sock_01_C", "Socks", { n = 1, hp = 50 }), citem("Sock_01_C", "Socks", { n = 2, hp = 50 }), fx }
  local inv = W.g:get_player_inventory({ gameId = ID1 }).value
  t.deep(inv, {
    { code = "Inmate_shirt_01", name = "Shirt", amount = 1, quality = "100" },
    { code = "Backpack_02_01", name = "Backpack", amount = 1, quality = "100" },
    { code = "Cal_50_AE", name = ".50 AE Ammo", amount = 13, quality = "100" },
    { code = "Cal_9mm", name = "9mm Ammo", amount = 20, quality = "100" },
    { code = "Magazine_M9", name = "M9 Magazine", amount = 1, quality = "100" },
    { code = "Emergency_bandage", name = "Emergency Bandage", amount = 2, quality = "100" },
    { code = "Apple", name = "Apple", amount = 1, quality = "100" },
    { code = "Weapon_M9", name = "TEC01 M9", amount = 1, quality = "100" },
    { code = "Spade", name = "Spade Shovel", amount = 1, quality = "37" },
    { code = "Belt_Pistol_Holster", name = "Leather Belt Pistol Holster", amount = 1, quality = "100" },
    { code = "Weapon_M1911", name = "M1911", amount = 1, quality = "100" },
    { code = "Sock_01", name = "Socks", amount = 2, quality = "50" },
  })
end)

t.test("inventory: the item in hands is added when it is not in the Children tree; different durability = own row", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local prisoner = pc:GetPrisoner()
  prisoner.Children = { citem("Weapon_M9_C", "TEC01 M9", { n = 1, hp = 80 }) }
  local inHands = citem("Weapon_M9_C", "TEC01 M9", { n = 2, hp = 100 })
  prisoner.GetItemInHands = function() return inHands end
  t.deep(W.g:get_player_inventory({ gameId = ID1 }).value, {
    { code = "Weapon_M9", name = "TEC01 M9", amount = 1, quality = "80" },
    { code = "Weapon_M9", name = "TEC01 M9", amount = 1, quality = "100" } })
  prisoner.Children = { inHands }
  t.deep(W.g:get_player_inventory({ gameId = ID1 }).value, {
    { code = "Weapon_M9", name = "TEC01 M9", amount = 1, quality = "100" } }, "not counted twice")
end)

t.test("inventory: a class whose case differs from its asset name reports the catalogue code (Takaro matches exactly)", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  pc:GetPrisoner().Children = { citem("Weapon_Deagle_357_C", "DEagle 357"), citem("Apple_C", "Apple") }
  t.eq(W.g:get_player_inventory({ gameId = ID1 }).value[1].code, "Weapon_Deagle_357", "no catalogue yet: class name")
  W.g.catalogs.items = { entries = { { code = "Apple", name = "Apple" }, { code = "Weapon_DEagle_357", name = "DEagle 357" } } }
  local inv = W.g:get_player_inventory({ gameId = ID1 }).value
  t.eq(inv[1].code, "Weapon_DEagle_357")
  t.eq(inv[2].code, "Apple")
end)

t.test("inventory guard: an empty walk while the world scan finds owned items is an error, never []; no character = error", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local prisoner = pc:GetPrisoner()
  prisoner.GetFullName = function() return "BP_Prisoner_C_1" end
  prisoner.Children = {}
  W.items = { citem("Inmate_shirt_01_C", "Shirt", { owner = prisoner }) }
  W.env.FindAllOf = function(n)
    if n == "ConZPlayerController" then return W.pcs end
    if n == "Item" then return W.items end
  end
  local r = W.g:get_player_inventory({ gameId = ID1 })
  t.eq(r.ok, false)
  t.ok(r.reason:find("1 items are owned"), r.reason)
  W.items = {}
  t.deep(W.g:get_player_inventory({ gameId = ID1 }), { ok = true, value = {} }, "really naked: []")
  pc.GetPrisoner = function() return nil end
  r = W.g:get_player_inventory({ gameId = ID1 })
  t.eq(r.ok, false, "no character: unknown state")
end)

t.test("inventory fallback (Children unreadable): items whose Owner chain reaches the prisoner, corpse items excluded", function()
  local W = world()
  local pc = W.add_pc(ID1, "Alice")
  local prisoner = pc:GetPrisoner()
  prisoner.GetFullName = function() return "BP_Prisoner_C_1" end
  local corpse = obj({ GetFullName = function() return "BP_PrisonerCorpse_C_2" end, GetOwner = function() return nil end })
  local shirt = citem("Inmate_shirt_01_C", "Shirt", { owner = prisoner, full = "shirt" })
  W.items = { shirt, citem("Credit_Card_Starter_C", "Starter Bank Card", { owner = shirt }),
    citem("Cal_9mm_C", "9mm Ammo", { owner = shirt, comps = stack(7) }), citem("Weapon_M9_C", "TEC01 M9", { owner = corpse }),
    citem("Apple_C", "Apple", {}) }
  W.env.FindAllOf = function(n)
    if n == "ConZPlayerController" then return W.pcs end
    if n == "Item" then return W.items end
  end
  local inv = W.g:get_player_inventory({ gameId = ID1 }).value
  t.deep(inv, { { code = "Inmate_shirt_01", name = "Shirt", amount = 1, quality = "100" },
    { code = "Credit_Card_Starter", name = "Starter Bank Card", amount = 1, quality = "100" },
    { code = "Cal_9mm", name = "9mm Ammo", amount = 7, quality = "100" } })
end)

t.test("death without a kill-log line: IsAlive poll -> player-death with last position after the hold time", function()
  local W = world()
  W.g:start_events(W.events)
  W.add_pc(ID1, "Alice")
  W.g:tick(1000)
  W.dead[ID1] = true
  W.g:tick(2000)
  t.eq(#W.events.got, 0, "held for a possible kill-log line")
  W.g:tick(5000)
  t.deep(W.events.got[1], { "player_death", { player = { steamId = ID1, name = "Alice" }, position = { x = 10, y = 20, z = 30 } } })
  W.g:tick(6000)
  t.eq(#W.events.got, 1, "one death, one event")
  W.dead[ID1] = nil
  W.g:tick(7000)
  W.dead[ID1] = true
  W.g:tick(8000)
  W.g:tick(11000)
  t.eq(#W.events.got, 2, "a second death after respawn is reported again")
end)

t.test("a kick or logout (prisoner gone) is never reported as a death", function()
  local W = world()
  W.g:start_events(W.events)
  local pc = W.add_pc(ID1, "Alice")
  W.g:tick(1000)
  pc.GetPrisoner = function() return nil end -- kicked: the pawn is destroyed before the controller goes
  pc.Pawn = nil
  W.g:tick(2000)
  W.g:tick(6000)
  t.eq(#W.events.got, 0)
  -- logout while a death was pending clears it as well
  local W2 = world()
  W2.g:start_events(W2.events)
  W2.add_pc(ID1, "Alice")
  W2.g:tick(1000)
  W2.dead[ID1] = true
  W2.g:tick(2000)
  W2.g:on_login({ kind = "logout", steamId = ID1, name = "Alice" })
  W2.g:tick(6000)
  for _, e in ipairs(W2.events.got) do t.ok(e[1] ~= "player_death", "no death after logout") end
end)

t.test("a PvP death in the kill log wins over the polled death (attacker kept, no duplicate)", function()
  local W = world()
  W.g:start_events(W.events)
  W.add_pc(ID2, "Bob")
  local f = LOGS .. "/kill_20261013210000.log"
  W.files[f] = ""
  W.g:tick(1000)
  W.dead[ID2] = true
  W.g:tick(2000)
  W.files[f] = u16("2026.10.13-21.00.05: Died: Bob (" .. ID2 .. "), Killer: Alice (" .. ID1 .. ") Weapon: Weapon_M9_C [Projectile]\r\n")
  W.g:tick(3500)
  W.g:tick(9000)
  t.eq(W.g.recentKills[ID2], 3500, "kill stamped with the tick clock")
  t.eq(#W.events.got, 1)
  t.eq(W.events.got[1][2].attacker.steamId, ID1)
end)

t.test("location of an offline player is the last position seen, never an error Takaro rejects", function()
  local W = world()
  W.g:start_events(W.events)
  W.g:on_login({ kind = "logout", steamId = ID1, name = "Alice", ip = "1.2.3.4", position = { x = 1, y = 2, z = 3 } })
  t.deep(W.g:get_player_location({ gameId = ID1 }), { ok = true, value = { x = 1, y = 2, z = 3 } })
  t.eq(W.g:get_player_location({ gameId = ID2 }).ok, false, "never seen: nothing to report")
end)

t.test("hot reload with a player online: no second player-connected from the next getPlayers", function()
  local W = world()
  W.add_pc(ID1, "Alice")
  W.g:start_events(W.events)
  W.g:list_players()
  t.eq(#W.events.got, 0)
  -- a new player after the reload is announced as usual
  W.add_pc(ID2, "Bob")
  W.g:list_players()
  t.eq(#W.events.got, 1)
  t.eq(W.events.got[1][2].steamId, ID2)
end)

t.test("entity-killed: an NPC that dies after a player's attack, credited with the game's weapon display name", function()
  local W = world()
  W.g.catalogs.entities = { done = true, entries = { { code = "BP_Zombie_Civilian", name = "Puppet" } } }
  W.g.catalogs.items = { done = true, entries = { { code = "Weapon_DEagle_50", name = "DEagle .50" } } }
  W.g:start_events(W.events)
  local pc = W.add_pc(ID1, "Alice")
  local z = W.add_npc("BP_Zombie_Civilian_C", 1000)
  local far = W.add_npc("BP_Zombie_Civilian_C", 900000)
  local deer = W.add_npc("BP_Deer_C", 500)
  W.g:tick(2000)
  local hook = W.hooks["/Script/SCUM.PlayerRpcChannel:Server_ProcessProjectileDamageEvents"]
  t.ok(hook, "projectile hook")
  local ch = obj({ GetOuter = function() return pc end })
  W.now = 3000 * 1000
  hook({ get = function() return ch end })
  z.alive, z.weapon = false, "Weapon_DEagle_50_C"
  far.alive, far.weapon = false, "Weapon_DEagle_50_C"
  W.g:tick(3500)
  t.eq(#W.events.got, 1, "the far NPC is out of range")
  t.deep(W.events.got[1], { "entity_killed", { player = { steamId = ID1, name = "Alice" }, entity = "Puppet",
    weapon = "DEagle .50" } })
  W.g:tick(4000)
  t.eq(#W.events.got, 1, "one death, one event")
  deer.alive = false -- no weapon recorded: never a placeholder
  W.g:tick(4500)
  t.eq(#W.events.got, 1)
  -- no attack for longer than the window: nothing credited
  local z2 = W.add_npc("BP_Zombie_Civilian_C", 100)
  W.g:tick(30000)
  z2.alive, z2.weapon = false, "Weapon_DEagle_50_C"
  W.g:tick(32000)
  t.eq(#W.events.got, 1)
  -- melee hook: the RPC runs on the attacker's own character
  local melee = W.hooks["/Script/SCUM.ConZCharacter:Server_HandleMeleeAttackCapsuleHit"]
  local z3 = W.add_npc("BP_Zombie_Civilian_C", 100)
  W.g:tick(34000)
  W.now = 34500 * 1000
  melee({ get = function() return pc:GetPrisoner() end })
  z3.alive, z3.weapon = false, "Weapon_DEagle_50_C"
  W.g:tick(35000)
  t.eq(#W.events.got, 2)
end)

t.test("game-thread cost is measured per tick", function()
  local W = world()
  W.g:tick(0)
  t.ok(W.g:perf_summary():find("ticks=1"), W.g:perf_summary())
end)

t.run("game")
