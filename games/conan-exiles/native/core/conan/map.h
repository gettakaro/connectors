// The Takaro protocol 1 map Dimension of the loaded Conan map.
//
// The dedicated server ships no map art (the LinuxServer and WindowsServer cooks only reference
// the client textures), so every Dimension declares `source: none`: Takaro draws a grid until the
// operator uploads the in-game map image in the dashboard. The calibration is the server's own:
// Base/AlwaysCook/RegionDataTable, struct MapCoordinatesAndSpans, gives two world <-> image reference
// points per map on the in-game map texture. The bounds below are that texture's full frame in
// world units, so an upload of the whole texture (any resolution, Takaro stretches it) lines up.
// Axes: world x grows to the right of the map, world y grows down (south), as the in-game map draws.
#pragma once

#include <string>

namespace conan {

struct MapCalibration {
    const char* region;   // ReadService::Region(): "ExiledLands" / "IsleOfSiptah"
    const char* id;       // Takaro Dimension id; positions on this map carry it as `dimension`
    const char* name;     // shown in Takaro's Dimension switcher
    const char* texture;  // the client texture whose full frame the bounds describe
    // RegionDataTable MapCoordinatesAndSpans: NorthEastWorld/SouthWestWorld (world x, y) and
    // NorthEastImage/SouthWestImage (texture pixels, x right, y down).
    double neWorld[2], swWorld[2], neImage[2], swImage[2];
    int textureWidth, textureHeight;
};

struct MapBounds {
    double minX, maxX, minY, maxY;
};

// nullptr when the region is unknown (world not loaded yet, or a map we have no row for).
const MapCalibration* MapForRegion(const std::string& region);
// The world rectangle the full texture covers: linear in each axis through the two reference points.
MapBounds BoundsOf(const MapCalibration& m);
// One protocol 1 Dimension object: { id, name, axes, bounds, source: { type: "none" } }.
std::string DimensionJson(const MapCalibration& m);

}  // namespace conan
