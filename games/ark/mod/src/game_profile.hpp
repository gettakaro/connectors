#pragma once
// Game Profile (Generic protocol v1): who this connector is (identify `game` and
// `connector`), the `game.profile` capability and the icon served by getGameIcon.
#include "json.hpp"
#include "version.hpp"
#include "ws_client.hpp"

#include <openssl/evp.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace takaro {

inline constexpr const char* kGameSlug = "ark";
inline constexpr const char* kConnectorName = "takaro-ark";
inline constexpr const char* kConnectorVersion = TAKARO_ARK_VERSION;
inline constexpr const char* kGameName = "ARK: Survival Evolved";
inline constexpr size_t kIconMaxBytes = 128 * 1024;
inline constexpr unsigned kIconSide = 256;

// The shipped icon, read once at startup: the revision always names the bytes served.
struct GameIcon {
  std::string png;
  std::string revision;  // first 12 hex chars of the PNG's sha256
};

inline std::string sha256_hex(const std::string& bytes) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  if (!EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr)) return {};
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned i = 0; i < length; ++i) { out += hex[digest[i] >> 4]; out += hex[digest[i] & 15]; }
  return out;
}

// Takaro rejects (and never refetches) a revision whose bytes are not a 256x256 PNG of
// at most 128 KiB, so an icon that fails these checks is not declared at all.
inline std::optional<GameIcon> parse_game_icon(std::string png, std::string& error) {
  static const std::string signature("\x89PNG\r\n\x1a\n", 8);
  auto be32 = [&](size_t at) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(png[at])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(png[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(png[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(png[at + 3]));
  };
  if (png.empty() || png.size() > kIconMaxBytes) { error = "icon size " + std::to_string(png.size()); return std::nullopt; }
  if (png.size() < 24 || png.compare(0, 8, signature) != 0 || png.compare(12, 4, "IHDR") != 0) {
    error = "icon is not a PNG";
    return std::nullopt;
  }
  if (be32(16) != kIconSide || be32(20) != kIconSide) {
    error = "icon is " + std::to_string(be32(16)) + "x" + std::to_string(be32(20)) + ", not 256x256";
    return std::nullopt;
  }
  GameIcon icon;
  icon.revision = sha256_hex(png).substr(0, 12);
  if (icon.revision.size() != 12) { error = "icon digest failed"; return std::nullopt; }
  icon.png = std::move(png);
  return icon;
}

inline std::shared_ptr<const GameIcon> load_game_icon(const std::string& path, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { error = "icon file not found"; return nullptr; }
  std::string bytes;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && bytes.size() <= kIconMaxBytes) bytes.append(buf, n);
  std::fclose(f);
  auto icon = parse_game_icon(std::move(bytes), error);
  return icon ? std::make_shared<const GameIcon>(std::move(*icon)) : nullptr;
}

// `version` is the running game build ("" = unknown, omitted); `icon` may be null.
inline json::Value game_profile_capability(const std::string& version, const GameIcon* icon) {
  json::Value parameters = json::obj({{"name", json::str(kGameName)}});
  if (!version.empty()) parameters.set("version", json::str(version));
  if (icon)
    parameters.set("icon", json::obj({{"revision", json::str(icon->revision)},
                                      {"size", json::num(static_cast<double>(icon->png.size()))}}));
  return json::obj({{"name", json::str("game.profile")}, {"parameters", std::move(parameters)}});
}

// The getGameIcon response: the whole file, never chunked, under its own revision.
inline json::Value game_icon_response(const GameIcon& icon) {
  return json::obj({{"revision", json::str(icon.revision)},
                    {"data", json::str(ws::base64(reinterpret_cast<const unsigned char*>(icon.png.data()), icon.png.size()))}});
}

}  // namespace takaro
