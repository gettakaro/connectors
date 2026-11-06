#include "starrupture_admin.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

const TakaroOperationFailure& Failure(const TakaroOperationResult& result)
{
    const auto* failure = std::get_if<TakaroOperationFailure>(&result);
    if (!failure) throw std::runtime_error("expected operation failure");
    return *failure;
}

class FakeAdminHost final : public ITakaroAdminHost
{
public:
    TakaroAdminHostResult PreflightQuit() override
    {
        calls.push_back("preflight-quit");
        return preflightResult;
    }

    TakaroAdminHostResult RequestSave() override
    {
        calls.push_back("request-save");
        return saveResult;
    }

    TakaroAdminHostResult RequestQuit() override
    {
        calls.push_back("request-quit");
        return quitResult;
    }

    void EndPlayerSessions() override
    {
        calls.push_back("end-player-sessions");
    }

    TakaroAdminHostResult preflightResult{true, ""};
    TakaroAdminHostResult saveResult{true, ""};
    TakaroAdminHostResult quitResult{true, ""};
    std::vector<std::string> calls;
};

TakaroOperationRequest Request(TakaroOperationType type = TakaroOperationType::Shutdown,
    uint64_t workToken = 1, uint64_t generation = 1)
{
    const std::string action = type == TakaroOperationType::Shutdown ? "shutdown" : "kickPlayer";
    auto request = TakaroOperationRequest{type, action, "{}", workToken, 4102444800000LL};
    request.generation = generation;
    return request;
}
}

