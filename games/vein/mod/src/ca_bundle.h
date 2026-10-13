// Where to find trusted CA certificates on the host. Distributions keep the bundle in different
// places; the connector must not depend on the Debian path alone.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace CaBundle {

// Debian/Ubuntu, RHEL/Fedora/Alma, openSUSE, RHEL extracted, Alpine/Arch/macOS-style.
const std::vector<std::string>& Candidates();

// TAKARO_CA_FILE (`configured`) when set, otherwise the first candidate `readable` accepts, otherwise
// "". `readable` defaults to "the file opens".
std::string Choose(const std::string& configured,
                   const std::function<bool(const std::string&)>& readable = nullptr);

}  // namespace CaBundle
