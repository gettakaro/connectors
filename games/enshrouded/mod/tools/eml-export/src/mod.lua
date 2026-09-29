-- Offline export (emm run -e): English (En_Us) display names for items and weapon categories.
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
