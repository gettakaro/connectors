-- SCUM gameplay logs (SCUM/Saved/SaveFiles/Logs/<category>_<YYYYMMDDHHMMSS>.log, UTF-16LE): decoder, line
-- parsers and a rotation-aware tailer. No game or transport symbols; the file system is injected:
--   fs.list(dir) -> { "name", ... }   fs.size(path) -> bytes|nil   fs.read_at(path, offset, n) -> bytes|nil
local M = {}

-- UTF-16LE bytes -> UTF-8. Returns text, leftover (an odd trailing byte or a dangling high surrogate).
function M.utf16le_to_utf8(bytes)
  local out, n, i, len = {}, 0, 1, #bytes
  while i + 1 <= len do
    local u = bytes:byte(i) + bytes:byte(i + 1) * 256
    local adv = 2
    if u >= 0xD800 and u <= 0xDBFF then
      if i + 3 > len then break end
      local lo = bytes:byte(i + 2) + bytes:byte(i + 3) * 256
      if lo >= 0xDC00 and lo <= 0xDFFF then
        u = 0x10000 + (u - 0xD800) * 0x400 + (lo - 0xDC00)
        adv = 4
      else
        u = 0xFFFD
      end
    elseif u >= 0xDC00 and u <= 0xDFFF then
      u = 0xFFFD
    end
    if u ~= 0xFEFF then
      n = n + 1
      out[n] = utf8.char(u)
    end
    i = i + adv
  end
  return table.concat(out), bytes:sub(i)
end

local function num(s) return tonumber(s) end

local function strip_ts(line)
  local ts, rest = line:match("^(%d%d%d%d%.%d%d%.%d%d%-%d%d%.%d%d%.%d%d):%s*(.*)$")
  return ts, rest or line
end
M.strip_ts = strip_ts

-- '1.2.3.4 76561198000000001:Name(12)' logged in at: X=1 Y=2 Z=3
function M.parse_login(line)
  local _, rest = strip_ts(line)
  local ip, id, name, prof, verb, tail = rest:match("^'(%S+)%s+(%d+):(.-)%((%d+)%)'%s+logged%s+(%a+)(.*)$")
  if not ip then return nil end
  local e = { kind = verb == "in" and "login" or (verb == "out" and "logout" or nil), ip = ip, steamId = id,
    name = name, profileId = prof }
  if not e.kind then return nil end
  local x, y, z = tail:match("X=([%-%d%.]+)%s+Y=([%-%d%.]+)%s+Z=([%-%d%.]+)")
  if x then e.position = { x = num(x), y = num(y), z = num(z) } end
  e.drone = tail:find("(as drone)", 1, true) ~= nil
  return e
end

-- '76561198000000001:Name(12)' 'Global: hello'
-- Same mapping as game.CHAT_TO_TAKARO: Local is proximity chat -> "friends" (Takaro has no local channel).
M.CHAT_CHANNELS = { global = "global", squad = "team", local_ = "friends", admin = "global" }
function M.parse_chat(line)
  local _, rest = strip_ts(line)
  local id, name, prof, chan, msg = rest:match("^'(%d+):(.-)%((%d+)%)'%s+'(%a+):%s?(.*)'%s*$")
  if not id then return nil end
  local key = chan:lower()
  if key == "local" then key = "local_" end
  return { steamId = id, name = name, profileId = prof, scumChannel = chan,
    channel = M.CHAT_CHANNELS[key] or "global", msg = msg }
end

-- '76561198000000001:Name(12)' Command: 'SpawnItem Weapon_M9 1'
function M.parse_admin(line)
  local _, rest = strip_ts(line)
  local id, name, cmd = rest:match("^'(%d+):(.-)%(%d+%)'%s+Command:%s+'(.*)'%s*$")
  if not id then return nil end
  return { steamId = id, name = name, command = cmd }
end

local function loc3(s)
  if not s then return nil end
  local x, y, z = s:match("([%-%d%.]+),%s*([%-%d%.]+),%s*([%-%d%.]+)")
  if not x then return nil end
  return { x = num(x), y = num(y), z = num(z) }
end

