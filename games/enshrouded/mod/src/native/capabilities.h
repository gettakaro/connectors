// The protocol 1 Capability Manifest of the Enshrouded connector: the actions it really answers (every
// live-supported row of games/enshrouded/capabilities.json), the events it emits, and the `map`
// capability with one Dimension for the world.
//
// Map: the dedicated server ships no map art (its .dat/.kfc hold none of the client's map textures), so
// the Dimension's source is `none`. Takaro draws a grid until the operator uploads a map image, which
// Takaro stretches over the declared bounds. World frame (VoxelWorldResource, build 1024233): origin
// (0, 0), 10240 x 10240 m in x (east) and z (north); y is height. Player positions are these raw
// metres, so horizontal is x to the right and vertical is z upwards.
#pragma once
#include <string>

namespace native {

constexpr const char* kMapDimensionId = "embervale";
constexpr const char* kMapDimensionName = "Embervale";
constexpr double kWorldMin = 0, kWorldMax = 10240;

std::string CapabilityManifestJson();  // the JSON array for identify / capabilities

}  // namespace native
