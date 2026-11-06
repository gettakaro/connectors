#pragma once

#include "plugin_interface.h"

IPluginSelf* GetSelf();

inline IPluginHooks* GetHooks()
{
    auto* self = GetSelf();
    return self ? self->hooks : nullptr;
}

#define TAKARO_LOG_INFO(format, ...)  if (auto s = GetSelf()) s->logger->Info(s, format, ##__VA_ARGS__)
#define TAKARO_LOG_WARN(format, ...)  if (auto s = GetSelf()) s->logger->Warn(s, format, ##__VA_ARGS__)
#define TAKARO_LOG_ERROR(format, ...) if (auto s = GetSelf()) s->logger->Error(s, format, ##__VA_ARGS__)
