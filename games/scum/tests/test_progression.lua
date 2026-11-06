-- Player progression: track building, the cached adapter read, the router answers and the manifest entry.
local t = require("lib.t")
local F = require("lib.fakes")
local json = require("takaro.json")
local game = require("takaro.game")
local progression = require("takaro.progression")
local v1 = require("takaro.v1")

local STEAM = "76561198000000001"
local KEY_PATTERN = "^[a-z][a-zA-Z0-9]*%.[a-z][a-zA-Z0-9-]*$"

t.test("every known skill has a valid, unique key and a name; the reserved character key is not used", function()
  local seen = {}
  for _, s in pairs(progression.SKILLS) do
    t.ok(s.key:match(KEY_PATTERN), s.key)
    t.ok(not seen[s.key], s.key)
    seen[s.key] = true
    t.ok(#s.name > 0, s.key)
    t.ok(s.key ~= "character")
  end
end)

t.test("skill_tracks keeps level and experience and leaves out hidden skills", function()
  local tracks = progression.skill_tracks({
    { id = 3, level = 2, experience = 36.5 },
    { id = 18, level = 2, experience = 0 }, -- Tactics: never shown to the player
    { id = 2, level = 0, experience = 0 }, -- Resistance: never shown to the player
    { id = 12, level = 0, experience = 0 },
  })
  t.deep(tracks, {
    { key = "skill.brawling", name = "Brawling", level = 0, experience = 0, progress = 0 },
    { key = "skill.running", name = "Running", level = 2, experience = 36.5, progress = 36.5 / 1000000 },
  })
end)

t.test("skill_tracks drops records that would fail the whole reading", function()
  local tracks = progression.skill_tracks({
    { id = 3, level = 1.5, experience = 1 },
    { id = 6, level = 5, experience = 1 },
    { id = 9, level = -1, experience = 1 },
    { id = 10, level = "1", experience = 1 },
    { id = 99, level = 1, experience = 1 },
    { id = 12, level = 1, experience = 1 },
    { id = 12, level = 2, experience = 1 },
  })
  t.eq(#tracks, 1)
  t.eq(tracks[1].level, 1)
end)

t.test("skill_tracks omits an experience that is not a finite number", function()
  for _, xp in ipairs({ 0 / 0, math.huge, -1, "x" }) do
    t.eq(progression.skill_tracks({ { id = 3, level = 1, experience = xp } })[1].experience, nil)
  end
end)

t.test("progress follows the level's experience scale and stops at 1", function()
  local function track(level, xp) return progression.skill_tracks({ { id = 3, level = level, experience = xp } })[1] end
  t.eq(track(0, 101).progress, 0.0101)
  t.eq(track(1, 14200).progress, 0.142)
  t.eq(track(0, 20000).progress, 1)
  t.eq(track(3, 1000000).progress, 0.1)
  t.eq(track(4, 1000000).progress, 0.1)
  t.eq(track(4, 20000000).progress, 1)
  t.eq(track(3, 500).experience, 500)
end)

local function adapter(records)
  local reads = 0
  local now = 0
  local skills = { ForEach = function(_, fn)
    reads = reads + 1
    for i, r in ipairs(records.list) do fn(i, { get = function() return r end }) end
  end }
  local prisoner = { IsValid = function() return true end,
    SkillComponent = { IsValid = function() return true end, _replicatedSkills = skills } }
  local pc = { IsValid = function() return true end, GetUserId = function() return STEAM end,
    GetPrisoner = function() return records.noPrisoner and { IsValid = function() return false end } or prisoner end }
  local log = { info = function() end, warn = function() end, error = function() end, debug = function() end }
  local g = game.new({ FindAllOf = function() return { pc } end, now_us = function() now = now + 1; return now end },
    log, { mod_dir = "/nowhere/SCUM/Binaries/Win64/ue4ss/Mods/TakaroSCUM",
      fs = { list = function() return {} end, read = function() return nil end, write = function() return true end,
        size = function() return nil end, read_at = function() return nil end } })
  return g, function() return reads end
end

t.test("the request reads the cache and never the game", function()
  local g, reads = adapter({ list = { { ID = 3, Level = 1, ExperiencePoints = 5 } } })
  t.eq(g:get_player_progression({ gameId = STEAM }).ok, false)
  g:refresh_progression(1000)
  t.eq(reads(), 1)
  for _ = 1, 3 do t.eq(g:get_player_progression({ gameId = STEAM }).value[1].key, "skill.running") end
  t.eq(reads(), 1)
end)

t.test("a player is read again only after the refresh time", function()
  local g, reads = adapter({ list = { { ID = 3, Level = 1, ExperiencePoints = 5 } } })
  g:refresh_progression(1000)
  g:refresh_progression(1000 + game.PROGRESSION_REFRESH_MS - 1)
  t.eq(reads(), 1)
  g:refresh_progression(1000 + game.PROGRESSION_REFRESH_MS)
  t.eq(reads(), 2)
end)

t.test("a player without a prisoner keeps the last reading", function()
  local records = { list = { { ID = 3, Level = 2, ExperiencePoints = 5 } } }
  local g = adapter(records)
  g:refresh_progression(1000)
  records.noPrisoner = true
  g:refresh_progression(1000 + game.PROGRESSION_REFRESH_MS)
  t.eq(g:get_player_progression({ gameId = STEAM }).value[1].level, 2)
end)

local function connect_v1(tk, native)
  native.open()
  tk.pump()
  native.deliver({ type = "identifyResponse", payload = { gameServerId = "gs-1", protocolVersion = 1 } })
  tk.pump()
end

local function answer(native, requestId)
  for i = #native.sent, 1, -1 do
    local s = native.sent[i]
    if s.msg.type == "response" and s.msg.requestId == requestId then return s end
  end
end

t.test("the wire answer: null for an unknown player, [] for a known one without a reading, never an error", function()
  local a, native, tk = F.app()
  connect_v1(tk, native)
  a.known:remember({ gameId = STEAM, name = "Known", steamId = STEAM })
  local unknown = tk.request("getPlayerProgression", { gameId = "76561198000000002" })
  local known = tk.request("getPlayerProgression", { gameId = STEAM })
  local noId = tk.request("getPlayerProgression", {})
  tk.pump()
  t.ok(answer(native, unknown).text:find('"payload":null', 1, true), answer(native, unknown).text)
  t.ok(answer(native, known).text:find('"payload":[]', 1, true), answer(native, known).text)
  t.ok(answer(native, noId).text:find('"payload":null', 1, true), answer(native, noId).text)
end)

t.test("the wire answer carries the cached tracks", function()
  local a, native, tk = F.app()
  connect_v1(tk, native)
  a.game.get_player_progression = function()
    return { ok = true, value = { { key = "skill.running", name = "Running", level = 1, experience = 9.5 } } }
  end
  local id = tk.request("getPlayerProgression", { gameId = STEAM })
  tk.pump()
  t.deep(answer(native, id).msg.payload,
    { { key = "skill.running", name = "Running", level = 1, experience = 9.5 } })
end)

t.test("a timed-out reading is null, so Takaro keeps the stored levels", function()
  local a = F.app()
  t.eq(a.router:fallback("getPlayerProgression", "timeout", "{}"), json.null)
end)

t.test("the manifest declares players.progression with source server", function()
  local found
  for _, c in ipairs(v1.manifest(nil, nil)) do
    if c.name == "players.progression" then found = c end
  end
  t.deep(found, { name = "players.progression", parameters = { source = "server" } })
end)

t.test("attribute_tracks unpacks the bytes the client showed as STR 5.3, CON 5.0, DEX 2.8, INT 7.1", function()
  local levels = {}
  for _, tr in ipairs(progression.attribute_tracks(0x9AFF6FDF9AFF6FDF)) do levels[tr.key] = tr.level end
  t.deep(levels, { ["attribute.strength"] = 5, ["attribute.constitution"] = 5, ["attribute.dexterity"] = 3,
    ["attribute.intelligence"] = 7 })
end)

t.test("a new prisoner's attributes of 3.0 read as level 3", function()
  for _, tr in ipairs(progression.attribute_tracks(0x487F7F48)) do t.eq(tr.level, 3, tr.key) end
end)

t.test("an attribute level holds near the half-way point and moves once the value is clear of it", function()
  local function strength(byte, level)
    return progression.attribute_tracks(byte << 24 | 0x7F7F48, { { key = "attribute.strength", level = level } })[1].level
  end
  t.eq(strength(0x5B, 3), 3) -- 3.5
  t.eq(strength(0x5D, 4), 4) -- 3.55
  t.eq(strength(0x60, 3), 4) -- 3.64
end)

t.test("no attribute tracks without a packed value", function()
  t.deep(progression.attribute_tracks(nil), {})
  t.deep(progression.attribute_tracks(0), {})
end)

t.test("a failed attribute read keeps the last attribute tracks", function()
  local g = adapter({ list = { { ID = 3, Level = 1, ExperiencePoints = 5 } } })
  local packed = 0x487F7F48
  g.read_attributes = function() return packed end
  g:refresh_progression(1000)
  packed = nil
  g:refresh_progression(1000 + game.PROGRESSION_REFRESH_MS)
  t.eq(#g:get_player_progression({ gameId = STEAM }).value, 5)
end)

t.run("progression")
