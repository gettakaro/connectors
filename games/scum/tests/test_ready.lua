-- Connecting only once the game can answer: SCUM's game thread stalls for about a minute right after the world
-- appears, and every request Takaro sends during that stall times out.
local t = require("lib.t")
local F = require("lib.fakes")
local App = require("takaro.app")

local function game(loaded)
  local g = F.game()
  g.world_loaded = function() return loaded() end
  return g
end

local function run(app, native, ms, step)
  step = step or 100
  for _ = 1, math.floor(ms / step) do
    native.clock = native.clock + step
    app:tick()
  end
end

local REGISTERED = { ["/mod/TakaroRegistered.txt"] = F.registered_marker() }

t.test("a registered server waits for the world, then for a steady game thread, before connecting", function()
  local loaded = false
  local app, native = F.app({ game = game(function() return loaded end), files = REGISTERED })
  t.eq(native.connected, nil, "not at start: the world is not loaded")
  run(app, native, 30000)
  t.eq(native.connected, nil, "not while the world loads")
  loaded = true
  run(app, native, App.READY_SETTLE_MS - 100)
  t.eq(native.connected, nil, "settling")
  run(app, native, 200)
  t.ok(native.connected, "connects once the game thread was steady for READY_SETTLE_MS")
end)

t.test("a game-thread stall after the world loaded restarts the settle window", function()
  local app, native = F.app({ game = game(function() return true end), files = REGISTERED })
  run(app, native, 5000)
  native.clock = native.clock + 58000 -- one tick that took 58 s (measured on SCUM 1.3.6.3)
  app:tick()
  run(app, native, App.READY_SETTLE_MS - 200)
  t.eq(native.connected, nil, "the stall reset the window")
  run(app, native, 300)
  t.ok(native.connected)
  t.ok(table.concat(native.logs, "\n"):find("game thread stalled 58000 ms", 1, true), "the stall is logged")
end)

t.test("a world that never reports loaded still connects after READY_MAX_WAIT_MS", function()
  local app, native = F.app({ game = game(function() return false end), files = REGISTERED })
  run(app, native, App.READY_MAX_WAIT_MS - 1000, 1000)
  t.eq(native.connected, nil)
  run(app, native, 2000, 1000)
  t.ok(native.connected)
  t.ok(table.concat(native.logs, "\n"):find("connecting anyway", 1, true))
end)

t.test("a world_loaded that throws counts as not loaded and never escapes the tick", function()
  local g = F.game()
  g.world_loaded = function() error("boom") end
  local app, native = F.app({ game = g, files = REGISTERED })
  run(app, native, 20000)
  t.eq(native.connected, nil)
end)

t.test("a new server waits for both a steady world and its catalogues", function()
  local done = false
  local g = game(function() return true end)
  g.catalogs_done = function() return done end
  local app, native = F.app({ game = g })
  run(app, native, App.READY_SETTLE_MS + 1000)
  t.eq(native.connected, nil, "steady, but the catalogues are not ready")
  done = true
  app:tick()
  t.ok(native.connected)
end)

t.test("a ready game connects once; later ticks never reconnect or re-check", function()
  local app, native = F.app({ game = game(function() return true end), files = REGISTERED })
  run(app, native, App.READY_SETTLE_MS + 200)
  t.ok(native.connected)
  local calls = 0
  local old = native.connect
  native.connect = function(...) calls = calls + 1; return old(...) end
  native.clock = native.clock + 60000
  run(app, native, 5000)
  t.eq(calls, 0)
end)

t.run("ready")
