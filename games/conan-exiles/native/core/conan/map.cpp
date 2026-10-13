#include "conan/map.h"

#include "takaro/json_util.h"

namespace conan {

using takaro::ObjBuilder;

namespace {
// Values as cooked in RegionDataTable (server build 25792439, rows ExiledLands and IsleOfSiptah).
// Both maps are 4000 x 4000 textures: T_FullscreenMap decodes to 4000 x 4000 DXT1, and
// T_Map_Siptah has the same encoded size (8,000,000 bytes of DXT1 plus its header).
const MapCalibration kMaps[] = {
    {"ExiledLands", "exiled_lands", "The Exiled Lands", "/Game/UI/Textures/GUIs/Map/T_FullscreenMap",
     {137100, 177000}, {-260255, 28025}, {2348, 3058}, {404, 2326}, 4000, 4000},
    {"IsleOfSiptah", "isle_of_siptah", "Isle of Siptah", "/Game/DLC/DLC_Siptah/UI/Map/T_Map_Siptah",
     {1899933, -137623}, {1231457, 238562}, {3495, 1165}, {540, 2835}, 4000, 4000},
};

std::string Range(double lo, double hi) { return ObjBuilder().N("min", lo).N("max", hi).Done(); }

std::string Axis(const char* field, const char* direction) {
    return ObjBuilder().S("field", field).S("direction", direction).Done();
}

// Rounded to 0.1 world units (1 mm): the reference points are whole units.
double Tenth(double v) { return (double)(long long)(v * 10 + (v < 0 ? -0.5 : 0.5)) / 10; }
}  // namespace

const MapCalibration* MapForRegion(const std::string& region) {
    for (auto& m : kMaps)
        if (region == m.region) return &m;
    return nullptr;
}

MapBounds BoundsOf(const MapCalibration& m) {
    double perPxX = (m.neWorld[0] - m.swWorld[0]) / (m.neImage[0] - m.swImage[0]);
    double perPxY = (m.neWorld[1] - m.swWorld[1]) / (m.neImage[1] - m.swImage[1]);
    double minX = m.swWorld[0] - m.swImage[0] * perPxX, minY = m.swWorld[1] - m.swImage[1] * perPxY;
    return {Tenth(minX), Tenth(minX + m.textureWidth * perPxX), Tenth(minY), Tenth(minY + m.textureHeight * perPxY)};
}

std::string DimensionJson(const MapCalibration& m) {
    MapBounds b = BoundsOf(m);
    return ObjBuilder()
        .S("id", m.id)
        .S("name", m.name)
        .Raw("axes", ObjBuilder().Raw("horizontal", Axis("x", "right")).Raw("vertical", Axis("y", "down")).Done())
        .Raw("bounds", ObjBuilder().Raw("horizontal", Range(b.minX, b.maxX)).Raw("vertical", Range(b.minY, b.maxY)).Done())
        .Raw("source", "{\"type\":\"none\"}")
        .Done();
}

}  // namespace conan
