-- Takaro Generic connector protocol (version 0) over the native transport. No game symbols here.
--
-- Transport notices (from takaro_poll): open(epoch) | frame(epoch, text) | closed(epoch, why) | error(epoch, why)
-- | confirmed(epoch, watermark). Every connection is an epoch; a response is only ever sent on the epoch its
-- request came from. Events are kept until a pong confirms them ("confirmed" watermark >= their outbox id) and
-- are sent again after a reconnect, so an outage cannot silently lose them (at-least-once).
local json = require("takaro.json")
local util = require("takaro.util")

local M = {}

M.EVENT_QUEUE_LIMIT = 1000
M.DEFAULT_TIMEOUT_MS = 8500 -- Takaro gives a Generic connector 10 s per request
M.REACHABILITY_TIMEOUT_MS = 4000 -- and 5 s for testReachability

local Client = {}
Client.__index = Client

-- opts: transport, log, now(), identify() -> payload table, on_request(action, args, reply, info),
--       timeout_payload(action, why, rawArgs) -> payload, log_frames (bool)
function M.new(opts)
  local c = setmetatable({}, Client)
  c.transport = opts.transport
  c.log = opts.log
  c.now = opts.now
  c.identify_payload = opts.identify
  c.on_request = opts.on_request
  c.timeout_payload = opts.timeout_payload or function() return { success = false, errorMessage = "timeout" } end
  c.log_frames = opts.log_frames or false
  c.epoch = nil -- open connection
  c.identified = false
  c.gameServerId = nil
  c.pending = {} -- requestId -> { epoch, deadline, action }
  c.events = {} -- not yet sent on the current epoch: { type, data, text }
  c.inflight = {} -- sent, waiting for confirmation: { ev, outbox, epoch }
  c.stats = { requests = 0, responses = 0, staleResponses = 0, timeouts = 0, unknownFrames = 0, errorFrames = 0,
    identifyErrors = 0, eventsSent = 0, eventsConfirmed = 0, eventsDropped = 0, badFrames = 0, opens = 0, closes = 0 }
  return c
end

function Client:frame_log(dir, text)
  if self.log_frames then self.log:info("%s %s", dir, util.truncate(text, 2000)) end
end

function Client:send_raw(tbl, kind, epoch)
  local ok, text = pcall(json.encode, tbl)
  if not ok then
    self.log:error("cannot encode outbound %s frame: %s", tostring(tbl.type), tostring(text))
    return "encode-error", 0
  end
  local status, outbox = self.transport.send(text, kind, epoch or self.epoch or 0)
  self:frame_log("out(" .. tostring(status) .. ")", text)
  return status, outbox or 0
end

