#include "plugin_lifecycle.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeLifecycleHost final : public ITakaroPluginLifecycleHost
{
public:
    bool EssentialHooksAvailable() const override { return hooksAvailable; }
    bool StartLink() override { calls.push_back("start-link"); return linkStarts; }
    void RegisterHooks() override { calls.push_back("register-hooks"); }
    void SetUnready() override { calls.push_back("set-unready"); }
    void BeginShutdown() override { calls.push_back("begin-shutdown"); }
    void UnregisterHooks() override { calls.push_back("unregister-hooks"); }
    void StopLink() override { calls.push_back("stop-link"); }
    void ResetState() override { calls.push_back("reset-state"); }

    bool hooksAvailable = true;
    bool linkStarts = true;
    std::vector<std::string> calls;
};

void ExpectCalls(const FakeLifecycleHost& host, std::initializer_list<const char*> expected)
{
    std::vector<std::string> values;
    for (const auto* value : expected) values.emplace_back(value);
    Expect(host.calls == values, "lifecycle call order differed");
}
}

int main()
{
    try
    {
        FakeLifecycleHost missingHooks;
        TakaroPluginLifecycle missingHooksLifecycle;
        missingHooks.hooksAvailable = false;
        Expect(!missingHooksLifecycle.Initialize(missingHooks), "missing essential hooks started runtime");
        ExpectCalls(missingHooks, {"reset-state"});

        FakeLifecycleHost failedBridge;
        TakaroPluginLifecycle failedBridgeLifecycle;
        failedBridge.linkStarts = false;
        Expect(!failedBridgeLifecycle.Initialize(failedBridge), "failed bridge started runtime");
        ExpectCalls(failedBridge, {"start-link", "reset-state"});

        FakeLifecycleHost started;
        TakaroPluginLifecycle lifecycle;
        Expect(lifecycle.Initialize(started), "valid runtime did not start");
        Expect(lifecycle.Started(), "successful runtime was not marked started");
        ExpectCalls(started, {"start-link", "register-hooks", "set-unready"});
        lifecycle.Shutdown(started);
        Expect(!lifecycle.Started(), "shutdown runtime remained started");
        ExpectCalls(started, {
            "start-link", "register-hooks", "set-unready",
            "begin-shutdown", "unregister-hooks", "stop-link", "reset-state",
        });

        TakaroPlayerSnapshot player;
        player.gameId = "stable-id";
        player.name = "Player One";
        player.ping = 23;
        player.hasLocation = false;
        TakaroPlayerSnapshot second = player;
        second.gameId = "stable-id-2";
        second.name = "Player Two";

        TakaroPlayerLifecycleTracker players;
        players.BeginReconciliation();
        const auto baseline = players.Reconcile({player});
        Expect(baseline.connected.empty() && baseline.disconnected.empty(),
            "initial reload/world snapshot emitted false lifecycle events");

        const auto missedJoin = players.Reconcile({player, second});
        Expect(missedJoin.connected.size() == 1 && missedJoin.connected.front().gameId == second.gameId,
            "periodic reconciliation did not recover a missed join callback");
        Expect(missedJoin.disconnected.empty(), "missed join reconciliation emitted a disconnect");
        const auto repeated = players.Reconcile({player, second});
        Expect(repeated.connected.empty() && repeated.disconnected.empty(),
            "repeated identical snapshot emitted lifecycle events");
        const auto missedLeave = players.Reconcile({player});
        Expect(missedLeave.connected.empty() && missedLeave.disconnected.size() == 1 &&
            missedLeave.disconnected.front().name == "Player Two",
            "periodic reconciliation did not use the prior cached snapshot for a missed leave");

        TakaroPlayerSnapshot provisional = second;
        provisional.gameId = "provisional-id";
        provisional.ping = 0;
        auto* stabilizingController = reinterpret_cast<void*>(0x10);
        Expect(!players.Joined(stabilizingController, provisional),
            "join callback emitted a provisional identity before world reconciliation");
        const auto stabilizedJoin = players.Reconcile(
            {player, second}, {{stabilizingController, second.gameId}});
        Expect(stabilizedJoin.connected.size() == 1 &&
            stabilizedJoin.connected.front().gameId == second.gameId &&
            stabilizedJoin.disconnected.empty(),
            "stable world identity did not replace a provisional callback with one clean connect");
        const auto stabilizedLeave = players.Left(stabilizingController);
        Expect(stabilizedLeave && stabilizedLeave->gameId == second.gameId,
            "stable reconciliation did not update the controller's teardown-safe identity cache");

        auto* rapidReconnectController = reinterpret_cast<void*>(0x11);
        Expect(!players.Joined(rapidReconnectController, provisional),
            "rapid reconnect emitted its provisional callback identity");
        const auto rapidReconnect = players.Reconcile(
            {player, second}, {{rapidReconnectController, second.gameId}});
        Expect(rapidReconnect.connected.size() == 1 &&
            rapidReconnect.connected.front().gameId == second.gameId &&
            rapidReconnect.disconnected.empty(),
            "leave and rapid reconnect did not preserve one new stable connection generation");
        const auto stabilizedRepeat = players.Reconcile(
            {player, second}, {{rapidReconnectController, second.gameId}});
        Expect(stabilizedRepeat.connected.empty() && stabilizedRepeat.disconnected.empty(),
            "stable identity reconciliation duplicated the rapid reconnect");

        auto* callbackFirstController = reinterpret_cast<void*>(0x1);
        Expect(!players.Joined(callbackFirstController, second),
            "join callback bypassed deferred identity stabilization");
        const auto afterCallbackJoin = players.Reconcile({player, second});
        Expect(afterCallbackJoin.connected.empty() && afterCallbackJoin.disconnected.empty(),
            "reconciliation duplicated an already-active stable identity");
        Expect(players.Left(callbackFirstController).has_value(), "left callback did not use cached player");
        const auto afterCallbackLeave = players.Reconcile({player});
        Expect(afterCallbackLeave.connected.empty() && afterCallbackLeave.disconnected.empty(),
            "reconciliation duplicated a callback-emitted disconnect");

        const auto reconcileFirstJoin = players.Reconcile({player, second});
        Expect(reconcileFirstJoin.connected.size() == 1, "reconcile-first join did not emit connect");
        auto* reconcileFirstController = reinterpret_cast<void*>(0x2);
        Expect(!players.Joined(reconcileFirstController, second),
            "callback duplicated a reconcile-emitted connect");
        const auto reconcileFirstLeave = players.Reconcile({player});
        Expect(reconcileFirstLeave.disconnected.size() == 1, "reconcile-first leave did not emit disconnect");
        Expect(!players.Left(reconcileFirstController), "callback duplicated a reconcile-emitted disconnect");
        const auto reconnected = players.Reconcile({player, second});
        Expect(reconnected.connected.size() == 1 && reconnected.disconnected.empty(),
            "later reconnect did not start exactly one new generation");

        auto* deferredController = reinterpret_cast<void*>(0x3);
        Expect(!players.Joined(deferredController, std::nullopt),
            "join without a Stable StarRupture Identity emitted a connect");
        Expect(players.RetainedConnectionCount() == 0,
            "unresolved join retained a raw controller outside the callback");
        Expect(!players.Left(deferredController),
            "unresolved join retained controller state for later teardown access");
        TakaroPlayerSnapshot third = player;
        third.gameId = "stable-id-3";
        const auto deferredConnect = players.Reconcile({player, second, third});
        Expect(deferredConnect.connected.size() == 1 && deferredConnect.connected.front().gameId == third.gameId,
            "stable reconciliation did not recover an unresolved join exactly once");
        const auto repeatedDeferred = players.Reconcile({player, second, third});
        Expect(repeatedDeferred.connected.empty() && repeatedDeferred.disconnected.empty(),
            "stable reconciliation duplicated a recovered unresolved join");
        Expect(!players.Left(deferredController),
            "missed leave for an unresolved callback retained a controller for later access");

        players.BeginReconciliation();
        Expect(!players.Joined(deferredController, player),
            "callback before the first world snapshot emitted a false connect");
        const auto worldBaseline = players.Reconcile({player});
        Expect(worldBaseline.connected.empty() && worldBaseline.disconnected.empty(),
            "world transition baseline emitted lifecycle events");
        Expect(players.Left(deferredController).has_value(),
            "world transition did not establish a fresh disconnect generation");

        std::cout << "plugin_lifecycle_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "plugin_lifecycle_test: " << error.what() << "\n";
        return 1;
    }
}
