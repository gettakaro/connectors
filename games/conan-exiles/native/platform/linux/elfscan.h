// Linux identity of the server image: the GNU build-id, and the executable code regions of the
// main module as mapped in this process (/proc/self/maps), for the startup signature scan.
#pragma once

#include "pins/pins.h"

#include <string>
#include <vector>

namespace linuxplat {

// The hex GNU build-id of an ELF64 file, or "" when it has none or cannot be read.
std::string ReadElfBuildId(const std::string& path);

// Readable+executable mappings of `path` in /proc/self/maps (or `mapsText` when given, for tests).
std::vector<pins::Region> ExecutableRegions(const std::string& path, const std::string& mapsText = "");

}  // namespace linuxplat
