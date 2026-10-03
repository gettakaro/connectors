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
    {"giveItem", "live-supported", "native", "{}", "Live 2026-10-03 rig 349 (El-Limon evidence native-stage2/L2b): MCP gameserverGiveItem Stone x1500 -> 2 AddItemTemplate stacks, 1000 + 500 in the client inventory; template id 10001 x7 after a death/respawn; x3 after the Steam64 fix; unknown code refused in the connector (Takaro's giveItem endpoint answers {} even then)", "ItemInventory.AddItemTemplate on ConanCharacter.GetBackpackInventory, one stack per call, looped until the amount is given and read back with GetNumberOfItemsByTemplate. The item code is a template id or an ItemNameToTemplateID row name (13 tables, 8,117 codes); unknown codes are refused. Quality is ignored (Conan items have no quality tier)."},
    {"listItems", "pending", "pending", "Takaro item DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: ItemTable rows with localized names."},
    {"listEntities", "pending", "pending", "Takaro entity DTO array with display names", "MCP sample has display names and zero class names", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback. Planned: class walk with a humaniser."},
    {"listLocations", "pending", "pending", "Takaro location DTO array", "MCP sample", "Not ported to the native connector yet (stage 2 Phase 2); answers a structured error naming the action. No RCON fallback."},
    {"executeConsoleCommand", "live-supported", "native", "{ success: boolean, rawResult: string }", "Live 2026-10-03 rig 349: MCP gameserverExecuteCommand 'log LogTemp Verbose' (engine console) and 'WhatsMyId' (player console) return the server's own output lines as rawResult; admin-only 'SpawnItem 12001 3' as a non-admin put 3 Plant Fiber in the client inventory; 'listplayers' refused", "KismetSystemLibrary.ExecuteConsoleCommand in the first online player's console with the admin flag set and restored inside one game-thread job (the engine console when nobody is online). rawResult = the server log lines tagged with that frame (Conan commands answer through the log; unrelated lines of the same frame can appear). exit/quit and the RCON verbs (listplayers, kickplayer, banplayer, unbanplayer, listbans, broadcast, shutdown) are refused with the Takaro action to use."},
    {"sendMessage", "live-supported", "native", "{}", "Live 2026-10-03 on rig 349 (bridge stopped): MCP gameserverSendMessage global and directed (opts.recipient.gameId + senderNameOverride) -> native log delivered=1 each, both lines in the client chat (El-Limon evidence native-stage2/L1)", "ClientReceiveChatMessage on each online ConanPlayerController through ProcessEvent on the game thread; global, or directed by Steam64 / character name."},
    {"teleportPlayer", "live-supported", "native", "{}", "Live 2026-10-03 rig 349: MCP gameserverTeleportPlayer to Riverwatch Camp and to two road positions; server log TeleportPlayerServer + HandleTeleportStreamingDone, read-back xy distance 0, client screenshots at each place", "ConanPlayerController.TeleportPlayerServer (RunCheatCheck=false, SnapToGround=true), Conan's own teleport with area streaming and loading screen; read back with K2_GetActorLocation (x/y within 300 units; z snaps to the ground)."},
    {"testReachability", "live-supported", "native", "{ connectable: boolean, reason: string | null }", "Live 2026-10-03: MCP gameserverTestReachabilityForId -> {connectable:true, reason:null} from the native library with the bridge stopped; unknown-build path covered by tests/so_test.py", "In-process: connectable when the server build is pinned and the ProcessEvent hook is installed; an unknown build answers connectable=false with the reason."},
    {"kickPlayer", "live-supported", "native", "{}", "Live 2026-10-03 rig 349: MCP gameserverKickPlayer -> client back at the main menu, server log Player disconnected within 0.3 s, connector confirmed the player left the online list (twice, before and after the Steam64 fix)", "PlayerController.ClientReturnToMainMenuWithTextReason with the reason (FText from KismetTextLibrary.Conv_StringToText); succeeds only once the player has left the online list. The client returns to the menu without showing the reason text."},
    {"banPlayer", "live-supported", "native", "{}", "Live 2026-10-03 rig 349: offline ban then rejoin -> kicked 2.6 s after Join succeeded (K2_PostLogin hook + sweep); online timed ban -> kicked at once, lifted by the connector at expiresAt, rejoin allowed", "Connector ban list keyed by Steam64 in Saved/Config/Takaro/bans.json (atomic write), online and offline, permanent or timed (expiresAt); enforced by kicks after K2_PostLogin and a sweep over the online players. Conan's own blacklist cannot hold offline or timed bans and is not touched."},
    {"unbanPlayer", "live-supported", "native", "{}", "Live 2026-10-03 rig 349: MCP gameserverUnbanPlayer (player offline) -> bans.json empty, rejoin succeeded and stayed in game", "Removes the Steam64 from the connector ban list (works offline). Timed bans are lifted by the connector itself at expiry (Takaro sends no unban)."},
    {"listBans", "live-supported", "native", "Takaro ban DTO array", "Live 2026-10-03 rig 349: MCP gameserverListBans with a timed ban (expiresAt), a permanent ban (null) and empty", "The active entries of the connector ban list with player, reason and expiresAt. Takaro sends no name with an offline ban, so the name falls back to the Steam64."},
    {"shutdown", "live-supported", "native", "{}", "Live 2026-10-03 rig 349: MCP gameserverShutdown -> '[Server]: The server shuts down in 1 minute / 30 / 10 / 5..2 seconds' in the client chat, then FUnixPlatformMisc::RequestExit(0, UGameEngine::HandleExitCommand), client 'Connection lost', LogExit: Exiting and the container exited with code 0 three minutes later", "Answers first, then a chat countdown (TAKARO_CONAN_SHUTDOWN_SECONDS, default 60), then the engine's graceful exit: the console command 'exit' through KismetSystemLibrary.ExecuteConsoleCommand in the admin-context console job. No signature pin. The server needs about three minutes to unload after the exit."},
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
