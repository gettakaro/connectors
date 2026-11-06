#include "plugin_lifecycle.h"
#include <utility>

bool TakaroPluginLifecycle::Initialize(ITakaroPluginLifecycleHost& host)
{
    if (started_) return true;
    if (!host.EssentialHooksAvailable())
    {
        host.ResetState();
        return false;
    }
    if (!host.StartLink())
    {
        host.ResetState();
        return false;
    }
    host.RegisterHooks();
    host.SetUnready();
    started_ = true;
    return true;
}

void TakaroPluginLifecycle::Shutdown(ITakaroPluginLifecycleHost& host)
{
    host.BeginShutdown();
    if (started_) host.UnregisterHooks();
    host.StopLink();
    host.ResetState();
    started_ = false;
}

void TakaroPlayerLifecycleTracker::BeginReconciliation()
{
    reconciling_ = true;
    connections_.clear();
    activePlayers_.clear();
}

std::optional<TakaroPlayerSnapshot> TakaroPlayerLifecycleTracker::Joined(
    void* playerController, const std::optional<TakaroPlayerSnapshot>& player)
{
    if (!playerController || !player) return std::nullopt;
    const auto [_, connectionInserted] = connections_.try_emplace(playerController, Connection{*player});
    if (!connectionInserted) return std::nullopt;

    // PostLogin can expose a platform identity before EOSPlus has installed the
    // stable server identity. Keep only the callback-local disconnect snapshot;
    // the authoritative GameState reconciliation emits the connect transition.
    return std::nullopt;
}

std::optional<TakaroPlayerSnapshot> TakaroPlayerLifecycleTracker::Left(void* playerController)
{
    const auto found = connections_.find(playerController);
    if (found == connections_.end()) return std::nullopt;
    const auto player = found->second.player;
    connections_.erase(found);
    if (activePlayers_.erase(player.gameId) == 0) return std::nullopt;
    return player;
}

TakaroPlayerLifecycleTransitions TakaroPlayerLifecycleTracker::Reconcile(
    const std::vector<TakaroPlayerSnapshot>& players,
    const std::map<void*, std::string>& controllerGameIds)
{
    TakaroPlayerLifecycleTransitions transitions;
    std::map<std::string, TakaroPlayerSnapshot> replacement;
    for (const auto& player : players) replacement[player.gameId] = player;

    for (const auto& [playerController, gameId] : controllerGameIds)
    {
        if (!playerController) continue;
        const auto player = replacement.find(gameId);
        if (player == replacement.end()) continue;
        connections_.insert_or_assign(playerController, Connection{player->second});
    }

    if (!reconciling_)
    {
        for (const auto& [gameId, player] : replacement)
            if (!activePlayers_.contains(gameId)) transitions.connected.push_back(player);
        for (const auto& [gameId, player] : activePlayers_)
            if (!replacement.contains(gameId)) transitions.disconnected.push_back(player);
    }

    activePlayers_ = std::move(replacement);

    for (auto connection = connections_.begin(); connection != connections_.end();)
    {
        if (!activePlayers_.contains(connection->second.player.gameId))
            connection = connections_.erase(connection);
        else
        {
            connection->second.player = activePlayers_.at(connection->second.player.gameId);
            ++connection;
        }
    }

    reconciling_ = false;
    return transitions;
}
