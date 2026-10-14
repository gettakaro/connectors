// File logger shared by the transport and the Lua side (takaro_log). Never pass token values here.
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

// Sets the log file (UTF-8 path). Until it is set, lines go to stderr and OutputDebugString only.
void LogSetFile(const std::string& path);
void LogLine(const std::string& line);
void PluginLog(const char* fmt, ...);
