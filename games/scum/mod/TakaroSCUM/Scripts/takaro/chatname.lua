-- The sender name on messages Takaro sends (sendMessage). Takaro's senderNameOverride (filled from the server's
-- serverChatName setting by newer Takaro releases) wins over SERVER_CHAT_NAME in TakaroConfig.txt; neither means no
-- name, the plain orange server line. SCUM's server chat lines (MiscStatics BroadcastChatLine/SendChatLineToPlayer)
-- carry text only, so the name goes in front of the text.
local util = require("takaro.util")

local M = {}

local function clean(name)
  name = util.opt_string(name)
  if not name then return nil end
  name = util.trim(name)
  return name ~= "" and name or nil
end

-- The name to show, or nil for none.
function M.resolve(senderNameOverride, configuredName)
  return clean(senderNameOverride) or clean(configuredName)
end

-- The chat text a player sees: "Name: message", or the message unchanged without a name.
function M.render(senderName, message)
  if not senderName then return message end
  return senderName .. ": " .. message
end

return M
