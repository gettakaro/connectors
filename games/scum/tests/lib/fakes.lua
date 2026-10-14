-- Fake native transport (what dlls/main.dll exposes) plus a fake Takaro on the other end of it.
-- The fake Takaro decodes every frame the connector sends, answers identify, and can send requests,
-- pings, error frames and drop the connection, so WIRE-1..6 run without a game or a network.
local json = require("takaro.json")

local F = {}

function F.native()
  local n = { inbox = {}, sent = {}, epoch = nil, nextEpoch = 0, outbox = 0, connected = nil, logs = {},
    closes = {}, identifiedEpochs = {}, clock = 1000, sendStatus = nil }
  function n.connect(url, ca) n.connected = { url = url, ca = ca }; return true end
  function n.disconnect() n.connected = nil end
  function n.send(text, kind, epoch)
    if n.sendStatus then return n.sendStatus, 0 end
    if not n.epoch then return "disconnected", 0 end
    if epoch ~= 0 and epoch ~= n.epoch then return "stale-epoch", 0 end
    local id = 0
    if kind == "event" then n.outbox = n.outbox + 1; id = n.outbox end
    n.sent[#n.sent + 1] = { text = text, kind = kind, epoch = n.epoch, outbox = id, msg = json.decode(text) }
    return "accepted", id
  end
  function n.poll()
    local it = table.remove(n.inbox, 1)
    if not it then return nil end
    return it.kind, it.epoch, it.text
  end
  function n.close(epoch, reason) n.closes[#n.closes + 1] = { epoch = epoch, reason = reason } end
  function n.identified(epoch) n.identifiedEpochs[#n.identifiedEpochs + 1] = epoch end
  function n.status() return "{}" end
  function n.log(line) n.logs[#n.logs + 1] = line end
  function n.now_ms() return n.clock end
  -- test helpers (the "network")
  function n.open()
    n.nextEpoch = n.nextEpoch + 1
    n.epoch = (7 << 32) | n.nextEpoch
    n.inbox[#n.inbox + 1] = { kind = "open", epoch = n.epoch, text = "" }
    return n.epoch
  end
  function n.deliver(msg)
    local text = type(msg) == "string" and msg or json.encode(msg)
    n.inbox[#n.inbox + 1] = { kind = "frame", epoch = n.epoch, text = text }
  end
  function n.drop(why)
    local e = n.epoch
    n.epoch = nil
    n.inbox[#n.inbox + 1] = { kind = "closed", epoch = e, text = why or "dropped" }
  end
  function n.confirm(watermark)
    n.inbox[#n.inbox + 1] = { kind = "confirmed", epoch = n.epoch, text = tostring(watermark) }
  end
  function n.frames(type_)
    local out = {}
    for _, s in ipairs(n.sent) do if s.msg.type == type_ then out[#out + 1] = s end end
    return out
  end
  return n
end

-- Fake Takaro driving an App through the fake native transport.
function F.takaro(app, native)
  local tk = { app = app, native = native, seq = 0 }
  function tk.pump(n)
    for _ = 1, (n or 3) do app:tick() end
  end
  function tk.connect(gameServerId)
    native.open()
    tk.pump()
    native.deliver({ type = "connected", payload = { clientId = "fake" } })
    native.deliver({ type = "identifyResponse", payload = { gameServerId = gameServerId or "gs-fake" } })
    tk.pump()
  end
  function tk.request(action, args, requestId)
    tk.seq = tk.seq + 1
    requestId = requestId or ("req-" .. tk.seq)
    local payload = { action = action }
    if args ~= nil then payload.args = args end
    native.deliver({ type = "request", requestId = requestId, payload = payload })
    return requestId
  end
  -- Raw frame text, for shapes json.encode cannot produce (explicit null).
  function tk.request_raw(text) native.deliver(text) end
  function tk.response(requestId)
    for _, s in ipairs(native.sent) do
      if s.msg.type == "response" and s.msg.requestId == requestId then return s.msg.payload, s end
    end
    return nil
  end
  function tk.responses(requestId)
    local out = {}
    for _, s in ipairs(native.sent) do
      if s.msg.type == "response" and s.msg.requestId == requestId then out[#out + 1] = s end
    end
    return out
  end
  return tk
end

-- A game adapter whose functions record their params and answer from a script.
function F.game(script)
  local g = { calls = {}, script = script or {}, pending = {} }
  local names = require("takaro.game").ACTION_FUNCTIONS
  for _, name in ipairs(names) do
    g[name] = function(self, params, done)
      self.calls[#self.calls + 1] = { name = name, params = params }
      local s = self.script[name]
      if s == "defer" then
        self.pending[#self.pending + 1] = { name = name, params = params, done = done }
        return nil
      end
      if type(s) == "function" then return s(params, done) end
      if s ~= nil then return s end
      return { ok = false, reason = "not_implemented" }
    end
  end
  g.start_events = function(self, events) self.events = events; return { ok = true } end
  return g
end

-- In-memory file system for config/identity/known-players files.
function F.fs(files)
  local fs = { files = files or {} }
  function fs.read(path) return fs.files[path] end
  function fs.write(path, text) fs.files[path] = text; return true end
  return fs
end

F.REG_TOKEN = "regtok-SECRET-0123456789abcdef"
F.ID_TOKEN = "idtok-SECRET-fedcba9876543210"

function F.config_text(extra)
  return table.concat({
    "TAKARO_WS_URL=wss://connect.example.test/",
    "REGISTRATION_TOKEN=" .. F.REG_TOKEN,
    "IDENTITY_TOKEN=" .. F.ID_TOKEN,
    "SERVER_NAME=SCUM test",
    extra or "",
  }, "\n")
end

-- A started App wired to fakes.
function F.app(opts)
  opts = opts or {}
  local App = require("takaro.app")
  local native = F.native()
  local fs = F.fs({ ["/mod/TakaroConfig.txt"] = opts.config or F.config_text(opts.extra) })
  local g = opts.game or F.game(opts.script)
  local app = App.new({ native = native, mod_dir = "/mod", fs = fs, game = g })
  app:start()
  return app, native, F.takaro(app, native), g, fs
end

return F
