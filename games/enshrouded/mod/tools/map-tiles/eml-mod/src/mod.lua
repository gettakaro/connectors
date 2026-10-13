-- Offline export (emm run -e) of the raw textures the client's map screen is drawn from, plus the world
-- frame. Input for ../render_tiles.py. Needs a complete client install (a sparse copy exports zeros).
local out = {}
local function v3(v) return tostring(v.x) .. "," .. tostring(v.y) .. (v.z and ("," .. tostring(v.z)) or "") end
for _, r in ipairs(game.assets.get_resources_by_type("keen::VoxelWorldResource")) do
  local d = r.data
  local s = "VoxelWorld type=" .. tostring(d.type) .. " origin=" .. v3(d.origin) .. " size=" .. v3(d.size)
  if not out[s] then out[s] = true; out[#out + 1] = s end
end
local want = {
  _map_shader_parameters_baseHeight = true,
  _map_shader_parameters_baseGradient = true,
  _map_shader_parameters_isolineGradient = true,
}
for _, r in ipairs(game.assets.get_resources_by_type("keen::UiTextureResource")) do
  local d = r.data
  local n = tostring(d.debugName)
  if want[n] or n:match("^DetailHeight%(%d+,%d+%)$") then
    local content = game.assets.get_content(game.guid.from_content_hash(d.data))
    out[#out + 1] = n .. " contentSize=" .. tostring(content.size) .. " fmt=" .. tostring(d.format)
    io.export(n:gsub("[%(%)]", "_"):gsub(",", "_") .. ".raw", content:read_data())
  end
end
io.export("world.txt", table.concat(out, "\n") .. "\n")
