-- Takaro protocol version 1: negotiation, Capability Manifest with the map Dimension, the version 1 response shape,
-- the acknowledged event stream, the manifest update after the world bounds are read, and the map calibration.
local t = require("lib.t")
local F = require("lib.fakes")
local json = require("takaro.json")
local map = require("takaro.map")
local v1 = require("takaro.v1")
local coverage = require("takaro.coverage")

local STEAM = "76561198000000001"

local function connect_v1(tk, native)
  native.open()
  tk.pump()
  native.deliver({ type = "identifyResponse", payload = { gameServerId = "gs-1", protocolVersion = 1 } })
  tk.pump()
end

local function names(manifest)
  local out = {}
  for _, c in ipairs(manifest) do out[c.name] = c end
  return out
end

local function last_response(native, requestId)
  for i = #native.sent, 1, -1 do
    local s = native.sent[i]
    if s.msg.type == "response" and s.msg.requestId == requestId then return s.msg end
  end
end

t.test("identify offers protocol versions [0, 1] with the manifest: live capabilities, events and the map", function()
  local a, native, tk = F.app()
  native.open()
  tk.pump()
  local id = native.frames("identify")[1].msg.payload
  t.deep(id.protocolVersions, { 0, 1 })
  local caps = names(id.capabilities)
  for _, name in ipairs({ "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
    "items.list", "items.give", "entities.list", "chat.send", "console.execute", "moderation.kick", "moderation.ban",
    "moderation.unban", "moderation.listBans", "server.shutdown", "events.player-connected",
    "events.player-disconnected", "events.chat-message", "events.player-death", "events.entity-killed",
    "events.log" }) do
    t.ok(caps[name], "declares " .. name)
  end
  t.eq(caps["locations.list"], nil, "the [] placeholder is not declared")
  local dims = caps.map.parameters.dimensions
  t.eq(#dims, 1)
  t.deep(dims[1], {
    id = "the_island", name = "The Island",
    axes = { horizontal = { field = "x", direction = "left" }, vertical = { field = "y", direction = "up" } },
    bounds = { horizontal = { min = -904800, max = 619200 }, vertical = { min = -904800, max = 619200 } },
    source = { type = "none" },
  })
  t.eq(a.client.version, 0, "version 0 until Takaro chooses")
end)

t.test("TAKARO_PROTOCOL=0 sends the plain version 0 identify", function()
  local _, native, tk = F.app({ extra = "TAKARO_PROTOCOL=0" })
  native.open()
  tk.pump()
  local id = native.frames("identify")[1].msg.payload
  t.eq(id.protocolVersions, nil)
  t.eq(id.capabilities, nil)
end)

t.test("an older Takaro without protocolVersion keeps version 0 (pong confirms, plain responses)", function()
  local app, native, tk = F.app({ script = { get_player_location = { ok = false, reason = "player is offline" } } })
  tk.connect("gs-old")
  t.eq(app.client.version, 0)
  local r = tk.request("getPlayerLocation", { gameId = STEAM })
  tk.pump()
  t.eq(tk.response(r).success, false, "version 0 failure shape")
end)

t.test("a version 1 identify refusal (top-level error) closes the epoch", function()
  local app, native, tk = F.app()
  native.open()
  tk.pump()
  native.deliver({ type = "identifyResponse", error = { code = "no_shared_protocol_version", message = "no" } })
  tk.pump()
  t.ok(not app.client.identified)
  t.eq(#native.closes, 1)
  local logged = table.concat(native.logs, "\n")
  t.ok(logged:find("TAKARO_PROTOCOL=0", 1, true), "hint names the setting")
end)

t.test("version 1: getPlayerLocation carries the dimension; no position answers null", function()
  local pos = { ok = true, value = { x = -569603.3, y = -711185.75, z = 14280.4 } }
  local app, native, tk = F.app({ script = { get_player_location = function(p)
    if p.gameId == STEAM then return pos end
    return { ok = false, reason = "player is offline" }
  end } })
  connect_v1(tk, native)
  t.eq(app.client.version, 1)
  local a = tk.request("getPlayerLocation", { gameId = STEAM })
  local b = tk.request("getPlayerLocation", { gameId = "76561198000000002" })
  tk.pump()
  local ra, rb = last_response(native, a), last_response(native, b)
  t.deep(ra.payload, { x = -569603.3, y = -711185.75, z = 14280.4, dimension = "the_island" })
  t.eq(ra.error, nil)
  t.eq(rb.payload, nil, "null payload decodes to nil")
  t.eq(rb.error, nil)
  local raw
  for _, s in ipairs(native.sent) do if s.msg.requestId == b then raw = s.text end end
  t.ok(raw:find('"payload":null', 1, true), "explicit null payload")
end)

t.test("version 1: failures become { code, message } errors; a refused console command stays a CommandOutput", function()
  local _, native, tk = F.app({ script = {
    kick_player = { ok = false, reason = "player is offline" },
    send_message = { ok = false, reason = "BroadcastChatLine failed" },
  } })
  connect_v1(tk, native)
  local bad = tk.request("giveItem", { gameId = STEAM })
  local off = tk.request("kickPlayer", { gameId = STEAM, reason = "x" })
  local ge = tk.request("sendMessage", { message = "hi" })
  local unk = tk.request("frobnicate", {})
  local cmd = tk.request("executeConsoleCommand", { command = "#SetTime 12" })
  local ok = tk.request("getPlayers", {})
  tk.pump()
  t.deep(last_response(native, bad).error, { code = "invalid_args", message = "invalid_args: item is required" })
  t.eq(last_response(native, off).error.code, "not_found")
  t.eq(last_response(native, ge).error.code, "game_error")
  t.eq(last_response(native, unk).error.code, "unsupported")
  local c = last_response(native, cmd)
  t.eq(c.error, nil)
  t.eq(c.payload.success, false)
  t.deep(last_response(native, ok).payload, {}, "a bare array stays a payload")
end)

t.test("version 1: a failed inventory read is an error (Takaro skips the sync), never an empty payload", function()
  local _, native, tk = F.app({ script = {
    get_player_inventory = function(params)
      if params.gameId == STEAM then return { ok = false, reason = "inventory walk found nothing but 3 items are owned by the player" } end
      return { ok = false, reason = "player has no character right now" }
    end,
  } })
  connect_v1(tk, native)
  local walk = tk.request("getPlayerInventory", { gameId = STEAM })
  local dead = tk.request("getPlayerInventory", { gameId = "76561198000000002" })
  tk.pump()
  local w, d = last_response(native, walk), last_response(native, dead)
  t.eq(w.payload, nil)
  t.eq(w.error.code, "game_error")
  t.ok(w.error.message:find("3 items are owned"), w.error.message)
  t.eq(d.payload, nil)
  t.eq(d.error.code, "not_found", "no character")
end)

t.test("version 1 events: streamId + seq, cumulative eventAck, resend with the same seq after a reconnect", function()
  local app, native, tk = F.app()
  connect_v1(tk, native)
  local c = app.client
  c:send_event("log", { msg = "one" })
  c:send_event("log", { msg = "two" })
  c:send_event("log", { msg = "three" })
  local evs = native.frames("gameEvent")
  t.eq(#evs, 3)
  t.eq(evs[1].msg.payload.seq, 1)
  t.eq(evs[3].msg.payload.seq, 3)
  t.eq(evs[1].msg.payload.streamId, c.streamId)
  t.ok(c.streamId:match("^[A-Za-z0-9_-]+$") and #c.streamId <= 128, "stream id shape")
  t.eq(evs[2].msg.payload.data.msg, "two")
  -- a pong watermark does not confirm on version 1
  native.confirm(99)
  tk.pump()
  t.eq(#c.inflight, 3)
  -- an ack of another stream is ignored; ours confirms through upTo
  native.deliver({ type = "eventAck", payload = { streamId = "other", upTo = 3 } })
  native.deliver({ type = "eventAck", payload = { streamId = c.streamId, upTo = 1 } })
  tk.pump()
  t.eq(#c.inflight, 2)
  t.eq(c.stats.eventsConfirmed, 1)
  -- reconnect: the unacknowledged events go out again, same seq, same stream
  native.drop("network")
  tk.pump()
  connect_v1(tk, native)
  local again = native.frames("gameEvent")
  t.eq(#again, 5)
  t.eq(again[4].msg.payload.seq, 2)
  t.eq(again[5].msg.payload.seq, 3)
  t.eq(again[5].msg.payload.streamId, c.streamId)
end)

t.test("version 1: no eventAck for 30 s closes the connection so the events are resent", function()
  local app, native, tk = F.app()
  connect_v1(tk, native)
  app.client:send_event("log", { msg = "x" })
  native.clock = native.clock + 29000
  tk.pump()
  t.eq(#native.closes, 0)
  native.clock = native.clock + 2000
  tk.pump()
  t.eq(#native.closes, 1)
  t.eq(native.closes[1].reason, "event ack timeout")
end)

t.test("version 1: eventError and capabilitiesResponse errors are logged, not fatal", function()
  local app, native, tk = F.app()
  connect_v1(tk, native)
  native.deliver({ type = "eventError", payload = { streamId = app.client.streamId, seq = 4 },
    error = { code = "invalid_args", message = "bad event" } })
  native.deliver({ type = "capabilitiesResponse", requestId = "x", error = { code = "invalid_args", message = "bad" } })
  tk.pump()
  t.eq(app.client.stats.eventErrors, 1)
  t.eq(app.client.stats.manifestErrors, 1)
  t.ok(app.client.identified)
end)

t.test("world bounds read from the server: same frame -> no update; a different frame -> capabilities frame", function()
  local g = F.game()
  local world = { minX = -954800, maxX = 669200, minY = -954800, maxY = 669200 }
  g.world_bounds = function() return { ok = true, value = world } end
  local app, native, tk = F.app({ game = g })
  connect_v1(tk, native)
  tk.pump()
  t.deep(app.mapBounds, { minX = -904800, maxX = 619200, minY = -904800, maxY = 619200 })
  t.eq(#native.frames("capabilities"), 0, "same bounds as declared")

  local g2 = F.game()
  local tries = 0
  g2.world_bounds = function()
    tries = tries + 1
    if tries == 1 then return { ok = false, reason = "world not loaded" } end
    return { ok = true, value = { minX = -1000000, maxX = 1000000, minY = -500000, maxY = 500000 } }
  end
  local app2, n2, tk2 = F.app({ game = g2 })
  connect_v1(tk2, n2)
  t.eq(app2.mapBounds, nil, "first read failed")
  n2.clock = n2.clock + 10000
  tk2.pump()
  local caps = n2.frames("capabilities")
  t.eq(#caps, 1)
  local m = names(caps[1].msg.payload.capabilities)
  t.deep(m.map.parameters.dimensions[1].bounds,
    { horizontal = { min = -950000, max = 950000 }, vertical = { min = -450000, max = 450000 } })
  -- a reconnect identifies with the new manifest right away
  n2.drop()
  tk2.pump()
  n2.open()
  tk2.pump()
  local ids = n2.frames("identify")
  t.deep(names(ids[#ids].msg.payload.capabilities).map.parameters.dimensions[1].bounds.vertical,
    { min = -450000, max = 450000 })
end)

t.test("invalid world bounds are ignored (the built-in frame stays)", function()
  t.ok(not map.valid_bounds({ minX = 0, maxX = 10, minY = 0, maxY = 10 }))
  t.ok(not map.valid_bounds({ minX = 0 / 0, maxX = 1e6, minY = 0, maxY = 1e6 }))
  t.ok(not map.valid_bounds(nil))
  t.deep(map.dimension(nil).bounds.horizontal, { min = -904800, max = 619200 })
end)

-- Live calibration 2026-10-14 (SCUM 1.3.4.2): server position vs the red player dot on the in-game map
-- (900 x 900 px on screen, frame registered against the texture; fractions from the top-left corner, pixel centres).
local CALIBRATION = {
  { spot = "Z0 road south of the bay", x = -569603.3, y = -711185.75, u = 0.7800, v = 0.8729 },
  { spot = "B1 airfield", x = -254856, y = -24248, u = 0.5734, v = 0.4222 },
  { spot = "C4 west meadow", x = 425600, y = 182000, u = 0.1271, v = 0.2870 },
  { spot = "D0 hills", x = -711200, y = 474320, u = 0.8729, v = 0.0951 },
}

t.test("map calibration: every live spot projects within 1 px of the in-game marker on the 2048 px texture", function()
  for _, c in ipairs(CALIBRATION) do
    local u, v = map.project(nil, c.x, c.y)
    t.ok(math.abs(u - c.u) * 2048 < 1, c.spot .. " horizontal " .. u .. " vs " .. c.u)
    t.ok(math.abs(v - c.v) * 2048 < 1, c.spot .. " vertical " .. v .. " vs " .. c.v)
  end
end)

t.test("capabilities.json declares the same Dimension and protocol versions as the connector", function()
  local f = assert(io.open("capabilities.json", "rb"))
  local doc = json.decode(f:read("a"))
  f:close()
  t.deep(doc.map, map.dimension())
  t.deep(doc.protocolVersions, { 0, 1 })
end)

t.test("every declared action capability is live-supported in the coverage registry", function()
  for _, c in ipairs(v1.manifest()) do
    if c.name ~= "map" and not c.name:find("^events%.") then
      local action
      for a, n in pairs(v1.CAPABILITY_OF) do if n == c.name then action = a end end
      t.eq(coverage.actions[action].status, "live-supported", c.name)
    end
  end
end)

t.run("v1")
