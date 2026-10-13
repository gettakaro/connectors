// Map survey (debug only, TAKARO_PLUGIN_DEBUG=1): samples the world's collision from above with
// UWorld::LineTraceSingleByChannel on a regular grid and writes a height field that
// scripts/render-map.py turns into a top-down map image for Takaro's map upload.
//
// The dedicated server ships no map art (its cook strips texture pixels), so the only map the
// server can produce is one derived from its own collision. The survey only sees what is loaded:
// run it on a server started with World Partition server streaming off (see DEVELOPMENT.md).
//
// Output, under <PluginDataDir>/map/:
//   heights.f32  w*h little-endian float32, row-major from (minX, minY); NaN = no hit
//   normalz.f32  w*h float32, the Z of the impact normal (1 = flat); NaN = no hit
//   meta.json    {minX, minY, step, w, h, zTop, zBottom, channel, landscapes, hits, ms, buildId}
#pragma once
#include "common.h"

#include <cmath>

namespace MapSurvey {

// Grid geometry: sample (col, row) is world (minX + (col + 0.5) * step, minY + (row + 0.5) * step).
struct Grid {
    double minX = 0, minY = 0, step = 0;
    int w = 0, h = 0;
};

// Builds the grid that covers [minX, maxX] x [minY, maxY] with `step` world units per sample.
// Returns false (and leaves `out` untouched) for an empty box, a non-positive step or a grid larger
// than maxSide samples per side.
inline bool PlanGrid(double minX, double maxX, double minY, double maxY, double step, int maxSide, Grid& out) {
    if (!(step > 0) || !std::isfinite(minX) || !std::isfinite(maxX) || !std::isfinite(minY) ||
        !std::isfinite(maxY) || !(maxX > minX) || !(maxY > minY))
        return false;
    double w = std::ceil((maxX - minX) / step), h = std::ceil((maxY - minY) / step);
    if (w < 1 || h < 1 || w > maxSide || h > maxSide) return false;
    out.minX = minX;
    out.minY = minY;
    out.step = step;
    out.w = (int)w;
    out.h = (int)h;
    return true;
}

// POST /debug/map/survey {step?, perJob?, channel?, minX?, maxX?, minY?, maxY?, zTop?, zBottom?}
// Starts a survey on a background thread; 202 with the plan, 409 while one runs.
int Start(const JsonValue& body, std::string& outJson);
// GET /debug/map/survey: progress of the running or last survey.
std::string Status();

}  // namespace MapSurvey
