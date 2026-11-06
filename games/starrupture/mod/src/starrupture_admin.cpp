#include "starrupture_admin.h"

#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
#include "SDK.hpp"
#include "plugin_helpers.h"
#include <memory>
#include <new>
#endif

namespace
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
constexpr const char* kShutdownDisconnectReason =
    "StarRupture dedicated server is shutting down (Takaro shutdown action)";
#endif

TakaroOperationFailure Failure(const TakaroOperationRequest& request, const std::string& reason)
{
    return TakaroOperationFailure{request.action + ": " + reason};
}

class PinnedSdkAdminHost final : public ITakaroAdminHost
{
public:
    TakaroAdminHostResult PreflightQuit() override
    {
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
        auto* world = SDK::UWorld::GetWorld();
        if (!world) return {false, "StarRupture world is unavailable"};
        auto* libraryClass = SDK::UKismetSystemLibrary::StaticClass();
        if (!libraryClass || !libraryClass->GetFunction("KismetSystemLibrary", "ExecuteConsoleCommand"))
            return {false, "pinned in-process console seam is unavailable"};
        return {true, ""};
#else
        return {false, "pinned StarRupture Game SDK is unavailable"};
#endif
    }

    TakaroAdminHostResult RequestSave() override
    {
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
        auto* world = SDK::UWorld::GetWorld();
        if (!world) return {false, "StarRupture world is unavailable before save"};
        auto* subsystemLibraryClass = SDK::USubsystemBlueprintLibrary::StaticClass();
        auto* saveClass = SDK::UCrSaveSubsystem::StaticClass();
        if (!subsystemLibraryClass ||
            !subsystemLibraryClass->GetFunction("SubsystemBlueprintLibrary", "GetGameInstanceSubsystem") ||
            !saveClass || !saveClass->GetFunction("CrSaveSubsystem", "ExecuteSave"))
            return {false, "pinned authoritative save subsystem seam is unavailable"};
        auto* subsystem = SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(
            world, SDK::TSubclassOf<SDK::UGameInstanceSubsystem>(saveClass));
        if (!subsystem || !subsystem->IsA(saveClass))
            return {false, "authoritative StarRupture save subsystem instance is unavailable"};
        static_cast<SDK::UCrSaveSubsystem*>(subsystem)->ExecuteSave();
        return {true, ""};
#else
        return {false, "pinned StarRupture Game SDK is unavailable"};
#endif
    }

    TakaroAdminHostResult RequestQuit() override
    {
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
        auto* world = SDK::UWorld::GetWorld();
        if (!world) return {false, "StarRupture world disappeared before deferred quit"};
        auto* libraryClass = SDK::UKismetSystemLibrary::StaticClass();
        if (!libraryClass || !libraryClass->GetFunction("KismetSystemLibrary", "ExecuteConsoleCommand"))
            return {false, "pinned in-process console seam disappeared before deferred quit"};
        // QuitGame needs a player controller and does nothing on an empty server;
        // this falls through to the engine when nobody is online.
        SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"quit"), nullptr);
        return {true, ""};
#else
        return {false, "pinned StarRupture Game SDK is unavailable"};
#endif
    }

    void EndPlayerSessions() override
    {
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
        // The quit command only requests exit; the engine leaves its loop at the end of
        // this frame, so the disconnects land in the Takaro event buffer
        // while the bridge is still serving.
        TakaroState().EndAllPlayerSessions(kShutdownDisconnectReason);
#endif
    }
};

PinnedSdkAdminHost g_adminHost;
TakaroShutdownCoordinator g_shutdown(g_adminHost);

struct ShutdownAcknowledgementContext
{
    uint64_t workToken;
    uint64_t generation;
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    IPluginEngineEvents* engine;
#endif
};

void CancelPreparedTakaroShutdown(void* rawContext)
{
    delete static_cast<ShutdownAcknowledgementContext*>(rawContext);
}
}

TakaroShutdownCoordinator::TakaroShutdownCoordinator(ITakaroAdminHost& host)
    : host_(host)
{
}

