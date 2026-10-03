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
    {"giveItem", "pending", "native", "{}", "MCP gameserverGiveItem plus the stack in the client inventory", "ItemInventory.AddItemTemplate on ConanCharacter.GetBackpackInventory, one stack per call, looped until the amount is given and read back with GetNumberOfItemsByTemplate. The item code is a template id or an ItemNameToTemplateID row name; unknown codes are refused. Quality is ignored (Conan items have no quality tier). Not proven live yet."},
    {"listItems", "pending", "pending", "Takaro item DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: ItemTable rows with localized names."},
    {"listEntities", "pending", "pending", "Takaro entity DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: class walk with a humaniser."},
    {"listLocations", "pending", "pending", "Takaro location DTO array", "MCP sample", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"executeConsoleCommand", "pending", "native", "{ success: boolean, rawResult: string }", "MCP gameserverExecuteCommand rawResult", "KismetSystemLibrary.ExecuteConsoleCommand in the first online player's console with the admin flag set and restored inside one game-thread job (the engine console when nobody is online). rawResult = the server log lines of that frame (Conan commands answer through the log). exit/quit and the RCON verbs (listplayers, kickplayer, banplayer, unbanplayer, listbans, broadcast, shutdown) are refused with the Takaro action to use. Not proven live yet."},
    {"sendMessage", "live-supported", "native", "{}", "Live 2026-10-03 on rig 349 (bridge stopped): MCP gameserverSendMessage global and directed (opts.recipient.gameId + senderNameOverride) -> native log delivered=1 each, both lines in the client chat (El-Limon evidence native-stage2/L1)", "ClientReceiveChatMessage on each online ConanPlayerController through ProcessEvent on the game thread; global, or directed by Steam64 / character name."},
    {"teleportPlayer", "pending", "native", "{}", "MCP gameserverTeleportPlayer plus the client position", "ConanPlayerController.TeleportPlayerServer (RunCheatCheck=false, SnapToGround=true), Conan's own teleport with area streaming; read back with K2_GetActorLocation (x/y within 300 units; z snaps to the ground). Not proven live yet."},
    {"testReachability", "live-supported", "native", "{ connectable: boolean, reason: string | null }", "Live 2026-10-03: MCP gameserverTestReachabilityForId -> {connectable:true, reason:null} from the native library with the bridge stopped; unknown-build path covered by tests/so_test.py", "In-process: connectable when the server build is pinned and the ProcessEvent hook is installed; an unknown build answers connectable=false with the reason."},
    {"kickPlayer", "pending", "native", "{}", "client disconnected with the reason", "PlayerController.ClientReturnToMainMenuWithTextReason with the reason (FText from KismetTextLibrary.Conv_StringToText); succeeds only once the player has left the online list. Not proven live yet."},
    {"banPlayer", "pending", "native", "{}", "rejoin refused, listBans", "Connector ban list keyed by Steam64 in Saved/Config/Takaro/bans.json (atomic write), online and offline, permanent or timed (expiresAt); enforced by a kick at K2_PostLogin and a sweep over the online players. Conan's own blacklist cannot hold offline or timed bans and is not touched. Not proven live yet."},
    {"unbanPlayer", "pending", "native", "{}", "rejoin allowed, listBans", "Removes the Steam64 from the connector ban list (works offline). Timed bans are lifted by the connector itself at expiry. Not proven live yet."},
    {"listBans", "pending", "native", "Takaro ban DTO array", "MCP gameserverListBans", "The active entries of the connector ban list with player, reason and expiresAt. Not proven live yet."},
    {"shutdown", "pending", "native", "{}", "countdown in the client plus process exit", "Answers first, then a chat countdown (TAKARO_CONAN_SHUTDOWN_SECONDS, default 60), then the engine's graceful exit through KismetSystemLibrary.ExecuteConsoleCommand(\"exit\") -> UGameEngine::HandleExitCommand -> FUnixPlatformMisc::RequestExit. No signature pin. Not proven live yet."},
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
