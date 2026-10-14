-- Player identity (IGamePlayer) and the last-known player store.
-- SCUM requires Steam, so gameId = SteamID64, steamId = SteamID64, platformId = "steam:<SteamID64>".
-- Only Takaro's IGamePlayer fields are ever emitted (Takaro rejects events with extra keys).
local util = require("takaro.util")

local M = {}

M.FIELDS = { "gameId", "name", "steamId", "epicOnlineServicesId", "xboxLiveId", "platformId", "ip", "ping" }

function M.is_steam64(id)
  return type(id) == "string" and id:match("^7656%d%d%d%d%d%d%d%d%d%d%d%d%d$") ~= nil
end

-- raw: whatever the game adapter produced, e.g. { steamId = "7656...", name = "Bob", ip = "1.2.3.4", ping = 30 }.
-- Returns the IGamePlayer table or nil, reason.
function M.to_player(raw)
  if type(raw) ~= "table" then return nil, "player is not a table" end
  local id = util.opt_string(raw.steamId) or util.opt_string(raw.gameId)
  if not M.is_steam64(id) then return nil, "no SteamID64 (got " .. tostring(id) .. ")" end
  local name = util.opt_string(raw.name)
  if name == nil or util.trim(name) == "" then name = id end
  local p = { gameId = id, name = name, steamId = id, platformId = "steam:" .. id }
  local ip = util.opt_string(raw.ip)
  if ip and ip ~= "" then p.ip = ip end
  local ping = util.opt_number(raw.ping)
  if ping and ping >= 0 then p.ping = math.floor(ping) end
  return p
end

-- Takaro sends a player as { gameId }, { player = { gameId } }, { player = "<id>" } or a bare string/number.
function M.game_id_from_args(args)
  if type(args) ~= "table" then return util.opt_string(args) end
  local id = util.opt_string(args.gameId)
  if id then return id end
  local p = args.player
  if type(p) == "table" then
    id = util.opt_string(p.gameId)
    if id then return id end
    if type(p.player) == "table" then return util.opt_string(p.player.gameId) end
    return nil
  end
  return util.opt_string(p)
end

local Known = {}
Known.__index = Known

function M.new_store()
  return setmetatable({ byId = {}, dirty = false }, Known)
end

function Known:remember(player)
  if type(player) ~= "table" or not player.gameId then return end
  local old = self.byId[player.gameId]
  local copy = {}
  for _, f in ipairs(M.FIELDS) do copy[f] = player[f] end
  -- A placeholder name (the raw id) never replaces a real one, and a missing IP keeps the last one.
  if old and (copy.name == nil or copy.name == copy.gameId) and old.name and old.name ~= old.gameId then
    copy.name = old.name
  end
  if old and copy.ip == nil then copy.ip = old.ip end
  if not old or old.name ~= copy.name or old.ip ~= copy.ip then self.dirty = true end
  copy.ping = nil -- ping is a live value; a stale one is worse than none
  self.byId[player.gameId] = copy
end

function Known:get(id)
  local p = self.byId[id]
  if not p then return nil end
  local copy = {}
  for k, v in pairs(p) do copy[k] = v end
  return copy
end

-- Always a valid IGamePlayer for getPlayer: the last-known record, or one built from the id
-- (Takaro rejects {}, null and error frames for getPlayer with a user-visible 400).
function Known:resolve(id)
  local p = self:get(id)
  if p then return p end
  id = tostring(id or "")
  p = { gameId = id, name = id }
  if M.is_steam64(id) then
    p.steamId = id
    p.platformId = "steam:" .. id
  end
  return p
end

function Known:to_list()
  local out = {}
  for _, p in pairs(self.byId) do out[#out + 1] = p end
  table.sort(out, function(a, b) return a.gameId < b.gameId end)
  return out
end

function Known:load_list(list)
  if type(list) ~= "table" then return end
  for _, raw in ipairs(list) do
    local p = M.to_player(raw)
    if p then self.byId[p.gameId] = p end
  end
  self.dirty = false
end

return M
