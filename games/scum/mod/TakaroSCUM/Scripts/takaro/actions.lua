-- Action router: normalises Takaro's args, validates them, calls the game adapter and always answers with
-- a payload Takaro accepts (bare arrays for list actions, a real IGamePlayer for getPlayer, a structured
-- { success = false, errorMessage } for failures). No transport or game symbols here.
local json = require("takaro.json")
local util = require("takaro.util")
local players = require("takaro.players")

local M = {}

M.LIST_ACTIONS = {
  getPlayers = true, listItems = true, listEntities = true, listLocations = true, listBans = true,
  getPlayerInventory = true,
}

M.DEFAULT_BAN_REASON = "Banned on the game server"

M.MAP_INFO_FALLBACK = { enabled = false, mapBlockSize = 0, maxZoom = 0, mapSizeX = 0, mapSizeY = 0, mapSizeZ = 0 }

local function fail(msg)
  return { success = false, errorMessage = msg }
end

-- Takaro sends args as {}, [], an object, a JSON-encoded string (sometimes twice), or nothing/null.
function M.normalize_args(raw)
  local v = raw
  for _ = 1, 2 do
    if type(v) ~= "string" then break end
    local s = util.trim(v)
    if s == "" then return {} end
    local ok, decoded = pcall(json.decode, s)
    if not ok then return {} end
    v = decoded
  end
  if type(v) ~= "table" or v == json.null then return {} end
  if util.is_array(v) then
    if #v == 1 and type(v[1]) == "table" and not util.is_array(v[1]) then return v[1] end
    return {}
  end
  return v
end

local Router = {}
Router.__index = Router

-- opts: game (adapter), known (players store), config, log, schedule(delayMs, fn)
function M.new(opts)
  local r = setmetatable({}, Router)
  r.game = opts.game
  r.known = opts.known
  r.config = opts.config
  r.log = opts.log
  r.schedule = opts.schedule or function(_, fn) fn() end
  r.stats = { calls = {}, adapterFailures = 0 }
  return r
end

-- Calls adapter function `name`. Adapter functions take (params, done) and may either return a result at
-- once or call done(result) later (e.g. from the game thread). A result is { ok = true, value = ... } or
-- { ok = false, reason = "..." }. cb runs exactly once.
function Router:call(name, params, cb)
  local fn = self.game and self.game[name]
  if type(fn) ~= "function" then
    cb({ ok = false, reason = "adapter has no " .. name })
    return
  end
  local finished = false
  local function done(res)
    if finished then return end
    finished = true
    if type(res) ~= "table" then res = { ok = false, reason = "adapter returned " .. type(res) } end
    if not res.ok then self.stats.adapterFailures = self.stats.adapterFailures + 1 end
    cb(res)
  end
  local ok, ret = pcall(fn, self.game, params, done)
  if not ok then
    done({ ok = false, reason = "adapter error: " .. tostring(ret) })
  elseif ret ~= nil then
    done(ret)
  end
end

-- The answer for an action that could not finish (timeout or internal error).
function Router:fallback(action, why, rawArgs)
  why = why or "unavailable"
  if action == "getPlayerInventory" then return { error = "inventory unavailable: " .. why } end
  if M.LIST_ACTIONS[action] then return {} end
  if action == "getPlayer" then
    return self.known:resolve(players.game_id_from_args(M.normalize_args(rawArgs)) or "")
  end
  if action == "testReachability" then return { connectable = false, reason = "game adapter: " .. why } end
  if action == "getMapInfo" then return util.shallow_copy(M.MAP_INFO_FALLBACK) end
  if action == "executeConsoleCommand" then return { success = false, rawResult = "", errorMessage = why } end
  return fail(why)
end

local function list_reply(self, action, reply)
  return function(res)
    if res.ok and type(res.value) == "table" then
      reply(res.value)
    else
      self.log:debug("%s: answering [] (%s)", action, tostring(res.reason))
      reply({})
    end
  end
end

local function simple_reply(reply)
  return function(res)
    if res.ok then
      local out = { success = true }
      if type(res.value) == "string" then out.rawResult = res.value end
      reply(out)
    else
      reply(fail(tostring(res.reason or "failed")))
    end
  end
end

local H = {}

function H.testReachability(self, _args, reply)
  self:call("reachability", {}, function(res)
    if res.ok and type(res.value) == "table" then
      local v = res.value
      reply({ connectable = v.connectable == true, reason = util.opt_string(v.reason) or json.null })
    else
      reply({ connectable = false, reason = "game adapter: " .. tostring(res.reason) })
    end
  end)