-- One transport notice.
function Client:handle_notice(kind, epoch, text)
  if kind == "open" then
    self.epoch = epoch
    self.identified = false
    self.gameServerId = nil
    self.stats.opens = self.stats.opens + 1
    self.log:info("Takaro connection open (epoch %s); identifying", tostring(epoch))
    self:send_raw({ type = "identify", payload = self.identify_payload() }, "control", epoch)
  elseif kind == "frame" then
    if epoch ~= self.epoch then return end -- a late frame of an older connection
    self:frame_log("in", text)
    local ok, msg = pcall(json.decode, text)
    if not ok or type(msg) ~= "table" then
      self.stats.badFrames = self.stats.badFrames + 1
      self.log:warn("ignoring a frame that is not a JSON object (%d bytes)", #tostring(text))
      return
    end
    self:handle_message(msg, epoch)
  elseif kind == "closed" then
    if epoch == self.epoch then
      self.stats.closes = self.stats.closes + 1
      self.log:info("Takaro connection closed (epoch %s): %s", tostring(epoch), tostring(text))
      self.epoch = nil
      self.identified = false
      self:requeue_inflight()
    end
  elseif kind == "error" then
    self.log:warn("Takaro transport: %s", tostring(text))
  elseif kind == "confirmed" then
    if epoch == self.epoch then self:confirm(tonumber(text) or 0) end
  end
end

function Client:handle_message(msg, epoch)
  local t = msg.type
  if t == "identifyResponse" then
    local p = util.opt_table(msg.payload) or {}
    local err = msg.error or p.error
    if err ~= nil or not util.opt_string(p.gameServerId) then
      self.stats.identifyErrors = self.stats.identifyErrors + 1
      local why = type(err) == "table" and (util.opt_string(err.message) or util.opt_string(err.name)) or tostring(err)
      self.log:error("Takaro rejected identify: %s; check REGISTRATION_TOKEN in TakaroConfig.txt (retrying with backoff)",
        util.truncate(why or "no gameServerId", 300))
      self.transport.close(epoch, "identify rejected")
      return
    end
    self.identified = true
    self.gameServerId = p.gameServerId
    self.transport.identified(epoch)
    self.log:info("identified with Takaro as game server %s", self.gameServerId)
    self:flush_events()
  elseif t == "request" then
    self:handle_request(msg, epoch)
  elseif t == "ping" then
    self:send_raw({ type = "pong" }, "control", epoch)
  elseif t == "pong" or t == "connected" or t == "eventAck" or t == "capabilitiesResponse" then
    -- pong is consumed by the native heartbeat; the others carry nothing for protocol version 0
  elseif t == "error" then
    self.stats.errorFrames = self.stats.errorFrames + 1
    local p = msg.payload
    local why = type(p) == "table" and (util.opt_string(p.message) or json.encode(p)) or tostring(p)
    self.log:warn("Takaro error frame: %s", util.truncate(why, 500))
  else
    self.stats.unknownFrames = self.stats.unknownFrames + 1
    self.log:debug("ignoring frame type %s", tostring(t))
  end
end

function Client:handle_request(msg, epoch)
  local requestId = msg.requestId
  if requestId == nil then
    self.log:warn("request without requestId ignored")
    return
  end
  local payload = util.opt_table(msg.payload) or {}
  local action = util.opt_string(payload.action) or ""
  self.stats.requests = self.stats.requests + 1
  local timeout = action == "testReachability" and M.REACHABILITY_TIMEOUT_MS or M.DEFAULT_TIMEOUT_MS
  local entry = { epoch = epoch, deadline = self.now() + timeout, action = action, args = payload.args }
  self.pending[requestId] = entry
  local function reply(result)
    if self.pending[requestId] ~= entry then return false end -- answered already (or timed out)
    self.pending[requestId] = nil
    self:respond(requestId, epoch, action, result)
    return true
  end
  local ok, err = pcall(self.on_request, action, payload.args, reply, { requestId = requestId })
  if not ok then
    self.log:error("action %s raised: %s", action, tostring(err))
    reply(self.timeout_payload(action, "internal error: " .. tostring(err), payload.args))
  end
end

function Client:respond(requestId, epoch, action, result)
  if result == nil then result = json.null end
  local status = self:send_raw({ type = "response", requestId = requestId, payload = result }, "response", epoch)
  if status == "accepted" then
    self.stats.responses = self.stats.responses + 1
  else
    self.stats.staleResponses = self.stats.staleResponses + 1
    self.log:warn("response to %s (%s) not sent: %s", action, tostring(requestId), tostring(status))
  end
end

-- Queue a game event; it is sent once identified and kept until Takaro confirms it.
function Client:send_event(eventType, data)
  local ok, text = pcall(json.encode, { type = "gameEvent", payload = { type = eventType, data = data } })
  if not ok then
    self.log:error("cannot encode %s event: %s", eventType, tostring(text))
    return false
  end
  self.events[#self.events + 1] = { type = eventType, text = text }
  self:trim_events()
  self:flush_events()
  return true
end

function Client:trim_events()
  while #self.events + #self.inflight > M.EVENT_QUEUE_LIMIT and #self.events > 0 do
    table.remove(self.events, 1)
    self.stats.eventsDropped = self.stats.eventsDropped + 1
  end
end

function Client:flush_events()
  if not self.identified or not self.epoch then return end
  while #self.events > 0 do
    local ev = self.events[1]
    local status, outbox = self.transport.send(ev.text, "event", self.epoch)
    if status ~= "accepted" then
      if status == "too-large" then
        table.remove(self.events, 1)
        self.stats.eventsDropped = self.stats.eventsDropped + 1
        self.log:warn("dropping %s event: too large", ev.type)
      end
      return
    end
    self:frame_log("out(event)", ev.text)
    table.remove(self.events, 1)
    self.inflight[#self.inflight + 1] = { ev = ev, outbox = outbox or 0, epoch = self.epoch }
    self.stats.eventsSent = self.stats.eventsSent + 1
  end
end

function Client:confirm(watermark)
  local keep = {}
  for _, f in ipairs(self.inflight) do
    if f.epoch == self.epoch and f.outbox > 0 and f.outbox <= watermark then
      self.stats.eventsConfirmed = self.stats.eventsConfirmed + 1
    else
      keep[#keep + 1] = f
    end
  end
  self.inflight = keep
end

-- The connection ended: unconfirmed events go back to the front of the queue, in order.
function Client:requeue_inflight()
  if #self.inflight == 0 then return end
  local q = {}
  for _, f in ipairs(self.inflight) do q[#q + 1] = f.ev end
  for _, ev in ipairs(self.events) do q[#q + 1] = ev end
  self.events = q
  self.inflight = {}
  self:trim_events()
end

-- Answers requests whose handler did not reply in time (Takaro would otherwise wait its full budget).
function Client:tick()
  local now = self.now()
  for requestId, entry in pairs(self.pending) do
    if now >= entry.deadline then
      self.pending[requestId] = nil
      self.stats.timeouts = self.stats.timeouts + 1
      self.log:warn("action %s (%s) did not finish in time; answering with its fallback", entry.action,
        tostring(requestId))
      self:respond(requestId, entry.epoch, entry.action, self.timeout_payload(entry.action, "timeout", entry.args))
    end
  end
  self:flush_events()
end

function Client:status()
  local s = util.shallow_copy(self.stats)
  s.epoch = self.epoch
  s.identified = self.identified
  s.gameServerId = self.gameServerId
  s.queuedEvents = #self.events
  s.inflightEvents = #self.inflight
  local n = 0
  for _ in pairs(self.pending) do n = n + 1 end
  s.pendingRequests = n
  return s
end

return M
