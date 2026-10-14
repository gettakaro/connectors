-- Takaro Generic Game Protocol version 1 on top of the version 0 handlers: the Capability Manifest and the
-- version 1 response shape (standard { code, message } errors, a dimension on positions). No transport or game
-- symbols here. The version 0 behaviour stays unchanged; protocol.lua only calls this once Takaro chose version 1.
local json = require("takaro.json")
local util = require("takaro.util")
local coverage = require("takaro.coverage")
local map = require("takaro.map")

local M = {}

M.ERROR_CODES = { unsupported = true, invalid_args = true, not_found = true, game_error = true, internal = true }

-- Takaro capability name of each action. Actions without one (getMapInfo, getMapTile, testReachability) are always
-- available or replaced by the map capability.
M.CAPABILITY_OF = {
  getPlayers = "players.list",
  getPlayer = "players.get",
  getPlayerLocation = "players.location",
  getPlayerInventory = "players.inventory",
  teleportPlayer = "players.teleport",
  listItems = "items.list",
  giveItem = "items.give",
  listEntities = "entities.list",
  listLocations = "locations.list",
  sendMessage = "chat.send",
  executeConsoleCommand = "console.execute",
  kickPlayer = "moderation.kick",
  banPlayer = "moderation.ban",
  unbanPlayer = "moderation.unban",
  listBans = "moderation.listBans",
  shutdown = "server.shutdown",
}

-- The Capability Manifest: every live-supported action and event of the coverage registry plus the map Dimension.
-- Placeholder answers (schema-fallback) are not declared, so Takaro does not rely on them.
function M.manifest(bounds)
  local out = {}
  for _, action in ipairs(coverage.ACTIONS) do
    local name, c = M.CAPABILITY_OF[action], coverage.actions[action]
    if name and c and c.status == "live-supported" then out[#out + 1] = { name = name } end
  end
  for _, ev in ipairs(coverage.EVENTS) do
    local c = coverage.events[ev]
    if c and c.status == "live-supported" then out[#out + 1] = { name = "events." .. ev } end
  end
  out[#out + 1] = map.capability(bounds)
  return out
end

-- The error code for a version 0 failure text. Handlers prefix their own codes ("invalid_args: ...").
function M.code_for(message)
  local m = tostring(message or "")
  local prefix = m:match("^([%a_]+):")
  if prefix and M.ERROR_CODES[prefix] then return prefix end
  local l = m:lower()
  if l:find("unknown action", 1, true) then return "unsupported" end
  if l:find("invalid_args", 1, true) then return "invalid_args" end
  if l:find("offline", 1, true) or l:find("not online", 1, true) or l:find("not found", 1, true)
    or l:find("no character", 1, true) or l:find("unknown player", 1, true) then
    return "not_found"
  end
  if l:find("internal error", 1, true) then return "internal" end
  return "game_error"
end

local function failure_text(r)
  return util.opt_string(r.errorMessage) or util.opt_string(r.error) or util.opt_string(r.rawResult) or ""
end

-- Translates a version 0 handler result into the version 1 answer: payload, err ({ code, message } or nil).
--   * { success = false, errorMessage } and { error = "..." } become errors; executeConsoleCommand keeps its
--     { success = false, rawResult } (a refused command is a valid CommandOutput).
--   * getPlayerLocation carries the map dimension; a player without a position answers null.
function M.translate(action, result, known)
  if known == false then
    return nil, { code = "unsupported", message = "unknown action '" .. tostring(action) .. "'" }
  end
  if type(result) ~= "table" or result == json.null or util.is_array(result) then return result, nil end
  local failed = (util.opt_string(result.error) or "") ~= ""
    or (result.success == false and action ~= "executeConsoleCommand")
  if action == "getPlayerLocation" then
    if failed then return json.null, nil end
    if type(result.x) == "number" and type(result.y) == "number" and type(result.z) == "number" then
      return { x = result.x, y = result.y, z = result.z, dimension = map.DIMENSION_ID }, nil
    end
    return result, nil
  end
  if not failed then return result, nil end
  local text = failure_text(result)
  if text == "" then text = tostring(action) .. " failed" end
  return nil, { code = M.code_for(text), message = text }
end

return M
