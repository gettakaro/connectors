#include "conan/coverage.h"

#include "common.h"

namespace conan {

namespace {
// Generated from capabilities.json; keep both in step (tests/drift_test.py).
const Coverage kActions[] = {
    {"getPlayer", "pending", "native", "public Takaro player DTO or null", "MCP getPlayer through gameserverGetPlayers / playerongameserverSearch (not yet run live)", "Same snapshot as getPlayers; an offline player answers the last record seen in this process, else a minimal record from the requested id (Takaro rejects every no-player answer)."},
    {"getPlayers", "pending", "native", "public Takaro player DTO array", "MCP gameserverGetPlayers with the client online (not yet run live)", "Worker-thread safe self-reads (process_vm_readv): GameStateBase.PlayerArray -> PlayerState -> Owner controller; Steam64 from UserIDFromURLOptions, name PlayerNamePrivate, ip SavedNetworkAddress, ping ExactPing (PlayerState+824, checked once against GetPingInMilliseconds). No RCON."},
    {"getPlayerLocation", "pending", "native", "{ x: number, y: number, z: number }", "MCP location vs client coordinates (not yet run live)", "Worker read: Controller.Pawn -> RootComponent -> RelativeLocation, or ComponentToWorld (+0x210, checked against K2_GetActorLocation) when attached to a mount; the last known position while dead/offline."},
    {"getPlayerInventory", "pending", "native", "Takaro item DTO array", "MCP inventory vs the client inventory screen (not yet run live)", "Worker read of the backpack, hotbar and equipment ItemInventory lists (torn-read checked, game-thread copy as fallback); stack and durability from the GameItem stat arrays (checked against GetIntStat/GetFloatStat); fist templates 51204/51205 dropped; amounts summed per code and durability %."},
    {"giveItem", "pending", "pending", "{}", "MCP plus the item in the client", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: CheatSpawnItem or the in-process console executor."},
    {"listItems", "pending", "native", "Takaro item DTO array with display names", "MCP itemSearch sample has display names and zero class names (not yet run live)", "/Game/Items/ItemTable rows with their localised names (worker FText decode checked against Conv_TextToString), codes from ItemNameToTemplateID; XX_/dev/test rows dropped, duplicate names get ' (#templateId)'. Built once per process."},
    {"listEntities", "pending", "native", "Takaro entity DTO array with display names", "MCP entitySearch sample has display names and zero class names (not yet run live)", "/Game/Systems/SpawnTable/SpawnDataTable rows: code = row name (also ConanCharacter.SourceSpawnTable on live NPCs), localised name, NPC class only in metadata. Built once per process."},
    {"listLocations", "pending", "native", "Takaro location DTO array", "MCP listLocations sample (not yet run live)", "/Game/Systems/Map/MapMarkers_ConanSandbox named places of the loaded map with world coordinates and discovery radius; retried until the table is loaded."},
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
    {"player-connected", "pending", "native", "{ player }", "eventSearch on a real join", "K2_PostLogin (BaseGameMode_C override) through the ProcessEvent hook dispatch: Steam64 from ConanPlayerController.UserIDFromURLOptions, name and IP from the PlayerState, cached per controller. Live proof pending."},
    {"player-disconnected", "pending", "native", "{ player }", "eventSearch on a real leave", "K2_OnLogout through the hook dispatch, identity from the PostLogin cache (PlayerState is already null at logout). Live proof pending."},
    {"chat-message", "pending", "native", "{ player, msg, channel }", "eventSearch plus the Discord post", "ConanPlayerController.ServerSendChatMessage through the hook dispatch: Steam64 from the called controller, text and channel from ChatRpcData (Global and Local -> global, Clan -> team). Live proof pending."},
    {"player-death", "pending", "native", "{ player, attacker?, position?, msg }", "real death in the client", "EventOnDeath transition to Dead on a player-controlled character, killer from OnOwnerKilled, cause from the last damage; position from K2_GetActorLocation. Live proof pending."},
    {"entity-killed", "pending", "native", "{ player, entity, weapon }", "real kill in the client", "EventOnDeath transition to Dead on a non-player character whose last damage came from a ConanPlayerController; entity display name and weapon item name read inside the hook. Live proof pending."},
    {"log", "live-supported", "native", "{ msg }", "Live 2026-10-03 on rig 349 (bridge stopped): Takaro log hook l2c-log-serverstats executed on three consecutive real LogServerStats lines 0.7-0.9 s after they were written (MCP eventSearch hook-executed; El-Limon evidence native-stage2/L2c)", "Worker-thread tail of ConanSandbox.log (follows the rotation at server start), redacted and rate limited (30 lines/s, burst 300, a summary line for dropped lines), plus the connector's own critical notices."},
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
