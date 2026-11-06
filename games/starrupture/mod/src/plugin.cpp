#include "plugin.h"
#include "takaro_link.h"
#include "plugin_config.h"
#include "plugin_helpers.h"
#include "plugin_hook_requirements.h"
#include "plugin_lifecycle.h"
#include "plugin_state.h"
#include "starrupture_chat_hooks.h"
#include "starrupture_admin.h"
#include "starrupture_combat_events.h"
#include "starrupture_moderation.h"
#include "starrupture_sdk_bridge.h"

static IPluginSelf* g_self = nullptr;

IPluginSelf* GetSelf()
{
    return g_self;
}

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

static PluginInfo s_pluginInfo = {
    "TakaroStarRupture",
    MODLOADER_BUILD_TAG,
    "Takaro community",
    "Takaro connector for StarRupture dedicated servers",
    PLUGIN_INTERFACE_VERSION,
    PLUGIN_TARGET_SERVER
};

namespace
{
constexpr int MaxBufferedEvents = 2048;
constexpr int MaxActionAgeSeconds = 30;
constexpr int MaxPendingActions = 128;

float g_reconcileElapsed = 0.0f;
TakaroPlayerLifecycleTracker g_playerLifecycle;
TakaroPlayerDeathStateTracker g_playerDeathStates;

std::optional<TakaroWorldPhase> g_loggedWorldPhase;

void NoteWorldPhase(TakaroWorldPhase phase, const std::string& worldName)
{
    if (g_loggedWorldPhase == phase) return;
    g_loggedWorldPhase = phase;
    TAKARO_LOG_INFO("World phase %s (%s).", TakaroWorldPhaseName(phase),
        worldName.empty() ? "no world" : worldName.c_str());
}

bool ReconcileWorld()
{
    const auto world = ObserveTakaroWorld();
    const auto phase = world ? ClassifyTakaroWorldPhase(*world) : TakaroWorldPhase::Unknown;
    const auto worldName = world ? world->worldName : std::string{};
    TakaroState().SetWorldPhase(phase, worldName);
    NoteWorldPhase(phase, worldName);
    if (phase != TakaroWorldPhase::Gameplay)
    {
        // DedicatedServerStart parks the server until an owner starts the world
        // from Manage Server; nothing on it is a joinable player or a ban target.
        TakaroState().SetReady(false);
        return false;
    }
    std::vector<TakaroPlayerSnapshot> players;
    std::map<void*, std::string> controllerGameIds;
    std::map<std::string, bool> playerDeadStates;
    std::map<std::string, TakaroPositionDto> playerLastDeathPositions;
    if (!ReconcileTakaroPlayersFromWorld(
            players, controllerGameIds, &playerDeadStates, &playerLastDeathPositions))
    {
        TakaroState().SetReady(false);
        return false;
    }
    const auto transitions = g_playerLifecycle.Reconcile(players, controllerGameIds);
    for (const auto& player : transitions.connected) TakaroState().AddPlayerConnected(player);
    for (const auto& player : transitions.disconnected) TakaroState().AddPlayerDisconnected(player);
    for (const auto& death : g_playerDeathStates.Reconcile(
             players, playerDeadStates, playerLastDeathPositions))
        RecordTakaroPlayerDeathObservation(death);
    TakaroState().ReplacePlayers(players);
    EnforceTakaroBans();
    TakaroState().SetReady(true);
    return true;
}

void OnPlayerJoined(void* playerController)
{
    if (auto* hooks = GetHooks(); !hooks ||
        !EnsureTakaroChatHookForController(hooks->Hooks, playerController))
    {
        TAKARO_LOG_ERROR("Runtime player-controller chat hook could not be installed.");
    }
    const auto player = g_playerLifecycle.Joined(playerController, TakaroPlayerFromController(playerController));
    if (player) TakaroState().AddPlayerConnected(*player);
}

void OnPlayerLeft(void* playerController)
{
    // Logout can run after the controller/player-state graph has begun teardown.
    // Only the normalized snapshot captured while the connection was live is safe here.
    const auto player = g_playerLifecycle.Left(playerController);
    if (player) TakaroState().AddPlayerDisconnected(*player);
}

// ModLoader does not call PluginShutdown when the engine exits, so the link stops here.
void OnEngineShutdown()
{
    StopTakaroLink();
}

void OnEngineTick(float deltaSeconds)
{
    TakaroState().ProcessPendingActions();
    g_reconcileElapsed += deltaSeconds > 0.0f && deltaSeconds < 1.0f ? deltaSeconds : 0.0f;
    if (g_reconcileElapsed >= 2.0f)
    {
        g_reconcileElapsed = 0.0f;
        ReconcileWorld();
    }
}

void OnAnyWorldBeginPlay(SDK::UWorld*, const char* worldName)
{
    TAKARO_LOG_INFO("World begin play observed: %s.", worldName ? worldName : "(unnamed)");
    // Classify the new world on the next engine tick instead of waiting out the
    // two-second cadence, so readiness follows begin play promptly.
    g_reconcileElapsed = 2.0f;
}

void OnBeforeWorldEndPlay(SDK::UWorld*, const char* worldName)
{
    g_reconcileElapsed = 0.0f;
    if (TakaroState().WorldPhase() == TakaroWorldPhase::Gameplay)
    {
        // The gameplay world is going away, so every session it still holds ends
        // with it. A Takaro shutdown already ended them at quit time; this only
        // catches sessions that were still in the snapshot.
        const auto ended = TakaroState().EndAllPlayerSessions(
            "StarRupture gameplay world ended (dedicated server shutting down)");
        if (!ended.empty())
        {
            TAKARO_LOG_INFO("Ended %zu player session(s) because world %s is ending.", ended.size(),
                worldName ? worldName : "(unnamed)");
        }
    }
    g_playerLifecycle.BeginReconciliation();
    ResetTakaroAdmin();
    ResetTakaroCombatEvents();
    g_playerDeathStates.Reset();
    TakaroState().ClearWorldState();
}

class ModLoaderLifecycleHost final : public ITakaroPluginLifecycleHost
{
public:
    bool EssentialHooksAvailable() const override
    {
        auto* hooks = GetHooks();
        return hooks && EssentialTakaroHookGroupsAvailable(
            hooks->Engine != nullptr, hooks->World != nullptr, hooks->Players != nullptr);
    }

