-- WIRE-1..6 from the acceptance checklist, args shapes, explicit JSON null, always-respond, timeouts.
local t = require("lib.t")
local F = require("lib.fakes")
local json = require("takaro.json")
local coverage = require("takaro.coverage")
local actions = require("takaro.actions")

local STEAM = "76561198000000001"

t.test("identify on open carries both tokens and the server name; identified after identifyResponse", function()
  local app, native, tk = F.app()
  t.eq(native.connected.url, "wss://connect.example.test/")
  tk.connect("gs-1")
  local id = native.frames("identify")
  t.eq(#id, 1, "one identify")
  t.eq(id[1].kind, "control")
  t.eq(id[1].msg.payload.registrationToken, F.REG_TOKEN)
  t.eq(id[1].msg.payload.identityToken, F.ID_TOKEN)
  t.eq(id[1].msg.payload.name, "SCUM test")
  t.ok(app.client.identified, "identified")
  t.eq(app.client.gameServerId, "gs-1")
  t.eq(native.identifiedEpochs[1], native.epoch, "backoff reset for this epoch")
end)

t.test("identify rejection closes the epoch (native backoff) and stays unidentified", function()
  local app, native, tk = F.app()
  native.open()
  tk.pump()
  native.deliver({ type = "identifyResponse", payload = { error = { name = "BadRequestError",
    message = "Invalid registrationToken provided", http = 400 } } })
  tk.pump()
  t.ok(not app.client.identified)
  t.eq(#native.closes, 1)
  t.eq(native.closes[1].reason, "identify rejected")
  t.eq(#native.identifiedEpochs, 0)
end)

t.test("WIRE-1: an inbound ping is answered with a pong on the same connection", function()
  local _, native, tk = F.app()
  tk.connect()
  native.deliver({ type = "ping" })
  tk.pump()
  local pongs = native.frames("pong")
  t.eq(#pongs, 1)
  t.eq(pongs[1].kind, "control")
end)

t.test("WIRE-2: concurrent requests are answered on their own requestId, even out of order", function()
  local game = F.game({ list_players = "defer", execute_command = "defer" })
  local _, native, tk = F.app({ game = game, extra = "COMMAND_ALLOWLIST=listplayers" })
  tk.connect()
  local a = tk.request("getPlayers", {}, "A-players")
  local b = tk.request("executeConsoleCommand", { command = "#ListPlayers" }, "B-command")
  tk.pump()
  t.eq(#game.pending, 2)
  -- the command finishes first, then the player list
  game.pending[2].done({ ok = true, value = "2 players" })
  game.pending[1].done({ ok = true, value = { { steamId = STEAM, name = "Alice" } } })
  tk.pump()
  local pa, pb = tk.response(a), tk.response(b)
  t.eq(pa[1].gameId, STEAM, "player list on A")
  t.eq(pb.rawResult, "2 players", "command result on B")
  t.eq(#tk.responses(a), 1)
  t.eq(#tk.responses(b), 1)
end)

-- every encoding Takaro (or a module) can produce for "no arguments"
local EMPTY_SHAPES = {
  { name = "absent", raw = nil },
  { name = "[]", raw = "[]" },
  { name = "{}", raw = "{}" },
  { name = "json string {}", raw = "\"{}\"" },
  { name = "json string []", raw = "\"[]\"" },
  { name = "null", raw = "null" },
  { name = "empty string", raw = "\"\"" },
}

local function request_raw_args(tk, action, rawArgs, requestId)
  local args = rawArgs and (",\"args\":" .. rawArgs) or ""
  tk.request_raw('{"type":"request","requestId":"' .. requestId .. '","payload":{"action":"' .. action .. '"' .. args .. "}}")
end

t.test("WIRE-3: every action answers identically for [], {}, JSON-string, null and absent args", function()
  local _, native, tk = F.app()
  tk.connect()
  for _, action in ipairs(coverage.ACTIONS) do
    local first
    for i, shape in ipairs(EMPTY_SHAPES) do
      local rid = action .. "-" .. i
      request_raw_args(tk, action, shape.raw, rid)
      tk.pump()
      local payload = tk.response(rid)
      t.ok(payload ~= nil, action .. " answered for " .. shape.name)
      if first == nil then first = payload else t.deep(payload, first, action .. " same result for " .. shape.name) end
    end
  end
end)

t.test("WIRE-3: object args as an object, a JSON string and a double-encoded JSON string are equivalent", function()
  local game = F.game({ teleport_player = { ok = true } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  local obj = { gameId = STEAM, x = 1, y = 2, z = 3 }
  tk.request("teleportPlayer", obj, "o")
  tk.request("teleportPlayer", json.encode(obj), "s")
  tk.request("teleportPlayer", json.encode(json.encode(obj)), "ss")
  tk.pump()
  t.eq(#game.calls, 3)
  for i = 1, 3 do t.deep(game.calls[i].params, { gameId = STEAM, x = 1, y = 2, z = 3 }) end
  for _, rid in ipairs({ "o", "s", "ss" }) do t.deep(tk.response(rid), { success = true }) end
end)

local PLAYER_ACTIONS = {
  getPlayer = {}, getPlayerLocation = {}, getPlayerInventory = {}, giveItem = { item = "Weapon_M1911", amount = 1 },
  teleportPlayer = { x = 1, y = 2, z = 3 }, kickPlayer = { reason = "r" }, banPlayer = { reason = "r" },
  unbanPlayer = {},
}

t.test("WIRE-4: flat { gameId } and nested { player = { gameId } } reach the adapter as the same player", function()
  for action, extra in pairs(PLAYER_ACTIONS) do
    local game = F.game()
    local _, native, tk = F.app({ game = game })
    tk.connect()
    local flat = { gameId = STEAM }
    local nested = { player = { gameId = STEAM, name = "Alice", id = "takaro-pog-uuid" } }
    local numeric = { gameId = 76561198000000001 }
    for k, v in pairs(extra) do flat[k] = v; nested[k] = v; numeric[k] = v end
    tk.request(action, flat, "flat")
    tk.request(action, nested, "nested")
    tk.request(action, json.encode(nested), "nested-string")
    tk.pump()
    t.eq(#game.calls, 3, action .. " adapter calls")
    for i = 1, 3 do t.eq(game.calls[i].params.gameId, STEAM, action .. " call " .. i) end
    t.deep(tk.response("nested"), tk.response("flat"), action .. " same answer")
  end
end)

t.test("explicit JSON null for optional args behaves like an absent key (module-driven calls)", function()
  local game = F.game({ teleport_player = { ok = true }, ban_player = { ok = true }, give_item = { ok = true },
    send_message = { ok = true }, kick_player = { ok = true } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  local pl = '{"gameId":"' .. STEAM .. '","name":"Alice"}'
  tk.request_raw('{"type":"request","requestId":"tp","payload":{"action":"teleportPlayer","args":{"player":' .. pl ..
    ',"x":10,"y":20,"z":30,"dimension":null}}}')
  tk.request_raw('{"type":"request","requestId":"ban","payload":{"action":"banPlayer","args":{"player":' .. pl ..
    ',"reason":null,"expiresAt":null}}}')
  tk.request_raw('{"type":"request","requestId":"give","payload":{"action":"giveItem","args":{"player":' .. pl ..
    ',"item":"Weapon_M1911","amount":2,"quality":null}}}')
  tk.request_raw('{"type":"request","requestId":"msg","payload":{"action":"sendMessage","args":{"message":"hi","opts":null}}}')
  tk.request_raw('{"type":"request","requestId":"msg2","payload":{"action":"sendMessage","args":{"message":"hi","opts":{"recipient":null,"senderNameOverride":null}}}}')
  tk.request_raw('{"type":"request","requestId":"kick","payload":{"action":"kickPlayer","args":{"gameId":"' .. STEAM .. '","reason":null}}}')
  tk.pump()
  for _, rid in ipairs({ "tp", "ban", "give", "msg", "msg2", "kick" }) do t.deep(tk.response(rid), { success = true }, rid) end
  local byName = {}
  for _, c in ipairs(game.calls) do byName[c.name] = byName[c.name] or {}; table.insert(byName[c.name], c.params) end
  t.deep(byName.teleport_player[1], { gameId = STEAM, x = 10, y = 20, z = 30 })
  t.deep(byName.ban_player[1], { gameId = STEAM, reason = "Banned by Takaro" })
  t.deep(byName.give_item[1], { gameId = STEAM, item = "Weapon_M1911", amount = 2 })
  t.deep(byName.send_message[1], { message = "hi" }, "global message")
  t.deep(byName.send_message[2], { message = "hi" }, "null recipient is global")
  t.deep(byName.kick_player[1], { gameId = STEAM, reason = "Kicked by Takaro" })
end)

t.test("sendMessage: opts.recipient.gameId is directed and senderNameOverride is passed", function()
  local game = F.game({ send_message = { ok = true } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("sendMessage", { message = "psst", opts = { recipient = { gameId = STEAM }, senderNameOverride = "Bot" } })
  tk.pump()
  t.deep(game.calls[1].params, { message = "psst", recipientGameId = STEAM, senderName = "Bot" })
end)

t.test("WIRE-5 (Lua side): reconnect re-identifies once per epoch, stale responses never cross epochs", function()
  local game = F.game({ list_players = "defer" })
  local app, native, tk = F.app({ game = game })
  tk.connect("gs-1")
  local epoch1 = native.epoch
  tk.request("getPlayers", {}, "slow")
  tk.pump()
  native.drop("receive error 12030")
  tk.pump()
  t.ok(not app.client.identified, "unidentified after close")
  tk.connect("gs-1")
  t.ok(native.epoch ~= epoch1, "new epoch")
  t.eq(#native.frames("identify"), 2, "identify again on the new connection")
  -- the old request finishes after the reconnect: it must not be answered on the new connection
  game.pending[1].done({ ok = true, value = {} })
  tk.pump()
  t.eq(tk.response("slow"), nil, "no response on the new epoch")
  t.eq(app.client.stats.staleResponses, 1)
  -- frames of the old epoch are ignored
  native.inbox[#native.inbox + 1] = { kind = "frame", epoch = epoch1, text = '{"type":"ping"}' }
  local pongs = #native.frames("pong")
  tk.pump()
  t.eq(#native.frames("pong"), pongs, "late frame of an old epoch ignored")
end)

t.test("WIRE-5 (Lua side): events wait for identify, survive a drop until confirmed, and are re-sent", function()
  local app, native, tk = F.app()
  app.events:log_line("before connect")
  tk.pump()
  t.eq(#native.frames("gameEvent"), 0, "not sent before identify")
  tk.connect()
  t.eq(#native.frames("gameEvent"), 1, "flushed after identify")
  app.events:log_line("second")
  tk.pump()
  t.eq(#native.frames("gameEvent"), 2)
  native.confirm(1) -- pong confirmed only the first
  tk.pump()
  t.eq(app.client.stats.eventsConfirmed, 1)
  native.drop("dead link")
  tk.pump()
  t.eq(#app.client.events, 1, "unconfirmed event requeued")
  tk.connect()
  local evs = native.frames("gameEvent")
  t.eq(#evs, 3, "unconfirmed event re-sent after reconnect")
  t.eq(evs[3].msg.payload.data.msg, "second")
  t.eq(evs[3].msg.payload.type, "log")
end)

t.test("WIRE-6: an unknown action gets a structured failure on its own requestId", function()
  local _, native, tk = F.app()
  tk.connect()
  tk.request("getMapImage", {}, "u1")
  tk.request("definitelyNotAnAction", {}, "u2")
  tk.request_raw('{"type":"request","requestId":"u3","payload":{}}')
  tk.pump()
  t.deep(tk.response("u1"), { success = false, errorMessage = "unknown action 'getMapImage'" })
  t.eq(tk.response("u2").success, false)
  t.eq(tk.response("u3").success, false, "missing action")
end)

t.test("always respond: every action answers with a stub adapter, in the shape Takaro validates", function()
  local _, native, tk = F.app()
  tk.connect()
  for _, action in ipairs(coverage.ACTIONS) do tk.request(action, { gameId = STEAM }, "all-" .. action) end
  tk.pump()
  for _, action in ipairs(coverage.ACTIONS) do
    local p, frame = tk.response("all-" .. action)
    t.ok(p ~= nil, action .. " answered")
    if action == "getPlayerInventory" then
      t.ok(type(p.error) == "string", "getPlayerInventory: a failing adapter answers { error }, Takaro keeps the last inventory")
    elseif actions.LIST_ACTIONS[action] then
      t.eq(frame.text:match('"payload":(.)'), "[", action .. " is a bare array")
    end
  end
  local gp = tk.response("all-getPlayer")
  t.deep(gp, { gameId = STEAM, name = STEAM, steamId = STEAM, platformId = "steam:" .. STEAM }, "synthesised IGamePlayer")
  t.deep(tk.response("all-testReachability"), { connectable = false, reason = "game adapter: not_implemented" })
  t.deep(tk.response("all-getMapInfo"), actions.MAP_INFO_FALLBACK)
  t.eq(tk.response("all-sendMessage").success, false)
end)

t.test("getPlayer for an offline player answers the last-known record", function()
  local game = F.game({ list_players = { ok = true, value = { { steamId = STEAM, name = "Alice", ip = "10.0.0.5", ping = 40 } } } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("getPlayers", {}, "list")
  tk.request("getPlayer", { gameId = STEAM }, "one")
  tk.pump()
  t.deep(tk.response("one"), { gameId = STEAM, name = "Alice", steamId = STEAM, platformId = "steam:" .. STEAM, ip = "10.0.0.5" })
end)

t.test("getPlayers drops players without a SteamID64 and fills identity fields", function()
  local game = F.game({ list_players = { ok = true, value = {
    { steamId = STEAM, name = "Alice", ping = 33.7 }, { steamId = "0", name = "Synthetic admin" }, { name = "x" } } } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("getPlayers", nil, "p")
  tk.pump()
  t.deep(tk.response("p"), { { gameId = STEAM, name = "Alice", steamId = STEAM, platformId = "steam:" .. STEAM, ping = 33 } })
end)

t.test("executeConsoleCommand: allowlist refuses with command_not_allowed and admits listed commands", function()
  local game = F.game({ execute_command = { ok = true, value = "done" } })
  local _, native, tk = F.app({ game = game, extra = "COMMAND_ALLOWLIST=announce, ListPlayers" })
  tk.connect()
  tk.request("executeConsoleCommand", { command = "#SetGodMode true" }, "deny")
  tk.request("executeConsoleCommand", { command = "#Announce hello" }, "allow")
  tk.request("executeConsoleCommand", { command = "listplayers" }, "allow2")
  tk.request("executeConsoleCommand", { command = "   " }, "empty")
  tk.pump()
  t.deep(tk.response("deny"), { success = false, rawResult = "", errorMessage = "command_not_allowed: 'setgodmode' is not in COMMAND_ALLOWLIST" })
  t.deep(tk.response("allow"), { success = true, rawResult = "done" })
  t.deep(tk.response("allow2"), { success = true, rawResult = "done" })
  t.eq(tk.response("empty").success, false)
  t.eq(#game.calls, 2, "only allowed commands reach the game")
end)

t.test("shutdown is refused by default and, when enabled, answers before it runs", function()
  local game = F.game({ shutdown = { ok = true } })
  local app, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("shutdown", {}, "s1")
  tk.pump()
  t.eq(tk.response("s1").success, false)
  t.ok(tk.response("s1").errorMessage:find("ENABLE_DESTRUCTIVE_COMMANDS", 1, true))
  t.eq(#game.calls, 0)

  local game2 = F.game({ shutdown = { ok = true } })
  local app2, native2, tk2 = F.app({ game = game2, extra = "ENABLE_DESTRUCTIVE_COMMANDS=true" })
  tk2.connect()
  tk2.request("shutdown", {}, "s2")
  tk2.pump()
  t.deep(tk2.response("s2"), { success = true })
  t.eq(#game2.calls, 0, "not yet: the response goes first")
  native2.clock = native2.clock + 1500
  tk2.pump()
  t.eq(game2.calls[1].name, "shutdown")
end)

t.test("a handler that never finishes is answered with its fallback before Takaro's 10 s budget", function()
  local game = F.game({ list_players = "defer", reachability = "defer", get_player = "defer", give_item = "defer" })
  local app, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("getPlayers", {}, "lp")
  tk.request("testReachability", {}, "tr")
  tk.request("getPlayer", { player = { gameId = STEAM } }, "gp")
  tk.request("giveItem", { gameId = STEAM, item = "X", amount = 1 }, "gi")
  tk.pump()
  native.clock = native.clock + 4100
  tk.pump()
  t.deep(tk.response("tr"), { connectable = false, reason = "game adapter: timeout" }, "reachability within 5 s")
  t.eq(tk.response("lp"), nil, "others still waiting")
  native.clock = native.clock + 4500
  tk.pump()
  t.deep(tk.response("lp"), {})
  t.eq(tk.response("gp").gameId, STEAM)
  t.eq(tk.response("gi").success, false)
  -- a late completion is not answered twice
  for _, p in ipairs(game.pending) do p.done({ ok = true, value = {} }) end
  tk.pump()
  t.eq(#tk.responses("lp"), 1)
  t.eq(app.client.stats.timeouts, 4)
end)

t.test("an adapter that throws still produces an answer", function()
  local game = F.game({ list_bans = function() error("boom") end, kick_player = function() error("boom") end })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("listBans", {}, "lb")
  tk.request("kickPlayer", { gameId = STEAM }, "k")
  tk.pump()
  t.deep(tk.response("lb"), {})
  t.eq(tk.response("k").success, false)
end)

t.test("listBans: every entry is a valid BanDTO (reason always a string, last-known name, expiry kept)", function()
  local game = F.game({ list_bans = { ok = true, value = {
    { player = { steamId = STEAM } },
    { player = { steamId = "76561198000000009", name = "Bob" }, reason = "cheat", expiresAt = "2099-01-01T00:00:00Z" },
    { player = { steamId = "not-a-steam-id" } },
  } } })
  local app, _, tk = F.app({ game = game })
  tk.connect()
  app.known:remember({ gameId = STEAM, name = "Alice", steamId = STEAM, platformId = "steam:" .. STEAM })
  tk.request("listBans", {}, "lb")
  tk.pump()
  local r = tk.response("lb")
  t.eq(#r, 2)
  t.eq(r[1].reason, "Banned on the game server")
  t.eq(r[1].player.name, "Alice")
  t.eq(r[1].expiresAt, nil)
  t.deep(r[2], { player = { gameId = "76561198000000009", name = "Bob", steamId = "76561198000000009",
    platformId = "steam:76561198000000009" }, reason = "cheat", expiresAt = "2099-01-01T00:00:00Z" })
end)

t.test("banPlayer keeps Takaro's own player name; a placeholder name never replaces a real one", function()
  local game = F.game({ ban_player = { ok = true } })
  local app, _, tk = F.app({ game = game })
  tk.connect()
  local OTHER = "76561198000000009"
  tk.request("banPlayer", { player = { gameId = OTHER, player = { name = "Bob", steamId = OTHER } }, reason = "r",
    expiresAt = json.null }, "b1")
  tk.pump()
  t.eq(game.calls[1].params.name, "Bob")
  t.eq(app.known:get(OTHER).name, "Bob")
  -- Takaro created the player from a bare SteamID (name = the id): not a name worth keeping
  tk.request("banPlayer", { player = { gameId = STEAM, player = { name = STEAM } }, reason = "r" }, "b2")
  tk.pump()
  t.eq(game.calls[2].params.name, nil)
  -- the real name arrives later; a later placeholder does not overwrite it
  app.known:remember({ gameId = STEAM, name = "Alice", steamId = STEAM })
  app.known:remember({ gameId = STEAM, name = STEAM, steamId = STEAM })
  t.eq(app.known:get(STEAM).name, "Alice")
end)

t.test("argument validation answers invalid_args instead of calling the game", function()
  local game = F.game()
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("teleportPlayer", { gameId = STEAM, x = "a", y = 1, z = 1 }, "tp")
  tk.request("giveItem", { gameId = STEAM, amount = 1 }, "gi")
  tk.request("giveItem", { gameId = STEAM, item = "X", amount = 0 }, "gi0")
  tk.request("banPlayer", { gameId = STEAM, expiresAt = "tomorrow" }, "ban")
  tk.request("kickPlayer", {}, "kick")
  tk.request("sendMessage", { message = "" }, "msg")
  tk.pump()
  for _, rid in ipairs({ "tp", "gi", "gi0", "ban", "kick", "msg" }) do
    t.ok(tk.response(rid).errorMessage:find("^invalid_args"), rid)
  end
  t.eq(#game.calls, 0)
end)

t.test("giveItem accepts item, code or name and numeric strings", function()
  local game = F.game({ give_item = { ok = true } })
  local _, native, tk = F.app({ game = game })
  tk.connect()
  tk.request("giveItem", { gameId = STEAM, code = "A", amount = "3" })
  tk.request("giveItem", { gameId = STEAM, name = "B" })
  tk.pump()
  t.deep(game.calls[1].params, { gameId = STEAM, item = "A", amount = 3 })
  t.deep(game.calls[2].params, { gameId = STEAM, item = "B", amount = 1 })
end)

t.test("error frames and junk frames are counted, never fatal", function()
  local app, native, tk = F.app()
  tk.connect()
  native.deliver({ type = "error", payload = { message = "validation failed" } })
  native.deliver("not json at all")
  native.deliver({ type = "somethingNew" })
  tk.pump()
  t.eq(app.client.stats.errorFrames, 1)
  t.eq(app.client.stats.badFrames, 1)
  t.eq(app.client.stats.unknownFrames, 1)
  t.ok(app.client.identified)
end)

t.test("getPlayerInventory: a game-side failure answers { error } (never [] = 'everything removed'); offline = []", function()
  local cases = {
    { { ok = true, value = { { code = "Apple", name = "Apple", amount = 1 } } }, { { code = "Apple", name = "Apple", amount = 1 } } },
    { { ok = false, reason = "player is offline" }, {} },
    { { ok = false, reason = "boom" }, { error = "inventory unavailable: boom" } },
    { function() error("lua blew up") end, nil },
  }
  for i, c in ipairs(cases) do
    local game = F.game({ get_player_inventory = c[1] })
    local _, _, tk = F.app({ game = game })
    tk.connect()
    tk.request("getPlayerInventory", { gameId = STEAM }, "inv")
    tk.pump()
    local r = tk.response("inv")
    if c[2] then t.deep(r, c[2], "case " .. i) else
      t.ok(type(r) == "table" and type(r.error) == "string" and r.error:find("lua blew up"), "case " .. i .. " adapter error") end
  end
end)

t.run("wire")