end

function H.getPlayers(self, _args, reply)
  self:call("list_players", {}, function(res)
    if not (res.ok and type(res.value) == "table") then
      self.log:debug("getPlayers: answering [] (%s)", tostring(res.reason))
      reply({})
      return
    end
    local out = {}
    for _, raw in ipairs(res.value) do
      local p, why = players.to_player(raw)
      if p then
        self.known:remember(p)
        out[#out + 1] = p
      else
        self.log:warn("getPlayers: skipping a player without a stable id (%s)", tostring(why))
      end
    end
    reply(out)
  end)
end

function H.getPlayer(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then
    reply(self.known:resolve(""))
    return
  end
  self:call("get_player", { gameId = id }, function(res)
    local p = res.ok and players.to_player(res.value) or nil
    if p then
      self.known:remember(p)
      reply(p)
    else
      reply(self.known:resolve(id))
    end
  end)
end

function H.getPlayerLocation(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  self:call("get_player_location", { gameId = id }, function(res)
    local v = res.ok and res.value
    local x, y, z = type(v) == "table" and util.opt_number(v.x), type(v) == "table" and util.opt_number(v.y),
      type(v) == "table" and util.opt_number(v.z)
    if x and y and z then
      reply({ x = x, y = y, z = z })
    else
      reply(fail(tostring(res.reason or "no position")))
    end
  end)
end

-- Takaro diffs every inventory answer against the previous one: an empty list records "removed" for every item the
-- player carries. So a game-side failure answers { error } (Takaro's generic client throws and skips that sync)
-- instead of []; only an offline player (Takaro polls online players only) or no id gets an empty list.
M.INVENTORY_EMPTY_REASONS = { ["player is offline"] = true }
function H.getPlayerInventory(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then return reply({}) end
  self:call("get_player_inventory", { gameId = id }, function(res)
    if res.ok and type(res.value) == "table" then return reply(res.value) end
    local why = tostring(res.reason)
    if M.INVENTORY_EMPTY_REASONS[why] then return reply({}) end
    self.log:warn("getPlayerInventory: %s; answering an error so Takaro keeps the last inventory", why)
    reply({ error = "inventory unavailable: " .. why })
  end)
end

function H.listItems(self, _args, reply) self:call("list_items", {}, list_reply(self, "listItems", reply)) end
function H.listEntities(self, _args, reply) self:call("list_entities", {}, list_reply(self, "listEntities", reply)) end
function H.listLocations(self, _args, reply) self:call("list_locations", {}, list_reply(self, "listLocations", reply)) end
-- IBanDTO { player: IGamePlayer, reason?, expiresAt? }. The player carries the last-seen name (Takaro's syncBans
-- renames the profile to whatever name a ban entry carries), never the raw id when a better name is known.
function H.listBans(self, _args, reply)
  self:call("list_bans", {}, function(res)
    if not (res.ok and type(res.value) == "table") then
      self.log:debug("listBans: answering [] (%s)", tostring(res.reason))
      return reply({})
    end
    local out = {}
    for _, b in ipairs(res.value) do
      local raw = type(b) == "table" and b.player or nil
      local p = players.to_player(raw)
      if p then
        if p.name == p.gameId then
          local k = self.known:get(p.gameId)
          if k and k.name then p.name = k.name end
        end
        local e = { player = p }
        -- Takaro's BanDTO requires reason to be a string (an entry without one fails validation, seen 2026-10-13).
        local reason = util.opt_string(b.reason)
        e.reason = (reason and reason ~= "") and reason or M.DEFAULT_BAN_REASON
        local exp = util.opt_string(b.expiresAt)
        if exp and exp ~= "" then e.expiresAt = exp end
        out[#out + 1] = e
      end
    end
    reply(out)
  end)
end

function H.giveItem(self, args, reply)
  local id = players.game_id_from_args(args)
  local item = util.opt_string(args.item) or util.opt_string(args.code) or util.opt_string(args.name)
  local amount = util.opt_number(args.amount) or 1
  local quality = util.opt_string(args.quality)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  if not item or util.trim(item) == "" then return reply(fail("invalid_args: item is required")) end
  if amount < 1 then return reply(fail("invalid_args: amount must be at least 1")) end
  self:call("give_item", { gameId = id, item = item, amount = math.floor(amount), quality = quality },
    simple_reply(reply))
end

function H.executeConsoleCommand(self, args, reply)
  local cmd = util.opt_string(args.command)
  cmd = cmd and util.trim(cmd) or ""
  if cmd == "" then return reply({ success = false, rawResult = "", errorMessage = "invalid_args: command is empty" }) end
  local first = (cmd:match("^(%S+)") or ""):gsub("^#", ""):lower()
  if not self.config.allowlist[first] then
    return reply({ success = false, rawResult = "",
      errorMessage = "command_not_allowed: '" .. first .. "' is not in COMMAND_ALLOWLIST" })
  end
  self:call("execute_command", { command = cmd }, function(res)
    if res.ok then
      reply({ success = true, rawResult = util.opt_string(res.value) or "" })
    else
      reply({ success = false, rawResult = "", errorMessage = tostring(res.reason) })
    end
  end)
end

function H.sendMessage(self, args, reply)
  local message = util.opt_string(args.message)
  if not message or message == "" then return reply(fail("invalid_args: message is required")) end
  local opts = util.opt_table(args.opts) or {}
  local recipient = util.opt_table(opts.recipient)
  local recipientId = recipient and players.game_id_from_args(recipient) or nil
  local sender = util.opt_string(opts.senderNameOverride)
  self:call("send_message", { message = message, recipientGameId = recipientId, senderName = sender },
    simple_reply(reply))
end

function H.teleportPlayer(self, args, reply)
  local id = players.game_id_from_args(args)
  local x, y, z = util.opt_number(args.x), util.opt_number(args.y), util.opt_number(args.z)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  if not (x and y and z) then return reply(fail("invalid_args: x, y and z must be numbers")) end
  self:call("teleport_player", { gameId = id, x = x, y = y, z = z }, simple_reply(reply))
end

function H.kickPlayer(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  local reason = util.opt_string(args.reason)
  if not reason or util.trim(reason) == "" then reason = "Kicked by Takaro" end
  self:call("kick_player", { gameId = id, reason = reason }, simple_reply(reply))
end

function H.banPlayer(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  local reason = util.opt_string(args.reason)
  if not reason or util.trim(reason) == "" then reason = "Banned by Takaro" end
  local expiresAt = util.opt_string(args.expiresAt)
  if expiresAt and not expiresAt:match("^%d%d%d%d%-%d%d%-%d%d") then
    return reply(fail("invalid_args: expiresAt must be an ISO 8601 date"))
  end
  -- Takaro sends its own player record (args.player.player.name). Keep that name for listBans when the game never
  -- saw the player, so syncBans never renames a known Takaro player to the raw SteamID.
  local name
  local pog = util.opt_table(args.player)
  local tp = pog and util.opt_table(pog.player)
  local tname = tp and util.opt_string(tp.name)
  if tname and util.trim(tname) ~= "" and tname ~= id then name = tname end
  if name and not self.known:get(id) then self.known:remember({ gameId = id, name = name }) end
  self:call("ban_player", { gameId = id, reason = reason, expiresAt = expiresAt, name = name }, simple_reply(reply))
end

function H.unbanPlayer(self, args, reply)
  local id = players.game_id_from_args(args)
  if not id then return reply(fail("invalid_args: gameId is required")) end
  self:call("unban_player", { gameId = id }, simple_reply(reply))
end

function H.shutdown(self, _args, reply)
  if not self.config.destructive then
    return reply(fail("shutdown is disabled: set ENABLE_DESTRUCTIVE_COMMANDS=true in TakaroConfig.txt"))
  end
  -- Answer first, then stop: Takaro must get the response before the socket dies.
  reply({ success = true })
  self.schedule(1000, function()
    self:call("shutdown", {}, function(res)
      if not res.ok then self.log:error("shutdown failed after it was acknowledged: %s", tostring(res.reason)) end
    end)
  end)
end

function H.getMapInfo(_self, _args, reply)
  reply(util.shallow_copy(M.MAP_INFO_FALLBACK))
end

function H.getMapTile(_self, _args, reply)
  reply(fail("unsupported: no map tiles are rendered for SCUM"))
end

M.HANDLERS = H

-- Entry point used by the protocol client.
function Router:handle(action, rawArgs, reply)
  self.stats.calls[action] = (self.stats.calls[action] or 0) + 1
  local h = H[action]
  if not h then
    self.log:warn("unknown action '%s'", tostring(action))
    return reply(fail("unknown action '" .. tostring(action) .. "'"))
  end
  return h(self, M.normalize_args(rawArgs), reply)
end

return M
