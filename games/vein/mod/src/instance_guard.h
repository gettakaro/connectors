// One connector per game process. Hosts can end up loading two copies, for example LD_PRELOAD plus
// the panel loader (libSDL3.so.0), and two copies would hook the game twice and open two Takaro
// connections with the same identity.
#pragma once

#include <string>
#include <vector>

namespace InstanceGuard {

// Paths of libtakaro-vein.so files mapped in `maps` (text of /proc/self/maps) other than `self`.
std::vector<std::string> OtherCopies(const std::string& maps, const std::string& self);
inline bool OtherCopyMapped(const std::string& maps, const std::string& self) { return !OtherCopies(maps, self).empty(); }

// Every copy since the panel loader exports this symbol and takes the process lock; a mapped copy
// without it is an older release that will run regardless, so a new copy must stay idle.
bool IsGuardedCopy(const std::string& path);

// Claims this process for the calling copy by binding an abstract unix socket named after the pid,
// held until exit. Returns false when another copy holds it. Fails open (true) when the socket
// cannot be created at all, so a restricted sandbox never disables the connector.
bool ClaimProcess(std::string* why = nullptr);

}  // namespace InstanceGuard
