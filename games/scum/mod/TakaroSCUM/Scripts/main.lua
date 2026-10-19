-- TakaroSCUM: UE4SS Lua mod entry. dlls/main.dll (same mod folder) registers the takaro_* transport
-- functions before this script runs. See takaro/app.lua for the wiring.
local function mod_dir()
  if type(takaro_mod_dir) == "function" then
    local d = takaro_mod_dir()
    if type(d) == "string" and d ~= "" then return (d:gsub("\\", "/")) end
  end
  local src = (debug.getinfo(1, "S").source or ""):gsub("^@", ""):gsub("\\", "/")
  return src:match("^(.*)/[Ss]cripts/[^/]*$") or "."
end

local dir = mod_dir()
package.path = dir .. "/Scripts/?.lua;" .. package.path

if type(takaro_connect) ~= "function" then
  print("[TakaroSCUM] native transport (dlls/main.dll) is not loaded; the connector stays idle. "
    .. "Check that UE4SS loads C++ mods and that dlls/main.dll is present.\n")
  return
end

local App = require("takaro.app")
local app = App.new({
  mod_dir = dir,
  env = {
    ExecuteInGameThread = ExecuteInGameThread, RegisterHook = RegisterHook, FindAllOf = FindAllOf,
    FindFirstOf = FindFirstOf, StaticFindObject = StaticFindObject, NotifyOnNewObject = NotifyOnNewObject,
    LoopAsync = LoopAsync, ExecuteWithDelay = ExecuteWithDelay, FName = FName,
    list_dir = takaro_list_dir, now_us = takaro_now_us, now_ms = takaro_now_ms,
  },
  native = {
    connect = takaro_connect, disconnect = takaro_disconnect, send = takaro_send, poll = takaro_poll,
    close = takaro_close, identified = takaro_identified, status = takaro_status, log = takaro_log,
    now_ms = takaro_now_ms,
  },
})
TAKARO_SCUM_APP = app -- for the UE4SS console and tests

local TICK_MS = 100
local ok, err = pcall(app.start, app)
if not ok then
  takaro_log("lua: [error] TakaroSCUM failed to start: " .. tostring(err))
  print("[TakaroSCUM] failed to start: " .. tostring(err) .. "\n")
  return
end

-- All Lua of this mod runs on the game thread. In the pinned UE4SS (83330635) LoopAsync callbacks run on the
-- async thread WITHOUT LuaMod::m_thread_actions_mutex, while RegisterHook/ExecuteInGameThread callbacks run on
-- the game thread; all share one Lua global state, so mixing them is a data race. The tick is cheap (it polls
-- the native inbox, which the WinHTTP threads fill), so a 100 ms game-thread loop costs microseconds when idle.
local function start_loop()
  if type(LoopInGameThreadWithDelay) == "function" then
    LoopInGameThreadWithDelay(TICK_MS, function() app:tick_safe() end)
  else
    takaro_log("lua: [warn] LoopInGameThreadWithDelay is missing; falling back to LoopAsync + ExecuteInGameThread")
    LoopAsync(TICK_MS, function()
      ExecuteInGameThread(function() app:tick_safe() end)
      return false
    end)
  end
end
start_loop()
