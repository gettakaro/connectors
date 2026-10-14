-- Regenerates capabilities.json from the coverage registry (Scripts/takaro/coverage.lua).
-- usage (from the source folder): LUA_PATH="mod/TakaroSCUM/Scripts/?.lua;;" lua5.4 scripts/gen-capabilities.lua > capabilities.json
local json = require("takaro.json")
local coverage = require("takaro.coverage")
local map = require("takaro.map")

local doc = json.object({
  game = "SCUM",
  connector = "Takaro SCUM native connector (UE4SS mod TakaroSCUM)",
  architecture = "in-process UE4SS mod inside SCUMServer.exe: Lua protocol + game adapter (Scripts/), C++ WinHTTP "
    .. "WebSocket transport (dlls/main.dll); outbound WSS only, no sidecar, no inbound port",
  runtime = "SCUMServer.exe (Steam app 3792580) with UE4SS v3.0.1-1171-g91d90727",
  takaroConnection = "outbound_websocket",
  protocolVersions = { 0, 1 },
  map = map.dimension(),
  statusDefinitions = json.object({
    ["live-supported"] = "proven end to end through Takaro on a real server",
    ["schema-fallback"] = "answers a valid empty or placeholder DTO; the game data is not wired yet or not available",
    unsupported = "answers a structured failure",
  }),
  actions = coverage.actions,
  events = coverage.events,
})

-- pretty-print one action/event per line so diffs stay readable
local function obj(t, indent)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys)
  local lines = {}
  for _, k in ipairs(keys) do
    local v = t[k]
    local enc
    if type(v) == "table" and (k == "actions" or k == "events" or k == "statusDefinitions") then
      enc = obj(v, indent .. "  ")
    else
      enc = json.encode(v)
    end
    lines[#lines + 1] = indent .. "  " .. json.encode(k) .. ": " .. enc
  end
  return "{\n" .. table.concat(lines, ",\n") .. "\n" .. indent .. "}"
end

io.write(obj(doc, ""), "\n")
