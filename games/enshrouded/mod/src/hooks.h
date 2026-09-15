// Signature resolution, MinHook install, game-thread work queue.
#pragma once
#include "common.h"

#include <functional>

void HooksInit();  // resolve + install; never throws, reports per-capability status

// Runs fn on the server main thread (drained from the session update tick). Returns false on timeout
// or when the game-thread hook is unavailable. On timeout the task may still run later.
bool RunOnGameThread(std::function<std::string()> fn, std::string& result, DWORD timeoutMs = 5000);

void HooksHousekeep();  // called periodically from the plugin housekeeping thread
std::string HookDiagnosticsJson();
std::string GameBuild();

// Moderation: account-action handler on the moderation-system thread. type: Kick=0, Ban=1, UnBan=2.
bool ModerationReady();
// Kick/Ban on a player whose group has canKickBan: the game itself skips such targets; the plugin lifts that check
// for the one call (bypassedAdminProtection=true). httpStatus: 409 when the game has no player slot for the account.
bool AccountAction(uint64_t accountId, uint8_t type, std::string& err, DWORD timeoutMs = 5000, int* httpStatus = nullptr,
                   bool* bypassedAdminProtection = nullptr);
// Graceful save-and-quit through the game's console control handler (async; returns once started).
bool RequestShutdown(std::string& err);
bool RunOnModerationThread(std::function<std::string(uint64_t server)> fn, std::string& result, DWORD timeoutMs = 3000);

// Shared helpers for other hook modules (world.cpp).
bool InstallHook(uint32_t rva, LPVOID detour, LPVOID* orig, std::string& err);
void RecordResolve(const std::string& name, uint32_t rva, const std::string& how, bool hooked);
uint64_t CapturedServer();       // Server* captured by the moderation-system detour (0 until it ran)
uint32_t ModerationSystemRva();  // server network update (0x69cd40 @1024233), 0 if unresolved
