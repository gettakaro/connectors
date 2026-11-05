-- Server chat name: resolution order (senderNameOverride > SERVER_CHAT_NAME > none) and the rendered chat text.
local t = require("lib.t")
local F = require("lib.fakes")
local chatname = require("takaro.chatname")
local config = require("takaro.config")
local json = require("takaro.json")

local STEAM = "76561198000000001"

t.test("resolve: the override wins over the config value", function()
  t.eq(chatname.resolve("Bot", "Cfg"), "Bot")
end)

t.test("resolve: no override falls back to the config value", function()
  t.eq(chatname.resolve(nil, "Cfg"), "Cfg")
end)

t.test("resolve: empty or whitespace counts as unset, names are trimmed", function()
  t.eq(chatname.resolve("", "Cfg"), "Cfg")
  t.eq(chatname.resolve("   ", "Cfg"), "Cfg")
  t.eq(chatname.resolve("  Bot  ", "Cfg"), "Bot")
  t.eq(chatname.resolve(nil, " \t "), nil)
  t.eq(chatname.resolve(nil, nil), nil)
  t.eq(chatname.resolve(json.null, "Cfg"), "Cfg", "JSON null override")
end)

t.test("render: 'Name: message' with a name, the message unchanged without one", function()
  t.eq(chatname.render("Bot", "hello"), "Bot: hello")
  t.eq(chatname.render(nil, "hello"), "hello")
end)

t.test("config: SERVER_CHAT_NAME is read, empty by default, and shown by describe()", function()
  t.eq(config.build({}).serverChatName, "")
  local c = config.build(config.parse("SERVER_CHAT_NAME =  My Server \n"))
  t.eq(c.serverChatName, "My Server")
  t.ok(config.describe(c):find('serverChatName="My Server"', 1, true))
end)

local function sent(extra, opts)
  local game = F.game({ send_message = { ok = true } })
  local _, _, tk = F.app({ game = game, extra = extra })
  tk.connect()
  tk.request("sendMessage", { message = "hi", opts = opts })
  tk.pump()
  return game.calls[1].params
end

t.test("sendMessage: SERVER_CHAT_NAME is the sender when Takaro sends none, global and directed", function()
  t.deep(sent("SERVER_CHAT_NAME=Cfg", {}), { message = "hi", senderName = "Cfg" })
  t.deep(sent("SERVER_CHAT_NAME=Cfg", { recipient = { gameId = STEAM } }),
    { message = "hi", recipientGameId = STEAM, senderName = "Cfg" })
end)

t.test("sendMessage: senderNameOverride wins over SERVER_CHAT_NAME; blank override falls back", function()
  t.deep(sent("SERVER_CHAT_NAME=Cfg", { senderNameOverride = "Bot" }), { message = "hi", senderName = "Bot" })
  t.deep(sent("SERVER_CHAT_NAME=Cfg", { senderNameOverride = "  " }), { message = "hi", senderName = "Cfg" })
end)

t.test("sendMessage: no override and no config value sends no name (unchanged rendering)", function()
  t.deep(sent(nil, {}), { message = "hi" })
  t.deep(sent("SERVER_CHAT_NAME=", { senderNameOverride = "" }), { message = "hi" })
end)

t.run("chatname")
