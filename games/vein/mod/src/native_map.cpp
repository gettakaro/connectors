#include "native_map.h"

#include <nlohmann/json.hpp>

namespace NativeMap {
namespace {
using Json = nlohmann::json;

constexpr double kHalfWidth = 600000;  // MapMakerMarker OrthoWidth 1,200,000 cm / 2
}  // namespace

std::string CapabilityJson() {
    const Json range = {{"min", -kHalfWidth}, {"max", kHalfWidth}};
    const Json dimension = {
        {"id", kDimensionId},
        {"name", "Champlain Valley"},
        {"axes", {{"horizontal", {{"field", "y"}, {"direction", "right"}}},
                  {"vertical", {{"field", "x"}, {"direction", "up"}}}}},
        {"bounds", {{"horizontal", range}, {"vertical", range}}},
        {"source", {{"type", "none"}}}};
    return Json{{"name", "map"}, {"parameters", {{"dimensions", Json::array({dimension})}}}}.dump();
}

std::string InfoJson() {
    return Json{{"enabled", false}, {"mapBlockSize", 128}, {"maxZoom", 0},
                {"mapSizeX", 0}, {"mapSizeY", 0}, {"mapSizeZ", 0}}
        .dump();
}

}  // namespace NativeMap
