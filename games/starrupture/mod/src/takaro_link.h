// The plugin's own connection to Takaro. Everything here runs on the transport's threads and one
// worker thread, never on the game thread.
#pragma once
#include <string>

struct TakaroLinkOptions
{
    std::string url;
    std::string identityToken;
    std::string registrationToken;
    std::string serverName;
    std::string caFile;
};

bool StartTakaroLink(const TakaroLinkOptions& options);
void StopTakaroLink();
