-- TakaroConfig.txt (key=value, '#' comments) read from the mod folder. Token values never leave this
-- module except into the identify frame; describe() reports only whether they are set.
local util = require("takaro.util")

local M = {}

M.FILE_NAME = "TakaroConfig.txt"

M.DEFAULTS = {
  TAKARO_WS_URL = "wss://connect.takaro.io/",
  IDENTITY_TOKEN = "",
  REGISTRATION_TOKEN = "",
  SERVER_NAME = "SCUM server",
  ENABLE_DESTRUCTIVE_COMMANDS = "false",
  COMMAND_ALLOWLIST = "",
  LOG_FRAMES = "false",
  TAKARO_CA_FILE = "",
  TAKARO_PROTOCOL = "1",
}

M.SECRET_KEYS = { IDENTITY_TOKEN = true, REGISTRATION_TOKEN = true }

M.TEMPLATE = [[
# TakaroSCUM connector settings. Edit, then restart the server.
# Takaro WebSocket endpoint (leave as is for takaro.io).
TAKARO_WS_URL=wss://connect.takaro.io/
# Paste the registration token from the Takaro dashboard (Game servers > Add > Generic).
REGISTRATION_TOKEN=
# Stable id of this server in Takaro. Leave empty to have one generated and kept in TakaroIdentity.txt.
IDENTITY_TOKEN=
# Name shown in Takaro.
SERVER_NAME=SCUM server
# Allow Takaro to shut the server down (shutdown action). true/false.
ENABLE_DESTRUCTIVE_COMMANDS=false
# Comma-separated console commands Takaro may run (first word, without '#'), e.g. announce,listplayers
COMMAND_ALLOWLIST=
# Log every Takaro frame to takaro.log (tokens are redacted). true/false.
LOG_FRAMES=false
# Takaro protocol: 1 offers version 1 (world map, falls back to 0 on an older Takaro); 0 speaks version 0 only.
TAKARO_PROTOCOL=1
]]

local function bool(v)
  v = tostring(v or ""):lower()
  return v == "true" or v == "1" or v == "yes" or v == "on"
end

-- Parses key=value text. Unknown keys are kept (and reported), duplicate keys: the last one wins.
function M.parse(text)
  local raw, unknown = {}, {}
  for line in (tostring(text or "") .. "\n"):gmatch("([^\r\n]*)\r?\n") do
    local l = util.trim(line)
    if l ~= "" and l:sub(1, 1) ~= "#" and l:sub(1, 1) ~= ";" then
      local k, v = l:match("^([%w_]+)%s*=%s*(.-)$")
      if k then
        k = k:upper()
        raw[k] = util.trim(v)
        if M.DEFAULTS[k] == nil then unknown[#unknown + 1] = k end
      end
    end
  end
  return raw, unknown
end

function M.allowlist_set(text)
  local set = {}
  for item in tostring(text or ""):gmatch("[^,%s]+") do
    set[(item:gsub("^#", "")):lower()] = true
  end
  return set
end

-- Builds the typed config from parsed values plus defaults.
function M.build(raw)
  local v = {}
  for k, d in pairs(M.DEFAULTS) do
    local x = raw[k]
    if x == nil then x = d end
    v[k] = x
  end
  return {
    url = v.TAKARO_WS_URL,
    identityToken = v.IDENTITY_TOKEN,
    registrationToken = v.REGISTRATION_TOKEN,
    serverName = v.SERVER_NAME ~= "" and v.SERVER_NAME or M.DEFAULTS.SERVER_NAME,
    destructive = bool(v.ENABLE_DESTRUCTIVE_COMMANDS),
    allowlist = M.allowlist_set(v.COMMAND_ALLOWLIST),
    logFrames = bool(v.LOG_FRAMES),
    caFile = v.TAKARO_CA_FILE,
    protocol = util.trim(v.TAKARO_PROTOCOL) == "0" and 0 or 1,
  }
end

-- Reads the config file; creates it from TEMPLATE when it is missing. fs = { read(path), write(path, text) }.
function M.load(dir, fs)
  local path = dir .. "/" .. M.FILE_NAME
  local text = fs.read(path)
  local created = false
  if text == nil then
    created = fs.write(path, M.TEMPLATE)
    text = M.TEMPLATE
  end
  local raw, unknown = M.parse(text)
  local cfg = M.build(raw)
  cfg.path, cfg.created, cfg.unknownKeys = path, created, unknown
  return cfg
end

-- One log line describing the config without any secret value.
function M.describe(cfg)
  local allow = {}
  for k in pairs(cfg.allowlist) do allow[#allow + 1] = k end
  table.sort(allow)
  return string.format(
    "url=%s serverName=%q registrationToken=%s identityToken=%s destructive=%s allowlist=[%s] logFrames=%s caFile=%s "
      .. "protocol=%s",
    cfg.url, cfg.serverName, cfg.registrationToken ~= "" and "set" or "MISSING",
    cfg.identityToken ~= "" and "set" or "generated", tostring(cfg.destructive), table.concat(allow, ","),
    tostring(cfg.logFrames), cfg.caFile ~= "" and "set" or "system", cfg.protocol == 0 and "0" or "[0,1]")
end

return M
