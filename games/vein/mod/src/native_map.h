#pragma once

// VEIN's world map for Takaro. The dedicated server ships no map image (its cook strips the pixels
// of every texture, including the in-game map), so the connector declares the protocol version 1
// `map` capability with a `none` source: the Dimension's axes and bounds come from the game's own
// map widget, and the server owner uploads the picture once in the Takaro dashboard.
//
// Calibration (UZoomUserWidget::ConvertMapLocationToWorldSpace and the ChamplainValley level's
// MapMakerMarker): the map is a square of OrthoWidth 1,200,000 cm centred on the world origin,
// with north = +X and east = +Y. On screen that is horizontal = y growing right and vertical = x
// growing up, both from -600,000 to 600,000.
#include <string>

namespace NativeMap {

constexpr const char* kDimensionId = "champlain-valley";

// The `map` capability entry ({"name":"map","parameters":{"dimensions":[...]}}) as JSON.
std::string CapabilityJson();

// Protocol version 0 MapInfoDTO: always `enabled:false`, because version 0 has no way to say
// "the operator supplies the picture". Takaro's version 0 operator upload still works.
std::string InfoJson();

}  // namespace NativeMap