-- Died: Victim (7656...), Killer: Killer (7656...) Weapon: Weapon_M9_C [Projectile] S:[KillerLoc : x, y, z VictimLoc: x, y, z, Distance: 2.96 m]
-- Killer may be an NPC: "Killer: BP_Drifter_Lvl_4_C_2146455350 (NPC)".
function M.parse_kill(line)
  local _, rest = strip_ts(line)
  local victim, vid, killer, kid, tail = rest:match("^Died:%s+(.-)%s+%((%d+)%),%s+Killer:%s+(.-)%s+%(([%w]+)%)%s*(.*)$")
  if not victim then return nil end
  local weapon, dmg = tail:match("Weapon:%s+(%S+)%s*%[([^%]]*)%]")
  if not weapon then weapon = tail:match("Weapon:%s+(%S+)") end
  local e = { victimName = victim, victimId = vid, killerName = killer, weapon = weapon, damageType = dmg }
  if kid:match("^%d+$") then e.killerId = kid else e.killerKind = kid end
  e.killerPos = loc3(tail:match("KillerLoc%s*:%s*([^A-Za-z]+)"))
  e.victimPos = loc3(tail:match("VictimLoc%s*:%s*([^A-Za-z]+)"))
  return e
end

-- "Weapon_M9_C" -> "Weapon_M9"; "BP_Drifter_Lvl_4_C_2146455350" -> "BP_Drifter_Lvl_4"
function M.class_code(s)
  if not s then return nil end
  s = s:gsub("_C_%d+$", ""):gsub("_C$", "")
  return s
end

-- Newest file per category prefix: names like "login_20261013223000.log".
function M.newest(names, category)
  local best, bestTs
  for _, n in ipairs(names or {}) do
    local ts = n:match("^" .. category .. "_(%d%d%d%d%d%d%d%d%d%d%d%d%d%d)%.log$")
    if ts and (not bestTs or ts > bestTs) then best, bestTs = n, ts end
  end
  return best
end

local Tailer = {}
Tailer.__index = Tailer

-- opts: dir, category, fs, from_start (bool: read existing content of the first file; default false = skip it)
function M.tailer(opts)
  return setmetatable({ dir = opts.dir, category = opts.category, fs = opts.fs, file = nil, offset = 0,
    pendingBytes = "", partial = "", partialSeen = 0, fromStart = opts.from_start == true, first = true,
    stats = { files = 0, lines = 0, bytes = 0 } }, Tailer)
end

-- Returns a list of complete lines appended since the last poll. A trailing line without a newline is
-- emitted once it has been stable for `settle` polls (SCUM leaves the last line unterminated until the next
-- entry), and never twice.
function Tailer:poll(settle)
  settle = settle or 2
  local fs = self.fs
  local newest = M.newest(fs.list(self.dir), self.category)
  local lines = {}
  if newest and newest ~= self.file then
    -- flush what is left of the old file
    if self.partial ~= "" and not self.partialEmitted then lines[#lines + 1] = self.partial end
    local switching = self.file ~= nil
    self.file, self.pendingBytes, self.partial, self.partialSeen, self.partialEmitted = newest, "", "", 0, false
    self.stats.files = self.stats.files + 1
    local size = fs.size(self.dir .. "/" .. newest) or 0
    self.offset = (self.first and not self.fromStart and not switching) and size or 0
    self.first = false
  end
  if not self.file then return lines end
  local path = self.dir .. "/" .. self.file
  local size = fs.size(path)
  if not size then return lines end
  if size < self.offset then self.offset, self.pendingBytes, self.partial = 0, "", "" end -- truncated
  if size > self.offset then
    local chunk = fs.read_at(path, self.offset, math.min(size - self.offset, 1048576)) or ""
    self.offset = self.offset + #chunk
    self.stats.bytes = self.stats.bytes + #chunk
    local text, left = M.utf16le_to_utf8(self.pendingBytes .. chunk)
    self.pendingBytes = left
    local buf = self.partial .. text
    if self.partialEmitted and self.partial ~= "" and buf:sub(1, #self.partial) == self.partial then
      -- the unterminated line we already emitted got its newline (and maybe more): skip it
      buf = buf:sub(#self.partial + 1):gsub("^\r?\n", "", 1)
    end
    self.partial, self.partialSeen, self.partialEmitted = "", 0, false
    for l, nl in buf:gmatch("([^\n]*)(\n?)") do
      if nl == "\n" then
        l = l:gsub("\r$", "")
        if l ~= "" then lines[#lines + 1] = l end
      elseif l ~= "" then
        self.partial = l
      end
    end
  elseif self.partial ~= "" and not self.partialEmitted then
    self.partialSeen = self.partialSeen + 1
    if self.partialSeen >= settle then
      lines[#lines + 1] = (self.partial:gsub("\r$", ""))
      self.partialEmitted = true
    end
  end
  self.stats.lines = self.stats.lines + #lines
  return lines
end

return M
