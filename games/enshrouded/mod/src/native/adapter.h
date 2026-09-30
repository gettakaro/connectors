// The 17 Takaro actions for Enshrouded: a port of the former sidecar's EnshroudedAdapter
// (sidecar/src/enshrouded/adapter.ts) with its plugin-client error semantics (pluginClient.ts),
// calling the plugin in-process through GameApi instead of over loopback HTTP.
//
// Runs on an action worker thread only. It reads an immutable ActionView and reports the state
// changes it wants (players seen, timed-ban changes) in the outcome; the bridge thread applies them.
#pragma once
#include "common.h"
#include "native/game_api.h"

#include <map>
#include <string>
#include <vector>

namespace native {

struct TimedBan {
    std::string gameId;     // SteamID64
    std::string expiresAt;  // as Takaro sent it (echoed back by listBans)
    std::string reason;
    int64_t expiresAtMs = 0;
    int64_t createdAtMs = 0;
    int64_t nextAttemptMs = 0;  // expiry retry backoff (not persisted meaningfully)
    int attempts = 0;
};

struct ActionView {
    std::vector<JsonValue> knownPlayers;  // Takaro-shaped players this connector has seen
    std::map<std::string, TimedBan> timedBans;
    std::string banStoreError;  // non-empty: the timed-ban store is unreadable; timed bans and listBans refuse
};

struct BanChange {
    enum Op { None, Upsert, Remove } op = None;
    TimedBan ban;
};

struct ActionOutcome {
    bool ok = false;
    JsonValue payload;  // Null answers {} (the sidecar's createResponse)
    std::string error;  // response error text when !ok
    std::vector<JsonValue> seenPlayers;
    BanChange ban;
};

ActionOutcome ExecuteAction(GameApi& game, const std::string& action, const JsonValue& args, const ActionView& view,
                            int64_t nowMs);
// Lifts a timed ban whose expiresAt has passed (internal, never sent by Takaro).
ActionOutcome ExpireTimedBan(GameApi& game, const std::string& gameId);

std::string EncodeUriComponent(const std::string& s);

}  // namespace native
