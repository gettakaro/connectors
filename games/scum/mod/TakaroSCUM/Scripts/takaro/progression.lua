-- Player progression: turns what the game adapter read from a prisoner into Takaro Progression Tracks. No game or
-- transport symbols here. SCUM has no single character level, so there is no `character` track: every skill is a
-- track (`skill.<name>`), with the game's own level (0 = no skill .. 4 = above advanced), experience and progress.
-- The four attributes are tracks too (`attribute.<name>`), at the attribute's value.
local M = {}

-- ESkillReplicationID, the id in UPrisonerSkillComponent._replicatedSkills records, with the name the game shows.
-- Tactics and Resistance also have records, but the game never shows them to the player, so they are not reported.
M.SKILLS = {
  [1] = { key = "skill.endurance", name = "Endurance" },
  [3] = { key = "skill.running", name = "Running" },
  [5] = { key = "skill.medical", name = "Medical" },
  [6] = { key = "skill.awareness", name = "Awareness" },
  [7] = { key = "skill.stealth", name = "Stealth" },
  [9] = { key = "skill.cooking", name = "Cooking" },
  [10] = { key = "skill.survival", name = "Survival" },
  [12] = { key = "skill.brawling", name = "Brawling" },
  [13] = { key = "skill.meleeWeapons", name = "Melee Weapons" },
  [14] = { key = "skill.rifles", name = "Rifles" },
  [15] = { key = "skill.handgun", name = "Handgun" },
  [16] = { key = "skill.sniping", name = "Sniping" },
  [17] = { key = "skill.camouflage", name = "Camouflage" },
  [20] = { key = "skill.archery", name = "Archery" },
  [21] = { key = "skill.thievery", name = "Thievery" },
  [22] = { key = "skill.driving", name = "Driving" },
  [23] = { key = "skill.motorcycling", name = "Motorcycling" },
  [24] = { key = "skill.engineering", name = "Engineering" },
  [25] = { key = "skill.demolition", name = "Demolition" },
  [26] = { key = "skill.aviation", name = "Aviation" },
  [27] = { key = "skill.farming", name = "Farming" },
}

M.MAX_SKILL_LEVEL = 4

-- Experience for a full level, read off the percentages on the client's stats screen. Experience starts again at 0
-- on every level; level 4 is the last and fills at the same 10,000,000 as level 3.
M.LEVEL_EXPERIENCE = { [0] = 10000, [1] = 100000, [2] = 1000000, [3] = 10000000, [4] = 10000000 }

-- UPrisonerBodySimulationComponent._repState1 packs one byte per attribute, scaled from 1 to the attribute's maximum
-- (the same 32 bits are repeated in the high half).
M.ATTRIBUTES = {
  { key = "attribute.strength", name = "Strength", shift = 24, max = 8 },
  { key = "attribute.constitution", name = "Constitution", shift = 16, max = 5 },
  { key = "attribute.dexterity", name = "Dexterity", shift = 8, max = 5 },
  { key = "attribute.intelligence", name = "Intelligence", shift = 0, max = 8 },
}

-- A level only moves once the value is this far from it, so a value drifting around x.5 fires no level events.
M.ATTRIBUTE_MARGIN = 0.6

local function whole(v)
  return type(v) == "number" and v == v and v >= 0 and v <= M.MAX_SKILL_LEVEL and math.floor(v) == v
end

-- records: { { id, level, experience } }. A record that is not a known skill with a valid level is left out: one bad
-- track fails the whole reading in Takaro, and a track missing from a reading just keeps its stored value.
function M.skill_tracks(records)
  local out, seen = {}, {}
  for _, r in ipairs(records or {}) do
    local skill = M.SKILLS[r.id]
    if skill and whole(r.level) and not seen[skill.key] then
      seen[skill.key] = true
      local t = { key = skill.key, name = skill.name, level = math.floor(r.level) }
      if type(r.experience) == "number" and r.experience == r.experience and r.experience >= 0
        and r.experience < math.huge then
        t.experience = r.experience
        local full = M.LEVEL_EXPERIENCE[t.level]
        if full then t.progress = math.min(r.experience / full, 1) end
      end
      out[#out + 1] = t
    end
  end
  table.sort(out, function(a, b) return a.key < b.key end)
  return out
end

-- packed: the _repState1 value. previous: the attribute tracks of the last reading of this character, if any.
-- The level is the nearest whole value: a new prisoner's 3.0 reads 2.98 and drifts, so rounding down would drop it.
function M.attribute_tracks(packed, previous)
  if math.type(packed) ~= "integer" or packed & 0xFFFFFFFF == 0 then return {} end
  local last = {}
  for _, t in ipairs(previous or {}) do last[t.key] = t.level end
  local out = {}
  for _, a in ipairs(M.ATTRIBUTES) do
    local value = 1 + ((packed >> a.shift) & 0xFF) / 255 * (a.max - 1)
    local level = math.floor(value + 0.5)
    if last[a.key] and math.abs(value - last[a.key]) <= M.ATTRIBUTE_MARGIN then level = last[a.key] end
    out[#out + 1] = { key = a.key, name = a.name, level = level }
  end
  return out
end

return M
