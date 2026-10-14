-- Event adapter: turns game signals (from game.lua hooks) into the 6 Takaro events. Every event passes the
-- DTO whitelist here: Takaro validates gameEvents with forbidNonWhitelisted, so one extra key drops the event.
-- Dedup: a caller-supplied key is accepted once per window. No transport or game symbols here.
local util = require("takaro.util")
local players = require("takaro.players")

local M = {}

M.CHANNELS = { global = true, team = true, friends = true, whisper = true }
M.DEDUP_WINDOW_MS = 3000

local Events = {}
Events.__index = Events

-- opts: client (protocol client with send_event), known (players store), log, now()
function M.new(opts)
  return setmetatable({
    client = opts.client, known = opts.known, log = opts.log, now = opts.now,
    recent = {}, stats = { emitted = {}, dropped = {} },
  }, Events)
end

function Events:drop(eventType, why)
  self.stats.dropped[eventType] = (self.stats.dropped[eventType] or 0) + 1
  self.log:warn("dropping %s event: %s", eventType, why)
  return false
end

function Events:dedup(key)
  if not key then return false end
  local now = self.now()
  for k, t in pairs(self.recent) do
    if now - t > M.DEDUP_WINDOW_MS then self.recent[k] = nil end
  end
  if self.recent[key] then return true end
  self.recent[key] = now
  return false
end

function Events:emit(eventType, data, key)
  if self:dedup(key and (eventType .. "|" .. key)) then return false end
  self.stats.emitted[eventType] = (self.stats.emitted[eventType] or 0) + 1
  return self.client:send_event(eventType, data)
end

-- A player from the game, with the last-known name when the game only supplied the id.
function Events:player(raw)
  local p, why = players.to_player(raw)
  if not p then return nil, why end
  local k = self.known:get(p.gameId)
  if k then
    if p.name == p.gameId and k.name then p.name = k.name end
    -- The login log is the only IP source; after a Lua reload the last-known IP fills the gap.
    if not p.ip and k.ip then p.ip = k.ip end
  end
  return p
end

local function position(v)
  if type(v) ~= "table" then return nil end
  local x, y, z = util.opt_number(v.x), util.opt_number(v.y), util.opt_number(v.z)
  if x and y and z then return { x = x, y = y, z = z } end
  return nil
end

function Events:player_connected(raw, key)
  local p, why = self:player(raw)
  if not p then return self:drop("player-connected", why) end
  self.known:remember(p)
  return self:emit("player-connected", { player = p }, key or ("connect|" .. p.gameId))
end

function Events:player_disconnected(raw, key)
  local p, why = self:player(raw)
  if not p then
    local id = type(raw) == "table" and (util.opt_string(raw.steamId) or util.opt_string(raw.gameId))
    p = id and self.known:get(id) or nil
    if not p then return self:drop("player-disconnected", why) end
  end
  return self:emit("player-disconnected", { player = p }, key or ("disconnect|" .. p.gameId))
end

-- e = { player = raw, msg = string, channel = "global"|..., recipient = raw? }
function Events:chat_message(e, key)
  if type(e) ~= "table" then return self:drop("chat-message", "not a table") end
  local p = e.player and self:player(e.player) or nil
  -- Player-less lines are server/admin broadcasts, including our own sendMessage output: never re-ingest them.
  if not p then return self:drop("chat-message", "no player (server message or echo)") end
  local msg = util.opt_string(e.msg)
  if not msg or msg == "" then return self:drop("chat-message", "empty message") end
  local channel = util.opt_string(e.channel) or "global"
  if not M.CHANNELS[channel] then channel = "global" end
  local data = { player = p, channel = channel, msg = msg }
  if channel == "whisper" and e.recipient then
    local r = self:player(e.recipient)
    if r then data.recipient = r end
  end
  return self:emit("chat-message", data, key)
end

-- e = { player = raw, attacker = raw?, position = {x,y,z}? }
function Events:player_death(e, key)
  if type(e) ~= "table" then return self:drop("player-death", "not a table") end
  local p, why = self:player(e.player)
  if not p then return self:drop("player-death", why) end
  local data = { player = p }
  local a = e.attacker and self:player(e.attacker) or nil
  if a then data.attacker = a end
  local pos = position(e.position)
  if pos then data.position = pos end
  return self:emit("player-death", data, key)
end

-- e = { player = raw, entity = string, weapon = string } (all required by Takaro)
function Events:entity_killed(e, key)
  if type(e) ~= "table" then return self:drop("entity-killed", "not a table") end
  local p, why = self:player(e.player)
  if not p then return self:drop("entity-killed", why) end
  local entity, weapon = util.opt_string(e.entity), util.opt_string(e.weapon)
  if not entity or entity == "" then return self:drop("entity-killed", "no entity") end
  if not weapon or weapon == "" then return self:drop("entity-killed", "no weapon (never sent as a placeholder)") end
  return self:emit("entity-killed", { player = p, entity = entity, weapon = weapon }, key)
end

function Events:log_line(msg, key)
  msg = util.opt_string(msg)
  if not msg or msg == "" then return self:drop("log", "empty") end
  return self:emit("log", { msg = msg }, key)
end

return M
