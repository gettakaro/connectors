-- SCUM gameplay log decoder, parsers and the rotation-aware tailer.
local t = require("lib.t")
local L = require("takaro.scumlog")

local function u16(s)
  local out = {}
  for _, c in utf8.codes(s) do
    if c >= 0x10000 then
      local v = c - 0x10000
      local hi, lo = 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF)
      out[#out + 1] = string.char(hi & 0xFF, hi >> 8, lo & 0xFF, lo >> 8)
    else
      out[#out + 1] = string.char(c & 0xFF, c >> 8)
    end
  end
  return table.concat(out)
end

local function memfs()
  local fs = { files = {} }
  function fs.list() local o = {} for k in pairs(fs.files) do o[#o + 1] = k:match("[^/]+$") end return o end
  function fs.size(p) return fs.files[p] and #fs.files[p] or nil end
  function fs.read_at(p, off, n) return fs.files[p] and fs.files[p]:sub(off + 1, off + n) end
  function fs.append(p, s) fs.files[p] = (fs.files[p] or "") .. s end
  return fs
end

t.test("UTF-16LE decode: BOM dropped, umlauts and emoji, odd byte kept for the next read", function()
  local bytes = "\xFF\xFE" .. u16("Grüße 🙂")
  local text, left = L.utf16le_to_utf8(bytes .. "A")
  t.eq(text, "Grüße 🙂")
  t.eq(left, "A")
  local text2, left2 = L.utf16le_to_utf8(u16("x") .. "\x3D\xD8") -- dangling high surrogate
  t.eq(text2, "x")
  t.eq(#left2, 2)
end)

t.test("login and logout lines (ip, SteamID64, name, profile id, position, drone)", function()
  local e = L.parse_login("2026.03.16-19.34.08: '10.0.0.1 76561198000000001:Player Name(1)' logged in at: X=150991.000 Y=-40018.000 Z=33531.000")
  t.deep(e, { kind = "login", ip = "10.0.0.1", steamId = "76561198000000001", name = "Player Name", profileId = "1",
    position = { x = 150991, y = -40018, z = 33531 }, drone = false })
  local o = L.parse_login("2026.03.16-19.40.08: '10.0.0.1 76561198000000001:Bob(2)' logged out at: X=1 Y=2 Z=3 (as drone)")
  t.eq(o.kind, "logout")
  t.eq(o.drone, true)
  t.eq(L.parse_login("2026.03.16-19.40.08: something else"), nil)
end)

t.test("chat lines map SCUM channels onto Takaro channels", function()
  local e = L.parse_chat("2026.03.16-18.22.07: '76561198000000001:PlayerName(1)' 'Global: hello: world'")
  t.deep(e, { steamId = "76561198000000001", name = "PlayerName", profileId = "1", scumChannel = "Global",
    channel = "global", msg = "hello: world" })
  t.eq(L.parse_chat("2026.03.16-18.22.07: '76561198000000001:P(1)' 'Squad: hi'").channel, "team")
  t.eq(L.parse_chat("2026.03.16-18.22.07: '76561198000000001:P(1)' 'Local: hi'").channel, "friends")
end)

t.test("admin lines", function()
  t.deep(L.parse_admin("2026.03.16-18.22.07: '76561198000000001:Admin(1)' Command: 'SpawnItem Weapon_M9 1'"),
    { steamId = "76561198000000001", name = "Admin", command = "SpawnItem Weapon_M9 1" })
end)

t.test("kill lines: player killer, NPC killer, weapon code without _C, positions", function()
  local e = L.parse_kill("2025.10.25-15.39.01: Died: Barry (76561198000000003), Killer: BP_Drifter_Lvl_4_C_2146455350 (NPC) Weapon: Weapon_DEagle_50_C [Projectile] S:[KillerLoc : -26007.18, -271845.03, 19384.88 VictimLoc: -26143.72, -272107.22, 19387.05, Distance: 2.96 m]")
  t.eq(e.victimId, "76561198000000003")
  t.eq(e.killerKind, "NPC")
  t.eq(e.killerId, nil)
  t.eq(L.class_code(e.killerName), "BP_Drifter_Lvl_4")
  t.eq(L.class_code(e.weapon), "Weapon_DEagle_50")
  t.eq(e.damageType, "Projectile")
  t.deep(e.victimPos, { x = -26143.72, y = -272107.22, z = 19387.05 })
  local p = L.parse_kill("2025.10.25-15.39.01: Died: A (76561198000000001), Killer: B (76561198000000002) Weapon: Knife_C [Melee]")
  t.eq(p.killerId, "76561198000000002")
end)

t.test("tailer: skips old content on first open, emits new lines, survives split UTF-16 reads", function()
  local fs = memfs()
  local dir = "/logs"
  fs.append(dir .. "/chat_20261013220000.log", "\xFF\xFE" .. u16("old line\r\n"))
  local tl = L.tailer({ dir = dir, category = "chat", fs = fs })
  t.deep(tl:poll(), {})
  local more = u16("first\r\nsecond\r\n")
  fs.append(dir .. "/chat_20261013220000.log", more:sub(1, 5))
  t.deep(tl:poll(), {})
  fs.append(dir .. "/chat_20261013220000.log", more:sub(6))
  t.deep(tl:poll(), { "first", "second" })
end)

t.test("tailer: an unterminated last line is emitted once after settling, never twice", function()
  local fs = memfs()
  local p = "/logs/kill_20261013220000.log"
  fs.append(p, "")
  local tl = L.tailer({ dir = "/logs", category = "kill", fs = fs })
  tl:poll()
  fs.append(p, u16("a\r\nlast"))
  t.deep(tl:poll(2), { "a" })
  t.deep(tl:poll(2), {})
  t.deep(tl:poll(2), { "last" })
  fs.append(p, u16("\r\nnext\r\n"))
  t.deep(tl:poll(2), { "next" })
end)

t.test("tailer: a new file per server start is picked up from its beginning", function()
  local fs = memfs()
  fs.append("/logs/login_20261013220000.log", u16("x\r\n"))
  local tl = L.tailer({ dir = "/logs", category = "login", fs = fs })
  tl:poll()
  fs.append("/logs/login_20261013230000.log", "\xFF\xFE" .. u16("new start\r\n"))
  fs.append("/logs/chat_20261014000000.log", u16("other category\r\n"))
  t.deep(tl:poll(), { "new start" })
  t.eq(tl.file, "login_20261013230000.log")
end)

t.run("scumlog")
