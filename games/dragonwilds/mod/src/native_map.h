#pragma once

// Dragonwilds' world map for Takaro protocol version 1. The dedicated server ships no map art (its
// cook strips the pixels of the world-map textures), so the connector declares the `map`
// capability with a `none` source: Takaro gets the Dimension's axes and bounds, and the server
// owner uploads the picture once in the dashboard (DEVELOPMENT.md, "World map").
//
// Calibration: the full-world survey of L_World (mod/src/mapsurvey.cpp, rendered by
// scripts/render-map.py) covers x -20,000..600,100 and y -160,000..480,050 in Unreal units at
// 150 per pixel, viewed from above with north up: x grows to the right and y grows downwards.
// These are the units of every player position the connector reports, so an image rendered by
// render-map.py lines up with no manual settings.
#include <nlohmann/json.hpp>
#include <string>

namespace NativeMap {

constexpr const char* kDimensionId = "overworld";
constexpr double kMinX = -20000, kMaxX = 600100;
constexpr double kMinY = -160000, kMaxY = 480050;

// The `map` capability entry ({"name":"map","parameters":{"dimensions":[...]}}) as JSON.
inline std::string CapabilityJson() {
    using Json = nlohmann::json;
    const Json dimension = {
        {"id", kDimensionId},
        {"name", "Dragonwilds"},
        {"axes", {{"horizontal", {{"field", "x"}, {"direction", "right"}}},
                  {"vertical", {{"field", "y"}, {"direction", "down"}}}}},
        {"bounds", {{"horizontal", {{"min", kMinX}, {"max", kMaxX}}},
                    {"vertical", {{"min", kMinY}, {"max", kMaxY}}}}},
        {"source", {{"type", "none"}}}};
    return Json{{"name", "map"}, {"parameters", {{"dimensions", Json::array({dimension})}}}}.dump();
}

}  // namespace NativeMap
