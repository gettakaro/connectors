// World access: player transform/teleport, inventory read, give item, chat send/receive, death/kill events.
// See research/re-world-items.md and research/re-chat-death.md for the reverse-engineering behind every offset.
#pragma once
#include "common.h"
#include "gamedata.h"

void WorldInitCapabilities();  // marks world capabilities "degraded: initializing" before resolution
void WorldHooksInit();         // resolve + hook; degrades per capability, never throws
void WorldHousekeep();         // periodic (250 ms) from the plugin thread
std::string WorldDiagnosticsJson();

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// All of these run the game-side work on the server thread and wait for it (never call from a game thread).
// `playerName` is the Steam persona name as the server stores it in the player slot.
bool WorldGetLocation(const std::string& playerName, Vec3& out, std::string& err);
bool WorldTeleport(const std::string& playerName, const Vec3& to, std::string& err);
bool WorldInventoryJson(const std::string& playerName, std::string& json, std::string& err);
bool WorldGiveItem(const std::string& playerName, const ItemDef& item, uint32_t amount, std::string& err, std::string& detail);
// recipientMachine: "" broadcasts; otherwise the machine index printed in "[server] Machine 'M'" of the recipient.
bool WorldSendMessage(const std::string& text, const std::string& recipientMachine, uint8_t type, uint32_t senderHandle,
                      std::string& err);
// Diagnostic: entities with enemy/animal/npc/boss/faction components within `radius` metres of the player.
bool WorldNearbyJson(const std::string& playerName, double radius, std::string& json, std::string& err);
bool WorldSlotsJson(std::string& json, std::string& err);  // diagnostic dump of the 16 player slots

const ItemDef* FindItemByCode(const std::string& code);  // exact, then case-insensitive, then numeric/hex itemId
const std::string& ItemsJson();
const std::string& EntitiesJson();
const std::string& LocationsJson();
