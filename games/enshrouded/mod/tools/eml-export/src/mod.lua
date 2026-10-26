-- Offline export (emm run -e): English (En_Us) display names for items, weapon categories and entity templates.
local loc = game.assets.get_resources_by_type("keen::LocaTagCollectionResource")[1].data
local hash = nil
local langs = {}
for _, l in ipairs(loc.languages) do
  langs[#langs + 1] = tostring(l.language)
  -- 1.0 (KFC4) moved the content hash into `data.hash`
  if tostring(l.language) == "En_Us" then hash = l.dataHash or (l.data and l.data.hash) end
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
local dirCats = {}
local okDir, dirs = pcall(game.assets.get_resources_by_type, "keen::WeaponCategoryDirectoryResource")
if okDir and dirs then
  for _, d in ipairs(dirs) do
    for _, e in ipairs(d.data.weaponCategories) do dirCats[tostring(e.guid)] = e.weaponCategory end
  end
end
for _, r in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
  local ref = r.data.weaponCategoryReference
  if ref and ref ~= "00000000-0000-0000-0000-000000000000" and not seen[ref] then
    seen[ref] = true
    -- 1.0 keeps weapon categories inside WeaponCategoryDirectoryResource; older builds as their own resources
    local w = dirCats[tostring(ref)]
    local ok, res = true, nil
    if not w then
      ok, res = pcall(game.assets.get_resource, ref, "keen::WeaponCategory")
      if ok and res then w = res.data end
    end
    if w then
      cats[#cats + 1] = tostring(game.guid.hash(ref)) .. "\t" .. tostring(ref) .. "\t" .. tostring(w.categoryType) .. "\t" .. clean(dict[game.guid.hash(w.locaTag)])
    else
      print("category " .. tostring(ref) .. " unresolved: " .. tostring(res))
    end
  end
end
io.export("weapon-categories.tsv", table.concat(cats, "\n") .. "\n")
-- Entity templates. The client names a creature where a player sees a name: a boss health bar
-- (BossHealthBar.displayName), an NPC (NpcSetup.name, NpcDialog's headline or the template's
-- LocaTagComponent) or a BossDisplay. Regular creatures carry no name of their own; the model,
-- arsenal and faction columns let gen_entity_labels.py tie a variant to a named template.
local function ref(r)
  if not r or r == "00000000-0000-0000-0000-000000000000" then return "" end
  return clean(dict[game.guid.hash(r)])
end
local function zero(g) return (not g or g == "00000000-0000-0000-0000-000000000000") and "" or tostring(g) end
local tmpl = {}
for i, r in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
  local boss, npc, bossd, tag, dialog, model, arsenal, faction = "", "", "", "", "", "", "", ""
  for _, c in ipairs(r.data.components) do
    local t, v = c.type, c.value
    if t == "keen::ecs::BossHealthBar" then boss = ref(v.displayName)
    elseif t == "keen::ecs::NpcSetup" then npc = ref(v.name)
    elseif t == "keen::ecs::BossDisplay" then bossd = clean(dict[v.displayName.value or v.displayName])
    elseif t == "keen::ecs::LocaTagComponent" then tag = ref(v.locaTag)
    elseif t == "keen::ecs::NpcDialog" then
      local ok, h = pcall(function() return v.dialogResource.headline.value end)
      if ok and h then dialog = clean(dict[h]) end
    elseif t == "keen::ecs::ModelResource" then model = zero(v.model)
    elseif t == "keen::ecs::EnemyArsenalReference" then arsenal = zero(v.arsenal)
    elseif t == "keen::ecs::Faction" then faction = tostring(v.faction) end
  end
  local name, src = "", ""
  if boss ~= "" then name, src = boss, "bossHealthBar"
  elseif npc ~= "" then name, src = npc, "npcSetup"
  elseif bossd ~= "" then name, src = bossd, "bossDisplay"
  elseif tag ~= "" then name, src = tag, "locaTag"
  elseif dialog ~= "" then name, src = dialog, "npcDialog" end
  tmpl[#tmpl + 1] = table.concat({tostring(r.guid), r.data.name, name, src, model, arsenal, faction}, "\t")
  if i % 500 == 0 then collectgarbage("collect") end
end
io.export("templates-en.tsv", table.concat(tmpl, "\n") .. "\n")
-- The Logic Spawner's list (1.0 building menu) names the creatures a player can place a spawner for.
local spawns = {}
for _, r in ipairs(game.assets.get_resources_by_type("keen::LogicSystemResource")) do
  for _, s in ipairs(r.data.spawnable) do
    local n = clean(dict[s.nameId.value])
    if n ~= "" then spawns[#spawns + 1] = tostring(s.templateRef) .. "\t" .. n end
  end
end
io.export("logic-spawns-en.tsv", table.concat(spawns, "\n") .. "\n")
-- Items that are a creature: a farm animal, pet, baby or NPC placed from the inventory, and a fish
-- by its catch.
local placed = {}
for _, r in ipairs(game.assets.get_resources_by_type("keen::ItemInfo")) do
  local d = r.data
  local n = clean(dict[game.guid.hash(d.name)])
  if n ~= "" then
    local okP, p = pcall(function() return d.equipment.placedEntity end)
    if okP and zero(p) ~= "" then placed[#placed + 1] = zero(p) .. "\t" .. d.debugName .. "\t" .. n .. "\tplacedEntity" end
    local okF, f = pcall(function() return d.fishableItemSetup.minigameEntity end)
    if okF and zero(f) ~= "" then placed[#placed + 1] = zero(f) .. "\t" .. d.debugName .. "\t" .. n .. "\tminigameEntity" end
  end
end
io.export("item-entities-en.tsv", table.concat(placed, "\n") .. "\n")
-- The whole En_Us table, for the faction words the generator checks against it.
local lines = {}
for _, tag in ipairs(data.tags) do lines[#lines + 1] = tostring(tag.id.value) .. "\t" .. clean(tag.text) end
io.export("loca-en.tsv", table.concat(lines, "\n") .. "\n")
