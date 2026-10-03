#include "conan/coverage.h"

#include "common.h"

namespace conan {

namespace {
// Generated from capabilities.json; keep both in step (tests/drift_test.py).
const Coverage kActions[] = {
    {"getPlayer", "pending", "pending", "public Takaro player DTO or null", "MCP gameserverGetPlayers then the player, with the client online", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: PlayerArray -> PlayerState -> controller (read path proven in stage 1)."},
    {"getPlayers", "pending", "pending", "public Takaro player DTO array", "MCP gameserverGetPlayers with the client online", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: PlayerArray -> PlayerState -> controller, plus ping and IP."},
    {"getPlayerLocation", "pending", "pending", "{ x: number, y: number, z: number }", "MCP location vs client coordinates", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: K2_GetActorLocation through ProcessEvent."},
    {"getPlayerInventory", "pending", "pending", "Takaro item DTO array", "MCP inventory vs the client inventory screen", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: UItemInventory by reflection (no sqlite)."},
    {"giveItem", "pending", "pending", "{}", "MCP plus the item in the client", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: CheatSpawnItem or the in-process console executor."},
    {"listItems", "pending", "pending", "Takaro item DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: ItemTable rows with localized names."},
    {"listEntities", "pending", "pending", "Takaro entity DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: class walk with a humaniser."},
    {"listLocations", "pending", "pending", "Takaro location DTO array", "MCP sample", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"executeConsoleCommand", "pending", "pending", "{ success: boolean, rawResult: string }", "MCP rawResult", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: the engine's own console executor in-process (never RCON)."},
    {"sendMessage", "live-supported", "native", "{}", "Live 2026-10-03 on rig 349 (bridge stopped): MCP gameserverSendMessage global and directed (opts.recipient.gameId + senderNameOverride) -> native log delivered=1 each, both lines in the client chat (El-Limon evidence native-stage2/L1)", "ClientReceiveChatMessage on each online ConanPlayerController through ProcessEvent on the game thread; global, or directed by Steam64 / character name."},
    {"teleportPlayer", "pending", "pending", "{}", "MCP plus the client position", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"testReachability", "live-supported", "native", "{ connectable: boolean, reason: string | null }", "Live 2026-10-03: MCP gameserverTestReachabilityForId -> {connectable:true, reason:null} from the native library with the bridge stopped; unknown-build path covered by tests/so_test.py", "In-process: connectable when the server build is pinned and the ProcessEvent hook is installed; an unknown build answers connectable=false with the reason."},
    {"kickPlayer", "pending", "pending", "{}", "client disconnected", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"banPlayer", "pending", "pending", "{}", "rejoin refused, blacklist.txt", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"unbanPlayer", "pending", "pending", "{}", "rejoin allowed, blacklist.txt", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"listBans", "pending", "pending", "Takaro ban DTO array", "MCP gameserverListBans", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"shutdown", "pending", "pending", "{}", "countdown in the client plus process exit", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: Conan's own admin shutdown with its countdown."},
    {"getMapInfo", "schema-fallback", "native", "{ enabled: false, mapBlockSize: 0, maxZoom: 0, mapSizeX: 0, mapSizeY: 0, mapSizeZ: 0 }", "MCP gameserverGetMapInfo", "Hosted Takaro shows no map tiles for Generic servers; answers the disabled MapInfo DTO Takaro validates."},
    {"getMapTile", "unsupported", "native", "structured error", "MCP gameserverGetMapTile returns the structured error", "Hosted Takaro shows no map tiles for Generic servers; no tile renderer exists for Conan."},
};
const Coverage kEvents[] = {
    {"player-connected", "pending", "pending", "{ player }", "eventSearch on a real join", "Not ported to the native connector yet (stage 2 Phase 2); nothing is emitted for it. Planned: K2_PostLogin or a PlayerArray diff."},
    {"player-disconnected", "pending", "pending", "{ player }", "eventSearch on a real leave", "Not ported to the native connector yet (stage 2 Phase 2); nothing is emitted for it. Planned: K2_OnLogout or a PlayerArray diff."},
    {"chat-message", "pending", "pending", "{ player, msg, channel }", "eventSearch plus the Discord post", "Not ported to the native connector yet (stage 2 Phase 2); nothing is emitted for it. Planned: ServerSendChatMessage in the detour (proven hook point)."},
    {"player-death", "pending", "pending", "{ player, attacker?, position? }", "real death in the client", "Not ported to the native connector yet (stage 2 Phase 2); nothing is emitted for it."},
    {"entity-killed", "pending", "pending", "{ player, entity, weapon }", "real kill in the client", "Not ported to the native connector yet (stage 2 Phase 2); nothing is emitted for it."},
    {"log", "pending", "native", "{ msg }", "eventSearch for the critical notice", "Today only the connector's own critical notices (an unsupported server build) are sent as log events; the in-process server log tail is Phase 2."},
};

std::string Section(const Coverage* rows, size_t n, const char* shapeKey) {
    std::string o = "{";
    for (size_t i = 0; i < n; i++) {
        const Coverage& c = rows[i];
        o += std::string(i ? "," : "") + JsonStr(c.name) + ":{\"status\":" + JsonStr(c.status) +
             ",\"implementation\":" + JsonStr(c.implementation) + ",\"" + shapeKey + "\":" + JsonStr(c.shape) +
             ",\"verification\":" + JsonStr(c.verification) + ",\"reason\":" + JsonStr(c.reason) + "}";
    }
    return o + "}";
}
}  // namespace

const Coverage* ActionCoverage(const std::string& action) {
    for (auto& c : kActions)
        if (action == c.name) return &c;
    return nullptr;
}

const Coverage* EventCoverage(const std::string& type) {
    for (auto& c : kEvents)
        if (type == c.name) return &c;
    return nullptr;
}

std::string RegistryJson() {
    return "{\"functions\":" + Section(kActions, sizeof kActions / sizeof kActions[0], "responseShape") +
           ",\"events\":" + Section(kEvents, sizeof kEvents / sizeof kEvents[0], "payloadShape") + "}";
}

}  // namespace conan
