-- Coverage drift: the Lua registry must equal capabilities.json and cover all 19 actions and 6 events.
local t = require("lib.t")
local json = require("takaro.json")
local coverage = require("takaro.coverage")
local actions = require("takaro.actions")
local game = require("takaro.game")

local function read(path)
  local f = assert(io.open(path, "rb"))
  local s = f:read("a")
  f:close()
  return s
end

local caps = json.decode(read("capabilities.json"))

local OFFICIAL_ACTIONS = { "getPlayer", "getPlayers", "getPlayerLocation", "getPlayerInventory", "giveItem", "listItems",
  "listEntities", "listLocations", "executeConsoleCommand", "sendMessage", "teleportPlayer", "kickPlayer", "banPlayer",
  "unbanPlayer", "listBans", "testReachability", "shutdown", "getMapInfo", "getMapTile" }
local OFFICIAL_EVENTS = { "player-connected", "player-disconnected", "chat-message", "player-death", "entity-killed", "log" }

local function keys(tbl)
  local out = {}
  for k in pairs(tbl) do out[#out + 1] = k end
  table.sort(out)
  return out
end

local function sorted(list)
  local c = {}
  for i, v in ipairs(list) do c[i] = v end
  table.sort(c)
  return c
end

t.test("registry covers exactly the 19 Takaro actions and 6 events", function()
  t.eq(#OFFICIAL_ACTIONS, 19)
  t.deep(keys(coverage.actions), sorted(OFFICIAL_ACTIONS))
  t.deep(sorted(coverage.ACTIONS), sorted(OFFICIAL_ACTIONS))
  t.deep(keys(coverage.events), sorted(OFFICIAL_EVENTS))
  t.deep(sorted(coverage.EVENTS), sorted(OFFICIAL_EVENTS))
end)

t.test("every entry has a valid status, a shape, a verification note and a reason", function()
  for name, c in pairs(coverage.actions) do
    t.ok(coverage.STATUSES[c.status], name .. " status")
    t.ok(c.responseShape and c.responseShape ~= "", name .. " responseShape")
    t.ok(c.liveVerification and c.liveVerification ~= "", name .. " liveVerification")
    t.ok(c.reason and c.reason ~= "", name .. " reason")
  end
  for name, c in pairs(coverage.events) do
    t.ok(c.status == "live-supported" or c.status == "unsupported", name .. " event status")
    t.ok(c.payloadShape and c.payloadShape ~= "", name .. " payloadShape")
    t.ok(c.liveVerification and c.liveVerification ~= "", name .. " liveVerification")
    t.ok(c.reason and c.reason ~= "", name .. " reason")
  end
end)

t.test("every live-supported entry names its live run; others say they are not live-tested", function()
  for _, set in ipairs({ coverage.actions, coverage.events }) do
    for name, c in pairs(set) do
      if c.status == "live-supported" then
        t.ok(c.liveVerification:find("^live 20%d%d%-%d%d%-%d%d on SCUM "), name)
      else
        t.ok(c.liveVerification:find("not live%-tested"), name)
      end
    end
  end
end)

t.test("capabilities.json equals the registry (no drift)", function()
  t.deep(caps.actions, coverage.actions, "actions")
  t.deep(caps.events, coverage.events, "events")
end)

t.test("every registry action has a router handler and every handler is in the registry", function()
  t.deep(keys(actions.HANDLERS), sorted(OFFICIAL_ACTIONS))
end)

t.test("list actions are exactly the ones answered with bare arrays", function()
  t.deep(keys(actions.LIST_ACTIONS), sorted({ "getPlayers", "listItems", "listEntities", "listLocations", "listBans",
    "getPlayerInventory" }))
end)

t.test("every adapter function answers a structured result even with no game world (never throws)", function()
  local logs = {}
  local g = game.new({}, { info = function() end, warn = function() end, error = function() end,
    debug = function() end }, { mod_dir = "/nowhere/SCUM/Binaries/Win64/ue4ss/Mods/TakaroSCUM",
    fs = { list = function() return {} end, read = function() return nil end, write = function() return true end,
      size = function() return nil end, read_at = function() return nil end } })
  for _, fn in ipairs(game.ACTION_FUNCTIONS) do
    local ok, r = pcall(g[fn], g, { gameId = "76561198000000001", message = "x", command = "listplayers",
      item = "Weapon_M9", amount = 1, x = 1, y = 2, z = 3 }, function() end)
    t.ok(ok, fn .. " threw: " .. tostring(r))
    t.ok(type(r) == "table" and type(r.ok) == "boolean", fn .. " returned a result")
  end
end)

t.run("coverage")
