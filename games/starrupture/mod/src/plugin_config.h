#pragma once

#include <string>
#include "plugin_interface.h"

namespace TakaroConfig
{
static const ConfigEntry CONFIG_ENTRIES[] = {
    {"General", "Enabled", ConfigValueType::Boolean, "true", "Enable the Takaro connector", 0.0f, 1.0f},
    {"Takaro", "RegistrationToken", ConfigValueType::String, "", "Registration token from your Takaro domain settings", 0.0f, 0.0f},
    {"Takaro", "ServerName", ConfigValueType::String, "StarRupture Server", "Name shown in Takaro", 0.0f, 0.0f},
    {"Takaro", "IdentityToken", ConfigValueType::String, "", "Identifies this server in Takaro; filled in on first start, keep it stable", 0.0f, 0.0f},
    {"Takaro", "WsUrl", ConfigValueType::String, "wss://connect.takaro.io/", "Takaro connection URL", 0.0f, 0.0f},
};

static const ConfigSchema SCHEMA = {
    CONFIG_ENTRIES,
    sizeof(CONFIG_ENTRIES) / sizeof(ConfigEntry)
};

void Initialize(IPluginSelf* self);
bool IsEnabled();
std::string WsUrl();
std::string RegistrationToken();
std::string ServerName();
// Generated and saved on first start, so a server keeps its record in Takaro.
std::string IdentityToken();
}