    bool StartLink() override
    {
        const auto registrationToken = TakaroConfig::RegistrationToken();
        if (registrationToken.empty())
        {
            TAKARO_LOG_ERROR("Takaro RegistrationToken is empty; paste it into TakaroStarRupture.ini and restart.");
            return true;
        }
        return StartTakaroLink({TakaroConfig::WsUrl(), TakaroConfig::IdentityToken(), registrationToken,
            TakaroConfig::ServerName(), {}});
    }

    void RegisterHooks() override
    {
        g_playerLifecycle.BeginReconciliation();
        auto* hooks = GetHooks();
        hooks->Players->RegisterOnPlayerJoined(OnPlayerJoined);
        hooks->Players->RegisterOnPlayerLeft(OnPlayerLeft);
        hooks->World->RegisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
        hooks->World->RegisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
        hooks->Engine->RegisterOnTick(OnEngineTick);
        hooks->Engine->RegisterOnShutdown(OnEngineShutdown);
        if (!InstallTakaroChatHook(hooks->Hooks))
        {
            TAKARO_LOG_ERROR("Pinned ModLoader ProcessEvent observer could not capture accepted player chat; inbound chat events are unavailable.");
        }
        if (!InstallTakaroCombatHooks(hooks->Hooks))
        {
            TAKARO_LOG_ERROR("Pinned ModLoader ProcessEvent observer could not capture combat callbacks; combat events are unavailable.");
        }
    }

    void SetUnready() override { TakaroState().SetReady(false); }
    void BeginShutdown() override { TakaroState().BeginShutdown(); }

    void UnregisterHooks() override
    {
        auto* hooks = GetHooks();
        if (!hooks) return;
        RemoveTakaroCombatHooks(hooks->Hooks);
        RemoveTakaroChatHook(hooks->Hooks);
        if (hooks->Engine)
        {
            hooks->Engine->UnregisterOnTick(OnEngineTick);
            hooks->Engine->UnregisterOnShutdown(OnEngineShutdown);
        }
        if (hooks->World)
        {
            hooks->World->UnregisterOnBeforeWorldEndPlay(OnBeforeWorldEndPlay);
            hooks->World->UnregisterOnAnyWorldBeginPlay(OnAnyWorldBeginPlay);
        }
        if (hooks->Players)
        {
            hooks->Players->UnregisterOnPlayerJoined(OnPlayerJoined);
            hooks->Players->UnregisterOnPlayerLeft(OnPlayerLeft);
        }
    }

    void StopLink() override { StopTakaroLink(); }
    void ResetState() override
    {
        g_loggedWorldPhase.reset();
        ResetTakaroAdmin();
        ResetTakaroCombatEvents();
        g_playerDeathStates.Reset();
        TakaroState().Reset();
    }
};

ModLoaderLifecycleHost g_lifecycleHost;
TakaroPluginLifecycle g_lifecycle;
}

extern "C" {

__declspec(dllexport) PluginInfo* GetPluginInfo()
{
    return &s_pluginInfo;
}

__declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self;
    TakaroConfig::Initialize(self);
    if (!TakaroConfig::IsEnabled())
    {
        TAKARO_LOG_WARN("TakaroStarRupture is disabled in config.");
        return true;
    }

    TakaroState().Configure({}, {}, MaxBufferedEvents, MaxActionAgeSeconds, MaxPendingActions);
    TakaroState().SetGameOperations(&TakaroGameOperations());
    ConfigureTakaroModerationRegistry("");
    if (!TakaroModerationRegistry().Healthy())
    {
        TAKARO_LOG_ERROR("Plugin-owned ban registry is unavailable or corrupt; refusing moderation startup.");
        return false;
    }

    const bool hooksAvailable = g_lifecycleHost.EssentialHooksAvailable();
    if (!g_lifecycle.Initialize(g_lifecycleHost))
    {
        if (!hooksAvailable)
        {
            TAKARO_LOG_ERROR("Essential ModLoader Engine, World, or Players hooks are unavailable; the connector cannot start.");
        }
        else
        {
            TAKARO_LOG_ERROR("The Takaro connection could not start; check WsUrl in TakaroStarRupture.ini.");
        }
        return false;
    }
    TAKARO_LOG_INFO("TakaroStarRupture loaded.");
    return true;
}

__declspec(dllexport) void PluginShutdown()
{
    g_lifecycle.Shutdown(g_lifecycleHost);
    g_self = nullptr;
}

}
