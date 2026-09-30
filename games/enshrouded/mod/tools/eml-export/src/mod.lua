-- Offline export (emm run -e): English (En_Us) display names for items, weapon categories and entity templates.
local loc = game.assets.get_resources_by_type("keen::LocaTagCollectionResource")[1].data
local hash = nil
local langs = {}
for _, l in ipairs(loc.languages) do
  langs[#langs + 1] = tostring(l.language)
  if tostring(l.language) == "En_Us" then hash = l.dataHash end
end
print("languages: " .. table.concat(langs, ","))
local buf = game.assets.get_content(game.guid.from_content_hash(hash)):read_data()
local data = buf:read_resource("keen::LocaTagCollectionResourceData")
local dict = {}
for _, tag in ipairs(data.tags) do dict[tag.id.value] = tag.text end
local function clean(s) return (s or ""):gsub("[\t\r\n]", " ") end
local out = {}
for _, r in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
  local d = r.data
  local wc = ""
  local ref = d.weaponCategoryReference
  if ref and ref ~= "00000000-0000-0000-0000-000000000000" then wc = tostring(game.guid.hash(ref)) end
  out[#out + 1] = d.itemId.value .. "\t" .. d.debugName .. "\t" .. clean(dict[game.guid.hash(d.name)]) .. "\t" .. tostring(d.category) .. "\t" .. wc
end
io.export("items-en.tsv", table.concat(out, "\n") .. "\n")
local cats, seen = {}, {}
for _, r in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
  local ref = r.data.weaponCategoryReference
  if ref and ref ~= "00000000-0000-0000-0000-000000000000" and not seen[ref] then
    seen[ref] = true
    local ok, res = pcall(game.assets.get_resource, ref, "keen::WeaponCategory")
    if ok and res then
      local w = res.data
      cats[#cats + 1] = tostring(game.guid.hash(ref)) .. "\t" .. tostring(ref) .. "\t" .. tostring(w.categoryType) .. "\t" .. clean(dict[game.guid.hash(w.locaTag)])
    else
      print("category " .. tostring(ref) .. " unresolved: " .. tostring(res))
    end
  end
end
io.export("weapon-categories.tsv", table.concat(cats, "\n") .. "\n")
-- Entity templates: the client names a creature only where it shows one, which is a boss health bar
-- (BossHealthBar.displayName), an NPC (NpcSetup.name or the template's LocaTagComponent) or a
-- BossDisplay. Regular creatures carry no display name at all.
local function ref(r)
  if not r or r == "00000000-0000-0000-0000-000000000000" then return "" end
  return clean(dict[game.guid.hash(r)])
end
local tmpl = {}
for _, r in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
  local boss, npc, bossd, tag = "", "", "", ""
  for _, c in ipairs(r.data.components) do
    local t, v = c.type, c.value
    if t == "keen::ecs::BossHealthBar" then boss = ref(v.displayName)
    elseif t == "keen::ecs::NpcSetup" then npc = ref(v.name)
    elseif t == "keen::ecs::BossDisplay" then bossd = clean(dict[v.displayName.value or v.displayName])
    elseif t == "keen::ecs::LocaTagComponent" then tag = ref(v.locaTag) end
  end
  local name, src = "", ""
  if boss ~= "" then name, src = boss, "bossHealthBar"
  elseif npc ~= "" then name, src = npc, "npcSetup"
  elseif bossd ~= "" then name, src = bossd, "bossDisplay"
  elseif tag ~= "" then name, src = tag, "locaTag" end
  tmpl[#tmpl + 1] = tostring(r.guid) .. "\t" .. r.data.name .. "\t" .. name .. "\t" .. src
end
io.export("templates-en.tsv", table.concat(tmpl, "\n") .. "\n")
