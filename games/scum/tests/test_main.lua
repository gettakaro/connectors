-- main.lua under a fake UE4SS: the native functions and the game-thread loop are fakes; files are real (temp dir).
local t = require("lib.t")
local F = require("lib.fakes")

local MAIN = "mod/TakaroSCUM/Scripts/main.lua"

local function tempdir()
  local p = os.tmpname()
  os.remove(p)
  assert(os.execute("mkdir -p '" .. p .. "/Scripts'"))
  return p
end

local function install(native, dir)
  local loops = {}
  _G.takaro_connect, _G.takaro_disconnect, _G.takaro_send = native.connect, native.disconnect, native.send
  _G.takaro_poll, _G.takaro_close, _G.takaro_identified = native.poll, native.close, native.identified
  _G.takaro_status, _G.takaro_log, _G.takaro_now_ms = native.status, native.log, native.now_ms
  _G.takaro_mod_dir = function() return dir end
  _G.LoopInGameThreadWithDelay = function(ms, fn) loops[#loops + 1] = { ms = ms, fn = fn, gt = true } end
  _G.LoopAsync = function() error("LoopAsync must not be used: it races the game-thread hooks") end
  _G.ExecuteInGameThread = function(fn) fn() end
  _G.RegisterHook = function() end
  _G.FindAllOf = function() return nil end
  _G.FindFirstOf = function() return nil end
  _G.StaticFindObject = function() return nil end
  _G.takaro_list_dir = function() return "" end
  _G.takaro_now_us = function() return 0 end
  return loops
end

t.test("main.lua starts the app, connects, identifies and answers through the UE4SS loop", function()
  local dir = tempdir()
  local f = assert(io.open(dir .. "/TakaroConfig.txt", "wb"))
  f:write(F.config_text())
  f:close()
  local native = F.native()
  local loops = install(native, dir)
  dofile(MAIN)
  t.eq(#loops, 1, "one game-thread loop")
  t.ok(loops[1].gt and loops[1].ms == 100, "LoopInGameThreadWithDelay(100)")
  t.eq(native.connected.url, "wss://connect.example.test/")
  native.open()
  loops[1].fn()
  native.deliver({ type = "identifyResponse", payload = { gameServerId = "gs-main" } })
  native.deliver({ type = "request", requestId = "r1", payload = { action = "testReachability", args = "{}" } })
  loops[1].fn()
  t.eq(#native.frames("identify"), 1)
  t.ok(TAKARO_SCUM_APP.client.identified)
  t.deep(native.frames("response")[1].msg.payload,
    { connectable = false, reason = "SCUM world is not loaded (no ConZGameMode)" })
  local logs = table.concat(native.logs, "\n")
  t.ok(logs:find("TakaroSCUM 0.1.0 starting", 1, true))
  t.eq(logs:find(F.REG_TOKEN, 1, true), nil)
  os.execute("rm -rf '" .. dir .. "'")
end)

t.test("main.lua stays idle (no error) when dlls/main.dll did not load", function()
  local native = F.native()
  local loops = install(native, tempdir())
  _G.takaro_connect = nil
  local printed = {}
  local old = print
  _G.print = function(s) printed[#printed + 1] = s end
  dofile(MAIN)
  _G.print = old
  t.eq(#loops, 0)
  t.ok(printed[1]:find("not loaded", 1, true))
end)

t.run("main")
