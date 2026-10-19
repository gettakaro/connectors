-- Takaro Generic connector protocol (versions 0 and 1) over the native transport. No game symbols here.
--
-- Transport notices (from takaro_poll): open(epoch) | frame(epoch, text) | closed(epoch, why) | error(epoch, why)
-- | confirmed(epoch, watermark). Every connection is an epoch; a response is only ever sent on the epoch its
-- request came from. Events are kept until Takaro confirms them and are sent again after a reconnect, so an outage
-- cannot silently lose them (at-least-once). Version 0 confirms through a pong ("confirmed" watermark >= the
-- event's outbox id); version 1 through eventAck on this process's event stream (streamId, seq 1, 2, 3, ...).
--
-- Version 1 is offered as protocolVersions [0, 1] with the Capability Manifest in identify; a Takaro that answers
-- no protocolVersion keeps version 0. Version 1 answers errors as { code, message } (v1.lua translates the
-- version 0 handler results) and replaces the manifest with a capabilities frame when it changes.
local json = require("takaro.json")
local util = require("takaro.util")
local v1 = require("takaro.v1")

local M = {}

M.EVENT_QUEUE_LIMIT = 1000
M.DEFAULT_TIMEOUT_MS = 8500 -- Takaro gives a Generic connector 10 s per request
M.REACHABILITY_TIMEOUT_MS = 4000 -- and 5 s for testReachability
M.ACK_TIMEOUT_MS = 30000 -- version 1: no eventAck for this long -> reconnect, which resends the unacknowledged events

-- One stream id per process, as protocol version 1 asks (^[A-Za-z0-9_-]{1,128}$).
function M.new_stream_id(seed)
  math.randomseed(seed or os.time())
  local hex = {}
  for i = 1, 24 do hex[i] = string.format("%x", math.random(0, 15)) end
  return "scum-" .. table.concat(hex)
end

local Client = {}
Client.__index = Client

-- opts: transport, log, now(), identify() -> payload table, on_request(action, args, reply, info { requestId,
--       version }), timeout_payload(action, why, rawArgs) -> payload, log_frames (bool),
--       offer_v1 (bool), manifest() -> Capability Manifest array (version 1), stream_id (optional)
function M.new(opts)
  local c = setmetatable({}, Client)
  c.transport = opts.transport
  c.log = opts.log
  c.now = opts.now
  c.identify_payload = opts.identify
  c.on_request = opts.on_request
  c.timeout_payload = opts.timeout_payload or function() return { success = false, errorMessage = "timeout" } end
  c.log_frames = opts.log_frames or false
  c.offer_v1 = opts.offer_v1 or false
  c.manifest = opts.manifest or function() return {} end
  c.streamId = opts.stream_id or M.new_stream_id(c.now())
  c.nextSeq = 1
  c.version = 0 -- the protocol version Takaro chose for the current connection
  c.sentManifest = nil -- JSON of the manifest Takaro holds for this connection (version 1)
  c.manifestRequests = 0
  c.epoch = nil -- open connection
  c.identified = false
  c.gameServerId = nil
  c.pending = {} -- requestId -> { epoch, deadline, action }
  c.events = {} -- not yet sent on the current epoch: { type, data, text }
  c.inflight = {} -- sent, waiting for confirmation: { ev, outbox, epoch }
  c.stats = { requests = 0, responses = 0, staleResponses = 0, timeouts = 0, unknownFrames = 0, errorFrames = 0,
    identifyErrors = 0, eventsSent = 0, eventsConfirmed = 0, eventsDropped = 0, badFrames = 0, opens = 0, closes = 0,
    acks = 0, eventErrors = 0, manifestsSent = 0, manifestErrors = 0, ackTimeouts = 0 }
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
    self.version = 0
    self.sentManifest = nil
    self.stats.opens = self.stats.opens + 1
    local payload = self.identify_payload()
    local offered = nil
    if self.offer_v1 then
      local ok, m = pcall(self.manifest)
      if ok and type(m) == "table" then
        payload.protocolVersions = { 0, 1 }
        payload.capabilities = m
        offered = json.encode(m)
      else
        self.log:error("cannot build the capability manifest (%s); offering protocol 0 only", tostring(m))
      end
    end
    self.offeredManifest = offered
    self.log:info("Takaro connection open (epoch %s); identifying (%s)", tostring(epoch),
      offered and "protocol versions [0, 1]" or "protocol 0")
    self:send_raw({ type = "identify", payload = payload }, "control", epoch)
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
      self.version = 0
      self:requeue_inflight()
    end
  elseif kind == "error" then
    self.log:warn("Takaro transport: %s", tostring(text))
  elseif kind == "confirmed" then
    -- version 0 only: a pong says Takaro read the frame; version 1 waits for eventAck
    if epoch == self.epoch and self.version == 0 then self:confirm(tonumber(text) or 0) end
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
      local code = type(err) == "table" and util.opt_string(err.code) or nil
      local hint = "check REGISTRATION_TOKEN in TakaroConfig.txt"
      if code == "no_shared_protocol_version" then hint = "set TAKARO_PROTOCOL=0 in TakaroConfig.txt" end
      self.log:error("Takaro rejected identify: %s%s; %s (retrying with backoff)", code and (code .. ": ") or "",
        util.truncate(why or "no gameServerId", 300), hint)
      self.transport.close(epoch, "identify rejected")
      return
    end
    self.identified = true
    self.gameServerId = p.gameServerId
    -- an older Takaro sends no protocolVersion: that is version 0
    self.version = (self.offeredManifest and p.protocolVersion == 1) and 1 or 0
    self.sentManifest = self.version == 1 and self.offeredManifest or nil
    self.transport.identified(epoch)
    self.log:info("identified with Takaro as game server %s, protocol %d", self.gameServerId, self.version)
    self:update_manifest() -- the manifest may have changed between identify and its answer
    self:flush_events()
  elseif t == "request" then
    self:handle_request(msg, epoch)
  elseif t == "ping" then
    self:send_raw({ type = "pong" }, "control", epoch)
  elseif t == "eventAck" then
    self:handle_ack(msg)
  elseif t == "eventError" then
    -- Takaro acknowledges a rejected event too (it is never resent); keep the reason visible.
    self.stats.eventErrors = self.stats.eventErrors + 1
    local p = util.opt_table(msg.payload) or {}
    local e = util.opt_table(msg.error) or {}
    self.log:warn("Takaro rejected event seq %s: %s: %s", tostring(p.seq), tostring(e.code),
      util.truncate(tostring(e.message), 300))
  elseif t == "capabilitiesResponse" then
    local e = util.opt_table(msg.error)
    if e then
      self.stats.manifestErrors = self.stats.manifestErrors + 1
      self.log:error("Takaro refused the capability manifest: %s: %s", tostring(e.code), util.truncate(tostring(e.message), 300))
    else
      self.log:info("Takaro stored the capability manifest")
    end
  elseif t == "pong" or t == "connected" then
    -- pong is consumed by the native heartbeat
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
  local entry = { epoch = epoch, deadline = self.now() + timeout, action = action, args = payload.args,
    version = self.version }
  self.pending[requestId] = entry
  local function reply(result)
    if self.pending[requestId] ~= entry then return false end -- answered already (or timed out)
    self.pending[requestId] = nil
    self:respond(requestId, epoch, action, result, entry.version)
    return true
  end
  local ok, err = pcall(self.on_request, action, payload.args, reply,
    { requestId = requestId, version = self.version })
  if not ok then
    self.log:error("action %s raised: %s", action, tostring(err))
    reply(self.timeout_payload(action, "internal error: " .. tostring(err), payload.args))
  end
end

function Client:respond(requestId, epoch, action, result, version)
  local frame
  if version == 1 then
    local ok, payload, err = pcall(v1.translate, action, result)
    if not ok then payload, err = nil, { code = "internal", message = "response translation failed: " .. tostring(payload) } end
    if err then
      frame = { type = "response", requestId = requestId, error = err }
    else
      if payload == nil then payload = json.null end
      frame = { type = "response", requestId = requestId, payload = payload }
    end
  else
    if result == nil then result = json.null end
    frame = { type = "response", requestId = requestId, payload = result }
  end
  local status = self:send_raw(frame, "response", epoch)
  if status == "accepted" then
    self.stats.responses = self.stats.responses + 1
  else
    self.stats.staleResponses = self.stats.staleResponses + 1
    self.log:warn("response to %s (%s) not sent: %s", action, tostring(requestId), tostring(status))
  end
end

-- Queue a game event; it is sent once identified and kept until Takaro confirms it. Every event gets the next seq
-- of this process's stream at once, so a resend after a reconnect carries the same seq.
function Client:send_event(eventType, data)
  local ok, text = pcall(json.encode, { type = "gameEvent", payload = { type = eventType, data = data } })
  if not ok then
    self.log:error("cannot encode %s event: %s", eventType, tostring(text))
    return false
  end
  local seq = self.nextSeq
  self.nextSeq = seq + 1
  self.events[#self.events + 1] = { type = eventType, text = text, data = data, seq = seq }
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
    local text = ev.text
    if self.version == 1 then
      text = json.encode({ type = "gameEvent",
        payload = { type = ev.type, data = ev.data, streamId = self.streamId, seq = ev.seq } })
    end
    local status, outbox = self.transport.send(text, "event", self.epoch)
    if status ~= "accepted" then
      if status == "too-large" then
        table.remove(self.events, 1)
        self.stats.eventsDropped = self.stats.eventsDropped + 1
        self.log:warn("dropping %s event: too large", ev.type)
      end
      return
    end
    self:frame_log("out(event)", text)
    table.remove(self.events, 1)
    self.inflight[#self.inflight + 1] = { ev = ev, outbox = outbox or 0, epoch = self.epoch, sentAt = self.now() }
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

-- Version 1 eventAck { streamId, upTo }: cumulative; drops every sent event of this stream with seq <= upTo.
function Client:handle_ack(msg)
  if self.version ~= 1 then return end
  local p = util.opt_table(msg.payload) or {}
  local upTo = util.opt_number(p.upTo)
  if p.streamId ~= self.streamId or not upTo then return end
  self.stats.acks = self.stats.acks + 1
  local keep = {}
  for _, f in ipairs(self.inflight) do
    if f.ev.seq <= upTo then
      self.stats.eventsConfirmed = self.stats.eventsConfirmed + 1
    else
      keep[#keep + 1] = f
    end
  end
  self.inflight = keep
end

-- Version 1: send the manifest again when it changed (e.g. the map bounds were read from the world).
function Client:update_manifest()
  if self.version ~= 1 or not self.identified or not self.epoch then return end
  local ok, m = pcall(self.manifest)
  if not ok or type(m) ~= "table" then return end
  local text = json.encode(m)
  if text == self.sentManifest then return end
  self.manifestRequests = self.manifestRequests + 1
  local status = self:send_raw({ type = "capabilities", requestId = "scum-capabilities-" .. self.manifestRequests,
    payload = { capabilities = m } }, "control", self.epoch)
  if status == "accepted" then
    self.sentManifest = text
    self.stats.manifestsSent = self.stats.manifestsSent + 1
    self.log:info("capability manifest changed; sent the replacement")
  end
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
      self:respond(requestId, entry.epoch, entry.action, self.timeout_payload(entry.action, "timeout", entry.args),
        entry.version)
    end
  end
  if self.version == 1 and self.epoch and self.ackClosedEpoch ~= self.epoch and #self.inflight > 0
    and now - self.inflight[1].sentAt >= M.ACK_TIMEOUT_MS then
    self.ackClosedEpoch = self.epoch
    self.stats.ackTimeouts = self.stats.ackTimeouts + 1
    self.log:warn("no eventAck from Takaro for %d s; reconnecting to resend %d event(s)", M.ACK_TIMEOUT_MS // 1000,
      #self.inflight)
    self.transport.close(self.epoch, "event ack timeout")
    return
  end
  self:flush_events()
end

function Client:status()
  local s = util.shallow_copy(self.stats)
  s.epoch = self.epoch
  s.identified = self.identified
  s.protocolVersion = self.version
  s.streamId = self.streamId
  s.gameServerId = self.gameServerId
  s.queuedEvents = #self.events
  s.inflightEvents = #self.inflight
  local n = 0
  for _ in pairs(self.pending) do n = n + 1 end
  s.pendingRequests = n
  return s
end

return M
