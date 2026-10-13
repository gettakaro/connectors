/**
 * The battlegroup's maps as Takaro map Dimensions.
 *
 * Positions are Unreal world units (cm) straight from the pawn transform: `x` grows east (right on the in-game map),
 * `y` grows south (down on the in-game map), `z` is height.
 *
 * The dedicated server ships no map art: its LinuxServer cook keeps every texture's header but strips the pixel
 * data (`T_WorldMap_Survival_1_D` is a 788-byte stub, and the in-game minimap texture is not in the server paks at
 * all). So each Dimension is declared with source `none` and the operator uploads the image in the Takaro dashboard.
 * What the connector does own is the calibration, taken from the server's own level data:
 *
 * - **Hagga Basin (`Survival_1`)**: the landscape is 8 x 8 streaming tiles (`SOC_1/Tiles/LOD/SOS-MinorErg_x0..7_y0..7`)
 *   of 101600 cm each, with tile origins from -457200 to 254000 on both axes. The playable square is therefore
 *   [-457200, 355600] on x and y. Fitted against the in-game map and 115 world markers, that square is within
 *   about 40 m of the in-game world map's frame.
 *
 * Deep Desert (`DeepDesert_1`) has no tiled landscape in the server data to calibrate from, so it is not declared.
 * Its positions still carry `dimension: "deepdesert_1"`, so Takaro never draws them on the Hagga Basin image.
 */
export interface MapDimension {
  id: string;
  name: string;
  axes: {
    horizontal: { field: 'x' | 'y' | 'z'; direction: 'right' | 'left' };
    vertical: { field: 'x' | 'y' | 'z'; direction: 'up' | 'down' };
  };
  bounds: { horizontal: { min: number; max: number }; vertical: { min: number; max: number } };
  source: { type: 'none' };
}

const HAGGA_BASIN_MIN = -457200;
const HAGGA_BASIN_MAX = 355600;

export const HAGGA_BASIN: MapDimension = {
  id: 'survival_1',
  name: 'Hagga Basin',
  axes: {
    horizontal: { field: 'x', direction: 'right' },
    vertical: { field: 'y', direction: 'down' },
  },
  bounds: {
    horizontal: { min: HAGGA_BASIN_MIN, max: HAGGA_BASIN_MAX },
    vertical: { min: HAGGA_BASIN_MIN, max: HAGGA_BASIN_MAX },
  },
  source: { type: 'none' },
};

/** The Dimensions declared to Takaro. */
export const MAP_DIMENSIONS: readonly MapDimension[] = [HAGGA_BASIN];

/** A battlegroup with one map runs Hagga Basin, and the native plugin is preloaded into that map process. */
export const DEFAULT_DIMENSION = HAGGA_BASIN.id;

/**
 * The Dimension id for a player's map. `worldMap` is `world_partition.map` (`Survival_1`, `DeepDesert_1`,
 * `SH_Arrakeen`, …); `actorMap` is the pawn's own `actors.map`, which names Hagga Basin `HaggaBasin`.
 */
export function dimensionForMap(worldMap?: string | null, actorMap?: string | null): string {
  const map = worldMap?.trim();
  if (map) return map.toLowerCase();
  if (actorMap?.trim().toLowerCase() === 'haggabasin') return HAGGA_BASIN.id;
  return DEFAULT_DIMENSION;
}
