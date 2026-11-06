#pragma once

#include "plugin_state.h"
#include <map>
#include <optional>
#include <vector>

class ITakaroPluginLifecycleHost
{
public:
    virtual ~ITakaroPluginLifecycleHost() = default;
    virtual bool EssentialHooksAvailable() const = 0;
    virtual bool StartLink() = 0;
    virtual void RegisterHooks() = 0;
    virtual void SetUnready() = 0;
    virtual void BeginShutdown() = 0;
    virtual void UnregisterHooks() = 0;
    virtual void StopLink() = 0;
    virtual void ResetState() = 0;
};

class TakaroPluginLifecycle
{
public:
    bool Initialize(ITakaroPluginLifecycleHost& host);
    void Shutdown(ITakaroPluginLifecycleHost& host);
    bool Started() const { return started_; }

private:
    bool started_ = false;
};

struct TakaroPlayerLifecycleTransitions
{
    std::vector<TakaroPlayerSnapshot> connected;
    std::vector<TakaroPlayerSnapshot> disconnected;
};

class TakaroPlayerLifecycleTracker
{
public:
    void BeginReconciliation();
    std::optional<TakaroPlayerSnapshot> Joined(
        void* playerController, const std::optional<TakaroPlayerSnapshot>& player);
    std::optional<TakaroPlayerSnapshot> Left(void* playerController);
    TakaroPlayerLifecycleTransitions Reconcile(
        const std::vector<TakaroPlayerSnapshot>& players,
        const std::map<void*, std::string>& controllerGameIds = {});
    size_t RetainedConnectionCount() const { return connections_.size(); }

private:
    struct Connection
    {
        TakaroPlayerSnapshot player;
    };

    bool reconciling_ = false;
    std::map<void*, Connection> connections_;
    std::map<std::string, TakaroPlayerSnapshot> activePlayers_;
};
