// The places listLocations answers with, as a player finds them on the in-game map.
//
// kLocations (gamedata.cpp) is every map-marker and spawn-point placement in the server's world
// data, 1031 rows. Many of them share one code: the 53 fast travel platforms are all
// Teleport_Platform_Gameplay, the 47 Flame Shrines a handful of codes. Takaro keys a location by
// its code, so rows that share one would collapse into a single marker; LocationRows gives each
// placement of a shared code its own code, `<code>@<x>_<z>` (metres, rounded), which stays the
// same as long as the placement does. A code used once is kept as it is.
//
// The name is the client's own map label (kLocationLabels, baked by tools/gen_location_labels.py
// from the En_Us localisation): 8kMapLabel_steppes_areaName_02_TwinPeaks is "Pillars of
// Creation". A placement the client gives no map name (respawn save points, boss braziers,
// hidden quest search areas) is not a place a player can find, so it is left out, and so are the
// fast travel platforms inside the Hollow Halls dungeons, which the world map does not show.
//
// Pure lookups over static tables: no Windows, no game memory. tests/location_test.cpp links
// this on the host.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

struct LocationDef;

struct LocationLabel {
    const char* code;  // LocationDef.code
    const char* name;  // En_Us map name
};

extern const LocationLabel kLocationLabels[];  // sorted by code (byte order)
extern const size_t kLocationLabelCount;

// The client's map name for a placement code, or nullptr when the game shows it under no name.
const char* LocationLabelByCode(const char* code);

struct LocationRow {
    const LocationDef* def;
    std::string code;  // unique across the rows
    const char* name;  // never empty
};

// The rows listLocations answers with, in kLocations order.
std::vector<LocationRow> LocationRows(const LocationDef* defs, size_t count);
