#include "plugin_config.h"

#include <random>

namespace
{
IPluginSelf* g_self = nullptr;

std::string ReadString(const char* section, const char* key, const char* fallback)
{
    char buffer[1024] = {};
    if (g_self && g_self->config->ReadString(g_self, section, key, buffer, sizeof(buffer), fallback)) return buffer;
    return fallback;
}

std::string NewIdentityToken()
{
    static constexpr char Hex[] = "0123456789abcdef";
    std::random_device random;
    std::string token = "starrupture-";
    for (int index = 0; index < 32; ++index) token += Hex[random() & 15];
    return token;
}
}

namespace TakaroConfig
{
void Initialize(IPluginSelf* self)
{
    g_self = self;
    if (g_self) g_self->config->InitializeFromSchema(g_self, &SCHEMA);
}

bool IsEnabled() { return g_self ? g_self->config->ReadBool(g_self, "General", "Enabled", true) : false; }
std::string WsUrl() { return ReadString("Takaro", "WsUrl", "wss://connect.takaro.io/"); }
std::string RegistrationToken() { return ReadString("Takaro", "RegistrationToken", ""); }
std::string ServerName() { return ReadString("Takaro", "ServerName", "StarRupture Server"); }

std::string IdentityToken()
{
    auto token = ReadString("Takaro", "IdentityToken", "");
    if (!token.empty() || !g_self) return token;
    token = NewIdentityToken();
    g_self->config->WriteString(g_self, "Takaro", "IdentityToken", token.c_str());
    return token;
}
}
