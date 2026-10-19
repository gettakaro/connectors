#include "conan/game_profile.h"

#include "takaro/json_util.h"

#include <cstdint>
#include <cstdio>

namespace conan {

namespace {

const unsigned char kIconPng[] = {
#include "conan/game_icon_png.inc"
};

// FIPS 180-4 SHA-256; only the icon revision needs it, so no crypto library is pulled in.
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void Block(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
};

}  // namespace

std::string Sha256Hex(const std::string& bytes) {
    Sha256 s;
    std::string msg = bytes;
    uint64_t bits = (uint64_t)bytes.size() * 8;
    msg += '\x80';
    while (msg.size() % 64 != 56) msg += '\0';
    for (int i = 7; i >= 0; i--) msg += (char)(bits >> (8 * i));
    for (size_t off = 0; off < msg.size(); off += 64) s.Block((const unsigned char*)msg.data() + off);
    char out[65];
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", s.h[i]);
    return std::string(out, 64);
}

std::string Base64(const std::string& bytes) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t n = (uint32_t)(unsigned char)bytes[i] << 16;
        if (i + 1 < bytes.size()) n |= (uint32_t)(unsigned char)bytes[i + 1] << 8;
        if (i + 2 < bytes.size()) n |= (unsigned char)bytes[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
    }
    return out;
}

const std::string& GameIconPng() {
    static const std::string png(reinterpret_cast<const char*>(kIconPng), sizeof(kIconPng));
    return png;
}

const std::string& GameIconRevision() {
    static const std::string revision = Sha256Hex(GameIconPng()).substr(0, 12);
    return revision;
}

std::string GameProfileCapability(const std::string& gameVersion) {
    std::string version = takaro::Trim(gameVersion);
    bool printable = !version.empty() && version.size() <= 32;
    for (char c : version)
        if (c < 0x20 || c > 0x7e) printable = false;
    takaro::ObjBuilder params;
    params.S("name", kGameDisplayName);
    if (printable) params.S("version", version);
    params.Raw("icon", takaro::ObjBuilder().S("revision", GameIconRevision()).N("size", (double)GameIconPng().size()).Done());
    return takaro::ObjBuilder().S("name", "game.profile").Raw("parameters", params.Done()).Done();
}

JsonValue GameIconPayload() {
    JsonValue p = takaro::JObj();
    takaro::Put(p, "revision", takaro::JStr(GameIconRevision()));
    takaro::Put(p, "data", takaro::JStr(Base64(GameIconPng())));
    return p;
}

}  // namespace conan
