// getMapInfo / getMapTile from an on-disk tile pack (src/native/maptiles.cpp), directly and through
// ExecuteAction so the action names and the payload shapes Takaro receives are pinned.
#include "fake_plugin.h"
#include "native/adapter.h"
#include "native/config.h"
#include "native/fileio.h"
#include "native/maptiles.h"
#include "testlib.h"

#include <map>
#include <sys/stat.h>

using namespace native;

namespace t {
std::string TempDir(const std::string& tag);
void Group(const char* name);
}  // namespace t

namespace {

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "tile-body";

void Write(const std::string& path, const std::string& data) {
    std::string err;
    EnsureDirectory(DirName(path), err);
    CHECK_MSG(AtomicWriteFile(path, data, err), path + ": " + err);
}

ActionOutcome Run(const std::string& mapDir, const std::string& action, const char* args) {
    t::FakePlugin fake;
    ActionView view;
    view.mapDir = mapDir;
    JsonValue a = t::J(args);
    return ExecuteAction(fake, action, a, view, 0);
}

const char* kManifest =
    R"({"format":"takaro-map-tiles/1","tileSize":128,"minZoom":0,"maxZoom":4,"mapSizeX":10240,"mapSizeY":4096,"mapSizeZ":10240})";

void Base64Tests() {
    t::Group("maptiles base64");
    CHECK_EQ(Base64Encode(""), std::string(""));
    CHECK_EQ(Base64Encode("f"), std::string("Zg=="));
    CHECK_EQ(Base64Encode("fo"), std::string("Zm8="));
    CHECK_EQ(Base64Encode("foo"), std::string("Zm9v"));
    CHECK_EQ(Base64Encode("foob"), std::string("Zm9vYg=="));
    CHECK_EQ(Base64Encode("fooba"), std::string("Zm9vYmE="));
    CHECK_EQ(Base64Encode("foobar"), std::string("Zm9vYmFy"));
    CHECK_EQ(Base64Encode(std::string("\xff\x00\x80", 3)), std::string("/wCA"));
}

void NoPackTests() {
    t::Group("maptiles no pack");
    std::string dir = t::TempDir("map-none");
    ActionOutcome info = Run(dir, "getMapInfo", "{}");
    CHECK(info.ok);
    CHECK(info.payload.get("enabled") && info.payload.get("enabled")->type == JsonValue::Bool &&
          !info.payload.get("enabled")->b);  // a real boolean: Takaro's MapInfoDTO rejects anything else
    CHECK(Num(info.payload.get("maxZoom")) && *Num(info.payload.get("maxZoom")) == 4);
    CHECK(Num(info.payload.get("mapSizeX")) && *Num(info.payload.get("mapSizeX")) == 0);
    ActionOutcome tile = Run(dir, "getMapTile", R"({"x":0,"y":0,"z":0})");
    CHECK(!tile.ok);
    CHECK_MSG(tile.error.find("no map tile pack installed") != std::string::npos, tile.error);

    // a pack without `current` is not served: the pointer is what makes a pack live
    Write(JoinPath(dir, "map.json"), kManifest);
    CHECK(!LoadMapTilePack(dir).ok);
    Write(JoinPath(dir, "current"), "../elsewhere\n");
    CHECK(!LoadMapTilePack(dir).ok);
    CHECK(LoadMapTilePack(dir).reason.find("must name a pack folder") != std::string::npos);
    Write(JoinPath(dir, "current"), "r1\n");
    CHECK(!LoadMapTilePack(dir).ok);
    CHECK_MSG(LoadMapTilePack(dir).reason.find("pack r1 is incomplete") != std::string::npos, LoadMapTilePack(dir).reason);
    std::string pack = JoinPath(dir, "r1");

    Write(JoinPath(pack, "map.json"), R"({"format":"other","tileSize":128,"maxZoom":4,"mapSizeX":1,"mapSizeZ":1})");
    CHECK(!LoadMapTilePack(dir).ok);
    CHECK(LoadMapTilePack(dir).reason.find("format") != std::string::npos);
    Write(JoinPath(pack, "map.json"), R"({"format":"takaro-map-tiles/1","tileSize":128,"maxZoom":4,"mapSizeX":0,"mapSizeZ":1})");
    CHECK(!LoadMapTilePack(dir).ok);
    Write(JoinPath(pack, "map.json"), "not json");
    CHECK(!LoadMapTilePack(dir).ok);
    Write(JoinPath(pack, "map.json"),
          R"({"format":"takaro-map-tiles/1","tileSize":128,"maxZoom":4,"mapSizeX":1,"mapSizeZ":1,"outsideTile":"../x.png"})");
    CHECK(!LoadMapTilePack(dir).ok);  // the outside tile must stay inside the pack
    CHECK(!LoadMapTilePack("").ok);
}

void PackTests() {
    t::Group("maptiles pack");
    std::string dir = t::TempDir("map-pack"), pack = JoinPath(dir, "pack-1");
    Write(JoinPath(pack, "map.json"), kManifest);
    Write(pack + "/4/40/41.png", kPng);
    Write(pack + "/0/0/0.png", kPng + "z0");
    Write(pack + "/4/1/1.png", "GIF89a not a png");
    Write(JoinPath(dir, "current"), "pack-1");

    ActionOutcome info = Run(dir, "getMapInfo", "[]");
    CHECK(info.ok);
    CHECK(info.payload.get("enabled") && info.payload.get("enabled")->b);
    CHECK(Num(info.payload.get("maxZoom")) && *Num(info.payload.get("maxZoom")) == 4);
    CHECK(Num(info.payload.get("mapBlockSize")) && *Num(info.payload.get("mapBlockSize")) == 128);
    CHECK(Num(info.payload.get("mapSizeX")) && *Num(info.payload.get("mapSizeX")) == 10240);
    CHECK(Num(info.payload.get("mapSizeY")) && *Num(info.payload.get("mapSizeY")) == 4096);
    CHECK(Num(info.payload.get("mapSizeZ")) && *Num(info.payload.get("mapSizeZ")) == 10240);

    ActionOutcome tile = Run(dir, "getMapTile", R"({"x":40,"y":41,"z":4})");
    CHECK(tile.ok);
    CHECK(tile.payload.type == JsonValue::String);
    CHECK_EQ(tile.payload.str, Base64Encode(kPng));
    // numeric strings and the "zoom" alias, as Takaro and modules may send them
    tile = Run(dir, "getMapTile", R"({"x":"0","y":"0","zoom":"0"})");
    CHECK(tile.ok && tile.payload.str == Base64Encode(kPng + "z0"));

    tile = Run(dir, "getMapTile", R"({"x":5,"y":5,"z":4})");
    CHECK(!tile.ok);
    CHECK_MSG(tile.error.find("outside the map") != std::string::npos, tile.error);
    tile = Run(dir, "getMapTile", R"({"x":0,"y":0,"z":5})");
    CHECK(!tile.ok && tile.error.find("zoom 5 is outside 0-4") != std::string::npos);
    tile = Run(dir, "getMapTile", R"({"x":0.5,"y":0,"z":4})");
    CHECK(!tile.ok && tile.error.find("integer x, y and z") != std::string::npos);
    tile = Run(dir, "getMapTile", R"({"x":0,"y":0})");
    CHECK(!tile.ok);
    tile = Run(dir, "getMapTile", R"({"x":1,"y":1,"z":4})");
    CHECK(!tile.ok && tile.error.find("not a PNG") != std::string::npos);

    // an outside tile answers in-range tiles that are not in the pack (beyond the world edge)
    Write(JoinPath(pack, "map.json"),
          R"({"format":"takaro-map-tiles/1","tileSize":128,"maxZoom":4,"mapSizeX":10240,"mapSizeZ":10240,"outsideTile":"outside.png"})");
    Write(JoinPath(pack, "outside.png"), kPng + "outside");
    tile = Run(dir, "getMapTile", R"({"x":-3,"y":-1,"z":2})");
    CHECK(tile.ok && tile.payload.str == Base64Encode(kPng + "outside"));
    tile = Run(dir, "getMapTile", R"({"x":40,"y":41,"z":4})");
    CHECK(tile.ok && tile.payload.str == Base64Encode(kPng));
    tile = Run(dir, "getMapTile", R"({"x":0,"y":0,"z":9})");
    CHECK(!tile.ok);  // zoom limits still apply

    std::string big(kMaxMapTileBytes + 1, 'x');
    big.replace(0, 8, kPng.substr(0, 8));
    Write(pack + "/3/0/0.png", big);
    tile = Run(dir, "getMapTile", R"({"x":0,"y":0,"z":3})");
    CHECK(!tile.ok && tile.error.find("larger than") != std::string::npos);
}

// A re-render lands in its own folder while the old pack keeps serving; switching `current` moves every
// tile to the new render at once, and a half-copied new pack is never served.
void SwapTests() {
    t::Group("maptiles pack swap");
    std::string dir = t::TempDir("map-swap");
    auto tile = [&](const char* args) {
        ActionOutcome o = Run(dir, "getMapTile", args);
        return o.ok ? o.payload.str : "error: " + o.error;
    };
    for (const char* tp : {"0/0/0", "4/39/45", "4/40/45"}) Write(dir + "/old/" + tp + ".png", kPng + "old");
    Write(dir + "/old/map.json", kManifest);
    Write(JoinPath(dir, "current"), "old\n");
    // the new render is half copied: some tiles there, no map.json yet
    Write(dir + "/new/4/39/45.png", kPng + "new");
    CHECK_EQ(tile(R"({"x":39,"y":45,"z":4})"), Base64Encode(kPng + "old"));
    CHECK_EQ(tile(R"({"x":40,"y":45,"z":4})"), Base64Encode(kPng + "old"));
    for (const char* tp : {"0/0/0", "4/40/45"}) Write(dir + "/new/" + tp + ".png", kPng + "new");
    Write(dir + "/new/map.json", kManifest);
    CHECK_EQ(tile(R"({"x":39,"y":45,"z":4})"), Base64Encode(kPng + "old"));  // not live until `current` says so
    Write(JoinPath(dir, "current"), "new");
    CHECK_EQ(tile(R"({"x":0,"y":0,"z":0})"), Base64Encode(kPng + "new"));
    CHECK_EQ(tile(R"({"x":39,"y":45,"z":4})"), Base64Encode(kPng + "new"));
    CHECK_EQ(tile(R"({"x":40,"y":45,"z":4})"), Base64Encode(kPng + "new"));
    // a tile the new pack lacks is outside the map, never the old pack's tile
    Write(dir + "/old/4/41/45.png", kPng + "old");
    CHECK(tile(R"({"x":41,"y":45,"z":4})").find("outside the map") != std::string::npos);
    // a pointer to a folder that is not finished is refused, not served half-empty
    Write(JoinPath(dir, "current"), "missing");
    CHECK(tile(R"({"x":0,"y":0,"z":0})").find("pack missing is incomplete") != std::string::npos);
    CHECK(!Run(dir, "getMapInfo", "{}").payload.get("enabled")->b);
}

void ConfigTests() {
    t::Group("maptiles config");
    auto env = [](std::map<std::string, std::string> m) {
        return EnvFn([m](const char* k) {
            auto it = m.find(k);
            return it == m.end() ? std::string() : it->second;
        });
    };
    NativeConfig c = LoadNativeConfig("Z:\\srv", env({}), "");
    CHECK_EQ(c.mapDir, std::string("Z:\\srv\\takaro\\map"));
    c = LoadNativeConfig("Z:\\srv", env({}), R"({"mapDir":"maps\\embervale"})");
    CHECK_EQ(c.mapDir, std::string("Z:\\srv\\maps\\embervale"));
    c = LoadNativeConfig("Z:\\srv", env({{"TAKARO_MAP_DIR", "D:\\tiles"}}), R"({"mapDir":"x"})");
    CHECK_EQ(c.mapDir, std::string("D:\\tiles"));
    CHECK(ConfigSummaryJson(c).find("\"mapDir\":\"D:\\\\tiles\"") != std::string::npos);
}

}  // namespace

void RunMapTileTests() {
    Base64Tests();
    NoPackTests();
    PackTests();
    SwapTests();
    ConfigTests();
}
