#pragma once

#include "plugin_state.h"

#include <cstdint>
#include <mutex>
#include <string>

struct TakaroPreparedShutdownAcknowledgement
{
    void* context = nullptr;
    void (*dispatch)(void* context) = nullptr;
    void (*cancel)(void* context) = nullptr;
};

struct TakaroAdminHostResult
{
    bool ok = false;
    std::string error;
};

class ITakaroAdminHost
{
public:
    virtual ~ITakaroAdminHost() = default;
    virtual TakaroAdminHostResult PreflightQuit() = 0;
    virtual TakaroAdminHostResult RequestSave() = 0;
    virtual TakaroAdminHostResult RequestQuit() = 0;
    // Called once the deferred quit is committed so every session still in the
    // Player Snapshot is reported as disconnected before the process goes away.
    virtual void EndPlayerSessions() = 0;
};

enum class TakaroShutdownState
{
    Idle,
    WaitingForActionResultAcknowledgement,
    QuitRequested,
    Failed
};

class TakaroShutdownCoordinator
{
public:
    explicit TakaroShutdownCoordinator(ITakaroAdminHost& host);

    TakaroOperationResult Request(const TakaroOperationRequest& request);
    bool CanAcknowledge(uint64_t workToken, uint64_t generation) const;
    void AfterActionResultAcknowledged(uint64_t workToken, uint64_t generation);
    void Reset();

    TakaroShutdownState State() const;
    std::string LastError() const;

private:
    ITakaroAdminHost& host_;
    mutable std::mutex mutex_;
    uint64_t pendingWorkToken_ = 0;
    uint64_t pendingGeneration_ = 0;
    TakaroShutdownState state_ = TakaroShutdownState::Idle;
    std::string lastError_;
};

TakaroOperationResult ExecuteTakaroAdminOperation(const TakaroOperationRequest& request);
bool PrepareTakaroShutdownAfterActionAcknowledgement(uint64_t workToken, uint64_t generation,
    TakaroPreparedShutdownAcknowledgement& prepared);
void RunAcknowledgedTakaroShutdownOnGameThread(void*);
void ResetTakaroAdmin();
TakaroShutdownState TakaroAdminShutdownState();
std::string TakaroAdminLastError();
