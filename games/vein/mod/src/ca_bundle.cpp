#include "ca_bundle.h"

#include <unistd.h>

namespace CaBundle {

const std::vector<std::string>& Candidates() {
    static const std::vector<std::string>& c = *new std::vector<std::string>{
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
    return c;
}

std::string Choose(const std::string& configured, const std::function<bool(const std::string&)>& readable) {
    if (!configured.empty()) return configured;
    for (const auto& path : Candidates()) {
        bool ok = readable ? readable(path) : access(path.c_str(), R_OK) == 0;
        if (ok) return path;
    }
    return "";
}

}  // namespace CaBundle
