-- The Takaro protocol 1 map Dimension of SCUM's island. No game symbols here (the adapter reads the world bounds).
--
-- The dedicated server ships no map art and the connector ships none, so the Dimension is `source: none`: Takaro
-- draws a grid with the player dots until the operator uploads the in-game map image in the dashboard.
-- The in-game map is the client texture /Game/ConZ_Files/UI/HUD/IslandMap (2048 x 2048, stored upside down: the map
-- widget shows it turned by 180 degrees). It frames the 5 x 5 sector grid (Z..D, 4..0), 3048 m (10,000 ft) per
-- sector: 1,524,000 world cm per side, centred in the server's AConZWorldSettings.WorldBounds, which adds a 500 m
-- margin on every side. So the map bounds are WorldBounds inset by 50,000 cm (read from the world at runtime; the
-- constants are SCUM 1.3.4.2's). Calibrated live against the in-game player marker (4 spots, 2026-10-14).
-- Positions keep getPlayerLocation's units (UE world cm), so the player dots line up without conversion.
local M = {}

M.DIMENSION_ID = "the_island"
M.NAME = "The Island"
M.TEXTURE = "/Game/ConZ_Files/UI/HUD/IslandMap"

-- AConZWorldSettings.WorldBounds (FBox2D, world cm) on SCUM 1.3.4.2, and the margin the map leaves out of it.
M.DEFAULT_WORLD_BOUNDS = { minX = -954800, maxX = 669200, minY = -954800, maxY = 669200 }
M.MAP_INSET = 50000

-- The map frame inside a world bounds box.
function M.map_bounds(world)
  local i = M.MAP_INSET
  return { minX = world.minX + i, maxX = world.maxX - i, minY = world.minY + i, maxY = world.maxY - i }
end

M.DEFAULT_BOUNDS = M.map_bounds(M.DEFAULT_WORLD_BOUNDS) -- x and y -904800 .. 619200

-- How the in-game map draws the world: world X grows to the left, world Y grows up (the operator uploads the image
-- the way the game shows it, i.e. the texture turned by 180 degrees).
M.AXES = { horizontal = { field = "x", direction = "left" }, vertical = { field = "y", direction = "up" } }

local function finite(v) return type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge end

-- A usable world rectangle: finite numbers, min < max and at least 1 km per side.
function M.valid_bounds(b)
  return type(b) == "table" and finite(b.minX) and finite(b.maxX) and finite(b.minY) and finite(b.maxY)
    and b.maxX - b.minX >= 100000 and b.maxY - b.minY >= 100000
end

local function axis_range(b, field)
  if field == "x" then return { min = b.minX, max = b.maxX } end
  return { min = b.minY, max = b.maxY }
end

-- One protocol 1 Dimension: { id, name, axes, bounds, source = { type = "none" } }. `bounds` is the map frame
-- (map_bounds of the world bounds); invalid or missing bounds fall back to the SCUM 1.3.4.2 frame.
function M.dimension(bounds)
  local b = M.valid_bounds(bounds) and bounds or M.DEFAULT_BOUNDS
  return {
    id = M.DIMENSION_ID,
    name = M.NAME,
    axes = {
      horizontal = { field = M.AXES.horizontal.field, direction = M.AXES.horizontal.direction },
      vertical = { field = M.AXES.vertical.field, direction = M.AXES.vertical.direction },
    },
    bounds = {
      horizontal = axis_range(b, M.AXES.horizontal.field),
      vertical = axis_range(b, M.AXES.vertical.field),
    },
    source = { type = "none" },
  }
end

-- The `map` capability of the Capability Manifest.
function M.capability(bounds)
  return { name = "map", parameters = { dimensions = { M.dimension(bounds) } } }
end

-- Where a world position lands on the full map image, as fractions 0..1 from the top-left corner.
-- Used by tests and by the calibration notes; Takaro does the same projection from the Dimension.
function M.project(bounds, x, y)
  local d = M.dimension(bounds)
  local function frac(axis, pos)
    local r = d.bounds[axis]
    local f = (pos - r.min) / (r.max - r.min)
    local dir = d.axes[axis].direction
    if dir == "left" or dir == "up" then f = 1 - f end
    return f
  end
  local h = d.axes.horizontal.field == "x" and x or y
  local v = d.axes.vertical.field == "x" and x or y
  return frac("horizontal", h), frac("vertical", v)
end

return M
