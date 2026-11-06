-- Offline export (emm run -e) against the server, chunks FIRST..LAST: every scene placement whose
-- template is a map marker, player spawn point or location (plus the template table on the first run).
-- emm runs out of Lua reference slots on the whole world in one run (about 700 chunks), so export
-- it in ranges: run once per range, editing FIRST/LAST (1..250, 251..500, ... up to the chunk count).
local FIRST, LAST = 1, 250
local tmpl, trows = {}, {}
for i, r in ipairs(game.assets.get_resources_by_type("keen::ecs::TemplateResource")) do
  local kind, target = "", ""
  local d = r.data
  for _, c in ipairs(d.components) do
    local t = c.type
    if t == "keen::ecs::PlayerSpawnPoint" then kind = "spawnPoint"; target = tostring(c.value.target)
    elseif t == "keen::ecs::MapMarker" and kind ~= "spawnPoint" then kind = "mapMarker"
    elseif t == "keen::ecs::Location" and kind == "" then kind = "location" end
  end
  if kind == "" and tostring(d.family or "") == "Altar" then kind = "location" end
  if kind ~= "" then
    tmpl[tostring(r.guid)] = true
    trows[#trows + 1] = tostring(r.guid) .. "\t" .. d.name .. "\t" .. kind .. "\t" .. target
  end
  if i % 100 == 0 then collectgarbage() end
end
if FIRST == 1 then io.export("location-templates.tsv", table.concat(trows, "\n") .. "\n") end
local chunks = game.assets.get_resources_by_type("keen::SceneEntityChunkResource")
local out = { "#range\t" .. FIRST .. "\t" .. math.min(LAST, #chunks) .. "\t" .. #chunks }
for i, r in ipairs(chunks) do
  if i >= FIRST and i <= LAST then
    local d = r.data
    local ents, temps = d.entities, d.templates
    for j = 1, #ents do
      local e = ents[j]
      local g = tostring(temps[e.templateIndex + 1])
      if tmpl[g] then
        local t = e.transform
        out[#out + 1] = r.part .. "\t" .. g .. "\t" .. t.positionX1 .. "\t" .. t.positionX0 .. "\t" .. t.positionY1 .. "\t" ..
          t.positionY0 .. "\t" .. t.positionZ1 .. "\t" .. t.positionZ0
      end
    end
    collectgarbage()
  end
end
print("chunks " .. FIRST .. ".." .. LAST .. " of " .. #chunks .. ": " .. (#out - 1) .. " placements")
io.export("placements-" .. FIRST .. ".tsv", table.concat(out, "\n") .. "\n")