int main()
{
    try
    {
        {
            FakeAdminHost host;
            TakaroShutdownCoordinator shutdown(host);
            const auto result = shutdown.Request(Request(TakaroOperationType::KickPlayer));
            Expect(Failure(result).error.find("not an administration action") != std::string::npos,
                "non-shutdown action was not rejected honestly");
            Expect(host.calls.empty(), "non-shutdown action touched the admin host");
        }

        {
            FakeAdminHost host;
            host.preflightResult = {false, "quit seam unavailable"};
            TakaroShutdownCoordinator shutdown(host);
            const auto result = shutdown.Request(Request());
            Expect(Failure(result).error.find("quit seam unavailable") != std::string::npos,
                "quit preflight failure was hidden");
            Expect(host.calls == std::vector<std::string>{"preflight-quit"},
                "save was requested despite unavailable quit seam");
            Expect(shutdown.State() == TakaroShutdownState::Idle,
                "failed quit preflight scheduled a shutdown");
        }

        {
            FakeAdminHost host;
            host.saveResult = {false, "save subsystem unavailable"};
            TakaroShutdownCoordinator shutdown(host);
            const auto result = shutdown.Request(Request());
            Expect(Failure(result).error.find("save subsystem unavailable") != std::string::npos,
                "save request failure was hidden");
            Expect(host.calls == std::vector<std::string>{"preflight-quit", "request-save"},
                "save failure did not stop shutdown preparation");
            Expect(shutdown.State() == TakaroShutdownState::Idle,
                "failed save request scheduled a shutdown");
        }

        {
            FakeAdminHost host;
            TakaroShutdownCoordinator shutdown(host);
            const auto result = shutdown.Request(Request());
            const auto* success = std::get_if<TakaroOperationSuccess>(&result);
            Expect(success && success->type == TakaroOperationType::Shutdown &&
                    std::holds_alternative<std::monostate>(success->value),
                "valid shutdown did not return the queued-shutdown acknowledgement");
            Expect(host.calls == std::vector<std::string>{"preflight-quit", "request-save"},
                "shutdown did not request save exactly once before acknowledgement");
            Expect(shutdown.State() == TakaroShutdownState::WaitingForActionResultAcknowledgement,
                "valid shutdown did not wait for action-result acknowledgement");

            shutdown.AfterActionResultAcknowledged(1, 1);
            Expect(host.calls == std::vector<std::string>{
                    "preflight-quit", "request-save", "request-quit", "end-player-sessions"},
                "shutdown did not request quit and then end player sessions after acknowledging the action result");
            Expect(shutdown.State() == TakaroShutdownState::QuitRequested,
                "successful quit request did not reach terminal state");
            shutdown.AfterActionResultAcknowledged(1, 1);
            Expect(host.calls.size() == 4, "quit was requested more than once");
        }

        {
            FakeAdminHost host;
            TakaroShutdownCoordinator shutdown(host);
            Expect(std::holds_alternative<TakaroOperationSuccess>(shutdown.Request(Request())),
                "first shutdown request failed");
            const auto duplicate = shutdown.Request(Request());
            Expect(Failure(duplicate).error.find("already pending") != std::string::npos,
                "duplicate shutdown request was accepted");
            Expect(host.calls.size() == 2, "duplicate shutdown repeated preflight or save");
        }

        {
            FakeAdminHost host;
            host.quitResult = {false, "world disappeared before quit"};
            TakaroShutdownCoordinator shutdown(host);
            Expect(std::holds_alternative<TakaroOperationSuccess>(shutdown.Request(Request())),
                "shutdown preparation unexpectedly failed");
            shutdown.AfterActionResultAcknowledged(1, 1);
            Expect(shutdown.State() == TakaroShutdownState::Failed,
                "deferred quit failure was not retained");
            Expect(host.calls.back() == "request-quit",
                "player sessions were ended although the deferred quit failed");
            Expect(shutdown.LastError().find("world disappeared before quit") != std::string::npos,
                "deferred quit failure was not reported honestly");
            shutdown.Reset();
            Expect(shutdown.State() == TakaroShutdownState::Idle && shutdown.LastError().empty(),
                "shutdown coordinator did not reset world-owned state");
        }

        {
            FakeAdminHost host;
            TakaroShutdownCoordinator shutdown(host);
            Expect(std::holds_alternative<TakaroOperationSuccess>(shutdown.Request(Request(
                TakaroOperationType::Shutdown, 41, 7))), "first-generation shutdown preparation failed");
            shutdown.Reset();
            Expect(std::holds_alternative<TakaroOperationSuccess>(shutdown.Request(Request(
                TakaroOperationType::Shutdown, 42, 8))), "replacement-generation shutdown preparation failed");
            Expect(!shutdown.CanAcknowledge(41, 7),
                "stale shutdown identity remained acknowledgement-eligible after reset");
            shutdown.AfterActionResultAcknowledged(41, 7);
            Expect(host.calls == std::vector<std::string>{
                    "preflight-quit", "request-save", "preflight-quit", "request-save"},
                "stale shutdown acknowledgement quit the replacement world");
            Expect(shutdown.CanAcknowledge(42, 8),
                "current shutdown identity was not acknowledgement-eligible");
            shutdown.AfterActionResultAcknowledged(42, 8);
            Expect(host.calls.size() >= 2 && host.calls[host.calls.size() - 2] == "request-quit" &&
                    host.calls.back() == "end-player-sessions" &&
                    shutdown.State() == TakaroShutdownState::QuitRequested,
                "current shutdown acknowledgement did not request quit and end player sessions");
        }

        {
            ResetTakaroAdmin();
            const auto unavailable = ExecuteTakaroAdminOperation(Request());
            Expect(Failure(unavailable).error.find("pinned StarRupture Game SDK is unavailable") != std::string::npos,
                "portable build did not fail the production shutdown seam closed");
            RunAcknowledgedTakaroShutdownOnGameThread(nullptr);
            Expect(TakaroAdminShutdownState() == TakaroShutdownState::Idle && TakaroAdminLastError().empty(),
                "portable production coordinator scheduled a quit without the pinned SDK");
        }

        std::cout << "starrupture_admin_test: passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "starrupture_admin_test: " << error.what() << "\n";
        return 1;
    }
}
