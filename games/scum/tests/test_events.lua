-- Event adapter: DTO whitelist, required fields, echo filter, dedup, identity on the wire.
local t = require("lib.t")
local F = require("lib.fakes")

local STEAM, STEAM2 = "76561198000000001", "76561198000000002"

local function last_event(native)
  local evs = native.frames("gameEvent")
  return evs[#evs] and evs[#evs].msg.payload
end

t.test("player-connected carries exactly the IGamePlayer fields, identity filled from the SteamID64", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:player_connected({ steamId = STEAM, name = "Alice", ip = "10.0.0.5", ping = 12, uobject = {}, slot = 3 })
  tk.pump()
  t.deep(last_event(native), { type = "player-connected", data = { player = {
    gameId = STEAM, name = "Alice", steamId = STEAM, platformId = "steam:" .. STEAM, ip = "10.0.0.5", ping = 12 } } })
end)

t.test("player-disconnected falls back to the last-known record when the game only knows the id", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:player_connected({ steamId = STEAM, name = "Alice" })
  app.events:player_disconnected({ gameId = STEAM, name = "" })
  tk.pump()
  t.eq(last_event(native).data.player.name, "Alice")
end)

t.test("chat after a reload (no IP from the login log) carries the last-known IP", function()
  local app, native, tk = F.app()
  tk.connect()
  app.known:remember({ gameId = STEAM, name = "Alice", ip = "10.0.0.5" })
  app.events:chat_message({ player = { steamId = STEAM, name = "Alice" }, msg = "hi", channel = "global" })
  tk.pump()
  t.eq(last_event(native).data.player.ip, "10.0.0.5")
end)

t.test("a player without a SteamID64 is never sent (no slot or entity ids as gameId)", function()
  local app, native, tk = F.app()
  tk.connect()
  t.eq(app.events:player_connected({ gameId = "17", name = "Bot" }), false)
  tk.pump()
  t.eq(#native.frames("gameEvent"), 0)
  t.eq(app.events.stats.dropped["player-connected"], 1)
end)

t.test("chat-message: channel validated, whisper keeps recipient, player-less lines are dropped (echo filter)", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:chat_message({ player = { steamId = STEAM, name = "A" }, msg = "hi", channel = "Local" })
  tk.pump()
  t.deep(last_event(native).data, { player = { gameId = STEAM, name = "A", steamId = STEAM, platformId = "steam:" .. STEAM },
    channel = "global", msg = "hi" })
  app.events:chat_message({ player = { steamId = STEAM, name = "A" }, msg = "psst", channel = "whisper",
    recipient = { steamId = STEAM2, name = "B" } })
  tk.pump()
  t.eq(last_event(native).data.recipient.gameId, STEAM2)
  local before = #native.frames("gameEvent")
  t.eq(app.events:chat_message({ msg = "[Takaro] server says hi" }), false)
  tk.pump()
  t.eq(#native.frames("gameEvent"), before, "server/echo line not re-ingested")
end)

t.test("player-death keeps attacker and position when known", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:player_death({ player = { steamId = STEAM, name = "A" }, attacker = { steamId = STEAM2, name = "B" },
    position = { x = 1.5, y = 2, z = 3 }, cause = "extra field" })
  tk.pump()
  local d = last_event(native).data
  t.eq(d.attacker.gameId, STEAM2)
  t.deep(d.position, { x = 1.5, y = 2, z = 3 })
  t.eq(d.cause, nil, "no extra keys")
end)

t.test("entity-killed requires entity and weapon; never a placeholder weapon", function()
  local app, native, tk = F.app()
  tk.connect()
  t.eq(app.events:entity_killed({ player = { steamId = STEAM, name = "A" }, entity = "Zombie" }), false)
  app.events:entity_killed({ player = { steamId = STEAM, name = "A" }, entity = "Puppet", weapon = "M1911", code = "x" })
  tk.pump()
  local evs = native.frames("gameEvent")
  t.eq(#evs, 1)
  t.deep(evs[1].msg.payload.data, { player = { gameId = STEAM, name = "A", steamId = STEAM, platformId = "steam:" .. STEAM },
    entity = "Puppet", weapon = "M1911" })
end)

t.test("dedup: the same keyed signal within the window is sent once", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:player_connected({ steamId = STEAM, name = "A" })
  app.events:player_connected({ steamId = STEAM, name = "A" })
  tk.pump()
  t.eq(#native.frames("gameEvent"), 1)
  native.clock = native.clock + 4000
  app.events:player_connected({ steamId = STEAM, name = "A" })
  tk.pump()
  t.eq(#native.frames("gameEvent"), 2, "a later reconnect is a new event")
end)

t.test("log event", function()
  local app, native, tk = F.app()
  tk.connect()
  app.events:log_line("Server started")
  tk.pump()
  t.deep(last_event(native), { type = "log", data = { msg = "Server started" } })
end)

t.test("the event queue is bounded while Takaro is unreachable (oldest dropped)", function()
  local protocol = require("takaro.protocol")
  local app, native, tk = F.app()
  for i = 1, protocol.EVENT_QUEUE_LIMIT + 5 do app.events:log_line("line " .. i) end
  t.eq(#app.client.events, protocol.EVENT_QUEUE_LIMIT)
  t.eq(app.client.stats.eventsDropped, 5)
  tk.connect()
  t.eq(native.frames("gameEvent")[1].msg.payload.data.msg, "line 6")
end)

t.run("events")
