-- Game Profile (Takaro generic protocol): what this connector says it is. The game and connector identity go into
-- identify; protocol 1 adds the game.profile capability (display name, running game build, icon revision + size)
-- and answers getGameIcon with the icon's bytes. The icon ships next to the scripts (icon-256.png in the mod
-- folder); its revision is the first 12 hex characters of its sha256, so a changed file always gets a new revision.
local sha256 = require("takaro.sha256")
local util = require("takaro.util")

local M = {}

M.GAME = "scum"
M.CONNECTOR_NAME = "takaro-scum"
M.DISPLAY_NAME = "SCUM"
M.ICON_FILE = "icon-256.png"
M.ICON_SIZE_PX = 256
M.ICON_MAX_BYTES = 131072
M.VERSION_MAX = 32

local PNG_SIGNATURE = "\137PNG\r\n\26\n"

-- The icon file as { bytes, size, revision }, or nil and the reason. Only a 256 x 256 PNG within Takaro's limit is
-- declared: Takaro rejects anything else, and a rejected revision is never fetched again.
function M.load_icon(fs, dir)
  local bytes = fs.read(dir .. "/" .. M.ICON_FILE)
  if not bytes or bytes == "" then return nil, M.ICON_FILE .. " is missing" end
  if #bytes > M.ICON_MAX_BYTES then
    return nil, string.format("%s is %d bytes (max %d)", M.ICON_FILE, #bytes, M.ICON_MAX_BYTES)
  end
  if bytes:sub(1, 8) ~= PNG_SIGNATURE or bytes:sub(13, 16) ~= "IHDR" then return nil, M.ICON_FILE .. " is not a PNG" end
  local w, h = string.unpack(">I4>I4", bytes, 17)
  if w ~= M.ICON_SIZE_PX or h ~= M.ICON_SIZE_PX then
    return nil, string.format("%s is %dx%d, not %dx%d", M.ICON_FILE, w, h, M.ICON_SIZE_PX, M.ICON_SIZE_PX)
  end
  return { bytes = bytes, size = #bytes, revision = sha256.hex(bytes):sub(1, 12) }
end

-- "1.3.4.2.161892+0" (GeneralProjectSettings.ProjectVersion) -> "1.3.4.2.161892", the build SCUM logs as its
-- game version. nil when it is not a usable version (printable ASCII, at most 32 characters).
function M.clean_version(v)
  if type(v) ~= "string" then return nil end
  v = (util.trim(v):gsub("%+%d*$", ""))
  if v == "" or #v > M.VERSION_MAX or v:find("[^\32-\126]") then return nil end
  return v
end

-- The identify fields.
function M.identity(connectorVersion)
  return M.GAME, { name = M.CONNECTOR_NAME, version = connectorVersion }
end

-- The game.profile capability; gameVersion and icon are left out while unknown.
function M.capability(gameVersion, icon)
  local p = { name = M.DISPLAY_NAME }
  if gameVersion then p.version = gameVersion end
  if icon then p.icon = { revision = icon.revision, size = icon.size } end
  return { name = "game.profile", parameters = p }
end

-- The getGameIcon answer for the declared icon (the bytes loaded at start, so revision and bytes always match).
function M.icon_response(icon)
  if not icon then return { success = false, errorMessage = "game_error: no game icon is available" } end
  icon.base64 = icon.base64 or sha256.base64(icon.bytes)
  return { revision = icon.revision, data = icon.base64 }
end

return M
