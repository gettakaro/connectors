#include "net_log.h"

#include <cstdarg>
#include <cstdio>

#include "../plugin_helpers.h"

void PluginLog(const char* fmt, ...)
{
    char line[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    TAKARO_LOG_INFO("%s", line);
}
