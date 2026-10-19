-- Wires config, logger, protocol client, action router, event adapter and game adapter together and drives
-- them from one tick (UE4SS LoopAsync). Native transport functions are injected, so tests run it unchanged.
local json = require("takaro.json")
local util = require("takaro.util")
local config = require("takaro.config")
local logmod = require("takaro.log")
local players = require("takaro.players")
local protocol = require("takaro.protocol")
local actions = require("takaro.actions")
local events = require("takaro.events")
local game = require("takaro.game")
local map = require("takaro.map")
local v1 = require("takaro.v1")
local profile = require("takaro.profile")

local M = {}

M.VERSION = "0.1.0" -- x-release-please-version
M.MAX_NOTICES_PER_TICK = 500
M.STATUS_EVERY_MS = 60000
M.SAVE_KNOWN_EVERY_MS = 30000
M.IDENTITY_FILE = "TakaroIdentity.txt"
M.KNOWN_FILE = "known-players.json"
M.WORLD_BOUNDS_RETRY_MS = 10000
M.GAME_VERSION_RETRY_MS = 10000

local App = {}
App.__index = App

M.default_fs = {
  read = function(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local s = f:read("a")
    f:close()
    return s
  end,
  write = function(path, text)
    local f = io.open(path, "wb")
    if not f then return false end
    f:write(text)
    f:close()
    return true
  end,
}

-- opts: native = { connect, disconnect, send, poll, close, identified, status, log, now_ms }, mod_dir,
--       env (UE4SS globals for the game adapter), fs, game (optional adapter override for tests)
function M.new(opts)
  local a = setmetatable({}, App)
  a.native = opts.native
  a.dir = opts.mod_dir or "."
  a.env = opts.env
  a.fs = opts.fs or M.default_fs
  a.game_override = opts.game
  a.timers = {}
  a.started = false
  return a
end

function App:now()
  return self.native.now_ms and self.native.now_ms() or math.floor(os.time() * 1000)
end

function App:schedule(delayMs, fn)
  self.timers[#self.timers + 1] = { at = self:now() + delayMs, fn = fn }
end

function App:identity_token()
  if self.cfg.identityToken ~= "" then return self.cfg.identityToken end
  local path = self.dir .. "/" .. M.IDENTITY_FILE
  local existing = self.fs.read(path)
  existing = existing and util.trim(existing) or ""
  if existing ~= "" then return existing end
  math.randomseed(os.time() + (self:now() % 100000))
  local hex = {}
  for i = 1, 32 do hex[i] = string.format("%x", math.random(0, 15)) end
  local token = "scum-" .. table.concat(hex)
  if not self.fs.write(path, token .. "\n") then
    self.log:warn("cannot write %s; the identity token changes on every start", M.IDENTITY_FILE)
  end
  return token
end

function App:start()
  if self.started then return end
  self.started = true
  self.log = logmod.new(self.native.log)
  self.cfg = config.load(self.dir, self.fs)
  self.log:add_secret(self.cfg.registrationToken)
  self.log:add_secret(self.cfg.identityToken)
  self.log:info("TakaroSCUM %s starting; config %s", M.VERSION, config.describe(self.cfg))
  if self.cfg.created then self.log:info("created %s; fill in REGISTRATION_TOKEN and restart", config.FILE_NAME) end
  if #self.cfg.unknownKeys > 0 then
    self.log:warn("unknown keys in %s: %s", config.FILE_NAME, table.concat(self.cfg.unknownKeys, ","))
  end
  self.identityToken = self:identity_token()
  self.log:add_secret(self.identityToken)

  self.known = players.new_store()
  local saved = self.fs.read(self.dir .. "/" .. M.KNOWN_FILE)
  if saved then
    local ok, list = pcall(json.decode, saved)
    if ok then self.known:load_list(list) end
  end
  self.lastKnownSave = self:now()
  self.lastStatus = self:now()
  self.mapBounds = nil -- map frame from the live world bounds; the SCUM 1.3.4.2 frame until it is read
  self.nextBoundsTry = 0
  self.gameVersion = nil -- the running SCUM build, read from the engine once it is available
  self.nextVersionTry = 0
  local icon, why = profile.load_icon(self.fs, self.dir)
  self.icon = icon
  if icon then
    self.log:info("game icon %s: %d bytes, revision %s", profile.ICON_FILE, icon.size, icon.revision)
  else
    self.log:warn("no game icon declared: %s", tostring(why))
  end

  self.game = self.game_override or game.new(self.env, self.log, { mod_dir = self.dir })
  self.router = actions.new({
    game = self.game, known = self.known, config = self.cfg, log = self.log,
    schedule = function(ms, fn) self:schedule(ms, fn) end,
  })
  self.client = protocol.new({
    transport = self.native,
    log = self.log,
    now = function() return self:now() end,
    log_frames = self.cfg.logFrames,
    identify = function()
      local gameSlug, connector = profile.identity(M.VERSION)
      return { identityToken = self.identityToken, registrationToken = self.cfg.registrationToken,
        name = self.cfg.serverName, game = gameSlug, connector = connector }
    end,
    offer_v1 = self.cfg.protocol >= 1,
    manifest = function() return v1.manifest(self.mapBounds, profile.capability(self.gameVersion, self.icon)) end,
    on_request = function(action, args, reply, info)
      -- getGameIcon is a protocol 1 action (Takaro only asks when game.profile declares an icon)
      if action == "getGameIcon" and info and info.version == 1 then return reply(profile.icon_response(self.icon)) end
      self.router:handle(action, args, reply)
    end,
    timeout_payload = function(action, why, rawArgs) return self.router:fallback(action, why, rawArgs) end,
  })
  self.events = events.new({ client = self.client, known = self.known, log = self.log,
    now = function() return self:now() end })
  self:read_game_version(self:now())

  local ok, res = pcall(self.game.start_events, self.game, self.events)
  if not ok then
    self.log:error("game event hooks failed: %s", tostring(res))
  elseif type(res) == "table" and res.ok == false then
    self.log:warn("game event hooks not installed: %s", tostring(res.reason))
  end

  if self.cfg.registrationToken == "" then
    self.log:warn("REGISTRATION_TOKEN is empty in %s; not connecting to Takaro", config.FILE_NAME)
    return
  end
  if not self.native.connect(self.cfg.url, self.cfg.caFile) then
    self.log:error("native transport refused the Takaro URL (must be wss://host[:port]/path)")
  end
end

function App:tick()
  for _ = 1, M.MAX_NOTICES_PER_TICK do
    local kind, epoch, text = self.native.poll()
    if not kind then break end
    self.client:handle_notice(kind, epoch, text)
  end
  self.client:tick()
  local now = self:now()
  if self.game.tick then
    local okg, errg = pcall(self.game.tick, self.game, now)
    if not okg then self.log:error("game tick failed: %s", tostring(errg)) end
  end
  self:read_map_bounds(now)
  self:read_game_version(now)
  if #self.timers > 0 then
    local due, keep = {}, {}
    for _, t in ipairs(self.timers) do
      if now >= t.at then due[#due + 1] = t else keep[#keep + 1] = t end
    end
    self.timers = keep
    for _, t in ipairs(due) do
      local ok, err = pcall(t.fn)
      if not ok then self.log:error("timer failed: %s", tostring(err)) end
    end
  end
  if self.known.dirty and now - self.lastKnownSave >= M.SAVE_KNOWN_EVERY_MS then
    self.lastKnownSave = now
    local ok, text = pcall(json.encode, self.known:to_list())
    if ok and self.fs.write(self.dir .. "/" .. M.KNOWN_FILE, text) then self.known.dirty = false end
  end
  if now - self.lastStatus >= M.STATUS_EVERY_MS then
    self.lastStatus = now
    self:log_status()
  end
end

-- Reads the server's world bounds once the world is loaded (retried) and re-declares the map when they differ from
-- the built-in frame.
function App:read_map_bounds(now)
  if self.mapBounds or now < self.nextBoundsTry or not self.game.world_bounds then return end
  self.nextBoundsTry = now + M.WORLD_BOUNDS_RETRY_MS
  local ok, res = pcall(self.game.world_bounds, self.game)
  if not (ok and type(res) == "table" and res.ok and map.valid_bounds(res.value)) then return end
  self.mapBounds = map.map_bounds(res.value)
  local b = self.mapBounds
  self.log:info("map bounds from the world: x %.0f..%.0f y %.0f..%.0f", b.minX, b.maxX, b.minY, b.maxY)
  self.client:update_manifest()
end

-- Reads the running game build (retried until the engine has it) and re-declares game.profile with it.
function App:read_game_version(now)
  if self.gameVersion or now < self.nextVersionTry or not self.game.game_version then return end
  self.nextVersionTry = now + M.GAME_VERSION_RETRY_MS
  local ok, res = pcall(self.game.game_version, self.game)
  local v = ok and type(res) == "table" and res.ok and profile.clean_version(res.value) or nil
  if not v then return end
  self.gameVersion = v
  self.log:info("game build %s", v)
  self.client:update_manifest()
end

-- tick() that never lets an error escape into UE4SS's loop
function App:tick_safe()
  local ok, err = pcall(self.tick, self)
  if not ok and self.log then self.log:error("tick failed: %s", tostring(err)) end
end

function App:log_status()
  local s = self.client:status()
  self.log:info("status identified=%s protocol=%d gameServerId=%s requests=%d responses=%d timeouts=%d events sent=%d "
    .. "confirmed=%d queued=%d inflight=%d dropped=%d errorFrames=%d acks=%d eventErrors=%d", tostring(s.identified),
    s.protocolVersion, tostring(s.gameServerId), s.requests, s.responses, s.timeouts, s.eventsSent, s.eventsConfirmed, s.queuedEvents,
    s.inflightEvents, s.eventsDropped, s.errorFrames, s.acks, s.eventErrors)
  if self.game.perf_summary then self.log:info("perf %s", self.game:perf_summary()) end
end

function App:stop()
  if self.native.disconnect then self.native.disconnect() end
end

return M
