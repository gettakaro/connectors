// Windows identity of the server image (stub interface; the Windows lane fills it in): the PE
// identity used as the pins build id (for example "<TimeDateStamp>-<SizeOfImage>" of
// ConanSandboxServer-Win64-Shipping.exe), and its executable sections as mapped in this process,
// for the startup signature scan in core/pins. Mirrors platform/linux/elfscan.h.
#pragma once

#include "pins/pins.h"

#include <string>
#include <vector>

namespace winplat {

std::string PeIdentity();                      // of the main module
std::vector<pins::Region> ExecutableRegions();  // .text (and any other executable section)

}  // namespace winplat
