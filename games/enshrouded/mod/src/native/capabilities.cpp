#include "native/capabilities.h"

#include "native/json_util.h"

namespace native {

namespace {

const char* const kActionCapabilities[] = {
    "players.list",  "players.get",   "players.location", "players.inventory",  "players.teleport",
    "items.list",    "items.give",    "entities.list",    "locations.list",     "chat.send",
    "console.execute", "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans",
    "server.shutdown"};
const char* const kEventCapabilities[] = {"events.player-connected", "events.player-disconnected",
                                          "events.chat-message",     "events.player-death",
                                          "events.entity-killed",    "events.log"};

std::string Range(double min, double max) { return ObjBuilder().N("min", min).N("max", max).Done(); }

std::string MapDimensionJson() {
    return ObjBuilder()
        .S("id", kMapDimensionId)
        .S("name", kMapDimensionName)
        .Raw("axes", ObjBuilder()
                         .Raw("horizontal", ObjBuilder().S("field", "x").S("direction", "right").Done())
                         .Raw("vertical", ObjBuilder().S("field", "z").S("direction", "up").Done())
                         .Done())
        .Raw("bounds", ObjBuilder()
                           .Raw("horizontal", Range(kWorldMin, kWorldMax))
                           .Raw("vertical", Range(kWorldMin, kWorldMax))
                           .Done())
        .Raw("source", ObjBuilder().S("type", "none").Done())
        .Done();
}

}  // namespace

std::string CapabilityManifestJson() {
    std::string out = "[";
    for (const char* name : kActionCapabilities) out += ObjBuilder().S("name", name).Done() + ",";
    for (const char* name : kEventCapabilities) out += ObjBuilder().S("name", name).Done() + ",";
    out += ObjBuilder()
               .S("name", "map")
               .Raw("parameters", ObjBuilder().Raw("dimensions", "[" + MapDimensionJson() + "]").Done())
               .Done();
    return out + "]";
}

}  // namespace native