TakaroOperationResult TakaroShutdownCoordinator::Request(const TakaroOperationRequest& request)
{
    std::lock_guard lock(mutex_);
    if (request.type != TakaroOperationType::Shutdown)
        return Failure(request, "operation is not an administration action");
    if (state_ != TakaroShutdownState::Idle)
        return Failure(request, "server shutdown is already pending or was already requested");

    const auto quit = host_.PreflightQuit();
    if (!quit.ok) return Failure(request, quit.error.empty() ? "quit seam preflight failed" : quit.error);
    const auto save = host_.RequestSave();
    if (!save.ok) return Failure(request, save.error.empty() ? "authoritative save request failed" : save.error);

    lastError_.clear();
    pendingWorkToken_ = request.workToken;
    pendingGeneration_ = request.generation;
    state_ = TakaroShutdownState::WaitingForActionResultAcknowledgement;
    return TakaroOperationSuccess{request.type, std::monostate{}};
}

bool TakaroShutdownCoordinator::CanAcknowledge(uint64_t workToken, uint64_t generation) const
{
    std::lock_guard lock(mutex_);
    return state_ == TakaroShutdownState::WaitingForActionResultAcknowledgement &&
        workToken == pendingWorkToken_ && generation == pendingGeneration_;
}

void TakaroShutdownCoordinator::AfterActionResultAcknowledged(uint64_t workToken, uint64_t generation)
{
    std::lock_guard lock(mutex_);
    if (state_ != TakaroShutdownState::WaitingForActionResultAcknowledgement ||
        workToken != pendingWorkToken_ || generation != pendingGeneration_) return;

    const auto quit = host_.RequestQuit();
    if (quit.ok)
    {
        state_ = TakaroShutdownState::QuitRequested;
        host_.EndPlayerSessions();
        return;
    }
    lastError_ = quit.error.empty() ? "deferred quit request failed" : quit.error;
    state_ = TakaroShutdownState::Failed;
}

void TakaroShutdownCoordinator::Reset()
{
    std::lock_guard lock(mutex_);
    pendingWorkToken_ = 0;
    pendingGeneration_ = 0;
    state_ = TakaroShutdownState::Idle;
    lastError_.clear();
}

TakaroOperationResult ExecuteTakaroAdminOperation(const TakaroOperationRequest& request)
{
    return g_shutdown.Request(request);
}

void RunAcknowledgedTakaroShutdownOnGameThread(void* rawContext)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    std::unique_ptr<ShutdownAcknowledgementContext> context(
        static_cast<ShutdownAcknowledgementContext*>(rawContext));
    if (context) g_shutdown.AfterActionResultAcknowledged(context->workToken, context->generation);
#else
    (void)rawContext;
#endif
}

void DispatchPreparedTakaroShutdown(void* rawContext)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    auto* context = static_cast<ShutdownAcknowledgementContext*>(rawContext);
    context->engine->PostToGameThread(&RunAcknowledgedTakaroShutdownOnGameThread, context);
#else
    CancelPreparedTakaroShutdown(rawContext);
#endif
}

bool PrepareTakaroShutdownAfterActionAcknowledgement(uint64_t workToken, uint64_t generation,
    TakaroPreparedShutdownAcknowledgement& prepared)
{
#if defined(TAKARO_STARRUPTURE_WITH_GAME_SDK)
    if (!g_shutdown.CanAcknowledge(workToken, generation)) return false;
    auto* hooks = GetHooks();
    if (!hooks || !hooks->Engine || !hooks->Engine->PostToGameThread) return false;
    auto* context = new (std::nothrow) ShutdownAcknowledgementContext{workToken, generation, hooks->Engine};
    if (!context) return false;
    prepared = TakaroPreparedShutdownAcknowledgement{
        context, &DispatchPreparedTakaroShutdown, &CancelPreparedTakaroShutdown};
    return true;
#else
    (void)workToken;
    (void)generation;
    (void)prepared;
    return false;
#endif
}

TakaroShutdownState TakaroShutdownCoordinator::State() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

std::string TakaroShutdownCoordinator::LastError() const
{
    std::lock_guard lock(mutex_);
    return lastError_;
}

void ResetTakaroAdmin()
{
    g_shutdown.Reset();
}

TakaroShutdownState TakaroAdminShutdownState()
{
    return g_shutdown.State();
}

std::string TakaroAdminLastError()
{
    return g_shutdown.LastError();
}
