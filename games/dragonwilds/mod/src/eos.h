// Platform accounts linked to an EOS ProductUserId, read from EOS Connect inside the game process.
//
// A player's APlayerState only carries the EOS ProductUserId; the Steam (or Xbox) account behind it
// never reaches the server's game objects or log. The server's own EOS SDK can still look it up:
// EOS_Connect_QueryProductUserIdMappings with no local user is the dedicated-server form of the
// query. The plugin interposes EOS_Platform_Tick (the server binary imports it from
// libEOSSDK-Linux-Shipping.so, and the plugin is LD_PRELOADed ahead of it) only to learn the
// platform handle the game ticks; every other EOS call is resolved with dlsym.
#pragma once

#include <string>

namespace Eos {

struct Linked {
    std::string steamId;     // SteamID64, decimal
    std::string xboxLiveId;  // XUID, decimal
};

enum class Lookup {
    Unavailable,  // no EOS platform seen (yet), or the SDK lacks the functions
    Pending,      // a query is in flight or waiting for a retry
    Done,         // EOS answered; `out` holds whatever accounts are linked (maybe none)
};

// Game thread only. The first call for a PUID starts the lookup; answers are cached for the process
// lifetime (a PUID's linked accounts do not change while it is online).
Lookup LinkedAccounts(const std::string& puid, Linked& out);

std::string DiagnosticsJson();

}  // namespace Eos
