#include "native/maptiles.h"

#include "native/fileio.h"
#include "native/json_util.h"
#include "native/mapping.h"

#include <cmath>
#include <cstdlib>

namespace native {

namespace {

const char* const kFormat = "takaro-map-tiles/1";
const char* const kPointer = "current";

bool ReadInt(const JsonValue* v, long long& out) {
    if (!v) return false;
    double d = 0;
    if (v->type == JsonValue::Number) {
        d = v->num;
    } else if (v->type == JsonValue::String) {
        std::string s = Trim(v->str);
        if (s.empty()) return false;
        char* end = nullptr;
        d = strtod(s.c_str(), &end);
        if (!end || *end) return false;
    } else {
        return false;
    }
    if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 1e9) return false;
    out = (long long)d;
    return true;
}

// A relative file name inside the pack: no separators, no "..".
bool SafeFileName(const std::string& s) {
    if (s.empty() || s == "." || s == "..") return false;
    for (char c : s)
        if (c == '/' || c == '\\' || c == ':') return false;
    return true;
}

std::string TilePath(const std::string& dir, long long z, long long x, long long y) {
    return JoinPath(JoinPath(JoinPath(dir, std::to_string(z)), std::to_string(x)), std::to_string(y) + ".png");
}

bool ReadTile(const std::string& path, std::string& bytes, std::string& err) {
    bool exists = false;
    if (!ReadWholeFile(path, bytes, exists, err)) return false;
    if (!exists) {
        err = "missing";
        return false;
    }
    if (bytes.size() > kMaxMapTileBytes) {
        err = "larger than " + std::to_string(kMaxMapTileBytes) + " bytes";
        return false;
    }
    if (bytes.size() < 8 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0) {
        err = "not a PNG file";
        return false;
    }
    return true;
}

}  // namespace

std::string Base64Encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

MapTilePack LoadMapTilePack(const std::string& mapDir) {
    MapTilePack p;
    if (mapDir.empty()) {
        p.reason = "no map tile directory is configured";
        return p;
    }
    std::string pointerPath = JoinPath(mapDir, kPointer);
    std::string text, err;
    bool exists = false;
    if (!ReadWholeFile(pointerPath, text, exists, err)) {
        p.reason = "cannot read " + pointerPath + ": " + err;
        return p;
    }
    if (!exists) {
        p.reason = "no map tile pack installed (" + pointerPath + " is missing)";
        return p;
    }
    std::string pack = Trim(text);
    if (!SafeFileName(pack)) {
        p.reason = pointerPath + " must name a pack folder inside " + mapDir;
        return p;
    }
    p.dir = JoinPath(mapDir, pack);
    std::string manifestPath = JoinPath(p.dir, "map.json");
    if (!ReadWholeFile(manifestPath, text, exists, err)) {
        p.reason = "cannot read " + manifestPath + ": " + err;
        return p;
    }
    if (!exists) {
        p.reason = "map tile pack " + pack + " is incomplete (" + manifestPath + " is missing)";
        return p;
    }
    JsonValue m;
    if (!ParseJson(text, m) || m.type != JsonValue::Object) {
        p.reason = manifestPath + " is not a JSON object";
        return p;
    }
    const JsonValue* fmt = m.get("format");
    if (!fmt || fmt->type != JsonValue::String || fmt->str != kFormat) {
        p.reason = manifestPath + " format must be \"" + kFormat + "\"";
        return p;
    }
    long long tileSize = 0, minZoom = 0, maxZoom = 0, sx = 0, sy = 0, sz = 0, block = 0;
    if (!ReadInt(m.get("tileSize"), tileSize) || tileSize < 16 || tileSize > 1024 ||
        !ReadInt(m.get("maxZoom"), maxZoom) || maxZoom < 0 || maxZoom > 12 ||
        !ReadInt(m.get("mapSizeX"), sx) || sx <= 0 || !ReadInt(m.get("mapSizeZ"), sz) || sz <= 0) {
        p.reason = manifestPath + " needs integer tileSize (16-1024), maxZoom (0-12), mapSizeX and mapSizeZ (>0)";
        return p;
    }
    if (m.get("minZoom") && (!ReadInt(m.get("minZoom"), minZoom) || minZoom < 0 || minZoom > maxZoom)) {
        p.reason = manifestPath + " minZoom must be an integer between 0 and maxZoom";
        return p;
    }
    if (m.get("mapSizeY") && (!ReadInt(m.get("mapSizeY"), sy) || sy < 0)) {
        p.reason = manifestPath + " mapSizeY must be a non-negative integer";
        return p;
    }
    block = tileSize;
    if (m.get("mapBlockSize") && (!ReadInt(m.get("mapBlockSize"), block) || block <= 0)) {
        p.reason = manifestPath + " mapBlockSize must be a positive integer";
        return p;
    }
    if (const JsonValue* o = m.get("outsideTile")) {
        if (o->type != JsonValue::String || !SafeFileName(o->str)) {
            p.reason = manifestPath + " outsideTile must be a file name inside the pack";
            return p;
        }
        p.outsideTile = o->str;
    }
    p.ok = true;
    p.tileSize = (int)tileSize;
    p.mapBlockSize = (int)block;
    p.minZoom = (int)minZoom;
    p.maxZoom = (int)maxZoom;
    p.mapSizeX = sx;
    p.mapSizeY = sy;
    p.mapSizeZ = sz;
    return p;
}

JsonValue MapInfoPayload(const std::string& mapDir) {
    MapTilePack p = LoadMapTilePack(mapDir);
    JsonValue o = JObj();
    Put(o, "enabled", JBool(p.ok));
    Put(o, "mapBlockSize", JNum(p.ok ? p.mapBlockSize : 128));
    Put(o, "maxZoom", JNum(p.ok ? p.maxZoom : 4));
    Put(o, "mapSizeX", JNum((double)p.mapSizeX));
    Put(o, "mapSizeY", JNum((double)p.mapSizeY));
    Put(o, "mapSizeZ", JNum((double)p.mapSizeZ));
    return o;
}

std::string MapTileBase64(const std::string& mapDir, const JsonValue& args) {
    long long x = 0, y = 0, z = 0;
    const JsonValue* zv = args.get("z") ? args.get("z") : args.get("zoom");
    if (!ReadInt(args.get("x"), x) || !ReadInt(args.get("y"), y) || !ReadInt(zv, z))
        Fail("getMapTile needs integer x, y and z");
    MapTilePack p = LoadMapTilePack(mapDir);
    if (!p.ok) FailCode("not_found", "Map tiles are not available: " + p.reason);
    if (z < p.minZoom || z > p.maxZoom)
        Fail("Map tile zoom " + std::to_string(z) + " is outside " + std::to_string(p.minZoom) + "-" +
             std::to_string(p.maxZoom));
    std::string bytes, err;
    std::string path = TilePath(p.dir, z, x, y);
    if (ReadTile(path, bytes, err)) return Base64Encode(bytes);
    if (err != "missing") FailCode("game_error", "Map tile " + path + " is unusable: " + err);
    if (!p.outsideTile.empty()) {
        std::string outside = JoinPath(p.dir, p.outsideTile);
        if (ReadTile(outside, bytes, err)) return Base64Encode(bytes);
        FailCode("game_error", "Map outside tile " + outside + " is unusable: " + err);
    }
    FailCode("not_found", "Map tile " + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) +
         " is outside the map");
}

}  // namespace native
