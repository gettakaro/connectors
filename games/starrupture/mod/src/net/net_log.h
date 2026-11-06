// Logging for the transport: routed to the ModLoader log. Never pass token values here.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <string>

void PluginLog(const char* fmt, ...);
