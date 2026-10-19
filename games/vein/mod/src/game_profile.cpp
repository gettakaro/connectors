#include "game_profile.h"

#include "common.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <cstdio>

namespace GameProfile {

const char* const kConnectorVersion = TAKARO_PLUGIN_VERSION;

namespace {
using Json = nlohmann::json;

const unsigned char kIconPng[] = {
#include "game_icon_png.inc"
};

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
}  // namespace

const std::string& IconPng() {
    static const std::string png(reinterpret_cast<const char*>(kIconPng), sizeof(kIconPng));
    return png;
}

const std::string& IconRevision() {
    static const std::string revision = [] {
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(IconPng().data()), IconPng().size(), digest);
        char hex[2 * SHA256_DIGEST_LENGTH + 1];
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        return std::string(hex, 12);
    }();
    return revision;
}

std::string ConnectorJson() {
    return Json{{"name", kConnectorName}, {"version", kConnectorVersion}}.dump();
}

std::string CapabilityJson(const std::string& gameVersion) {
    const std::string version = Trim(gameVersion);
    bool printable = !version.empty() && version.size() <= 32;
    for (char c : version)
        if (c < 0x20 || c > 0x7e) printable = false;
    Json params = {{"name", kGameDisplayName}};
    if (printable) params["version"] = version;
    params["icon"] = {{"revision", IconRevision()}, {"size", IconPng().size()}};
    return Json{{"name", "game.profile"}, {"parameters", params}}.dump();
}

std::string IconResponseJson() {
    const std::string& png = IconPng();
    std::string data(4 * ((png.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&data[0]),
                                  reinterpret_cast<const unsigned char*>(png.data()), (int)png.size());
    data.resize(n < 0 ? 0 : (size_t)n);
    return Json{{"revision", IconRevision()}, {"data", data}}.dump();
}

}  // namespace GameProfile
