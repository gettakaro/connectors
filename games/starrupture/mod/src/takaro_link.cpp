#include "takaro_link.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "game_icon.h"
#include "net/net_log.h"
#include "net/transport_winhttp.h"
#include "plugin_state.h"
#include "starrupture_admin.h"
#include "takaro_protocol.h"
#include "version.h"

namespace
{
using Clock = std::chrono::steady_clock;
using native::FrameKind;
using native::Notice;
using native::NoticeType;
using native::QueueStatus;

constexpr auto TickInterval = std::chrono::milliseconds(50);
constexpr auto EventPollInterval = std::chrono::milliseconds(250);
// Takaro fails a call after 10 seconds, so a late answer has no one to reach.
constexpr auto ActionTimeout = std::chrono::milliseconds(9500);
constexpr auto ShutdownFlushDelay = std::chrono::milliseconds(500);
constexpr size_t EventBatch = 200;
constexpr size_t MaxInbox = 4096;

struct PendingAction
{
    std::string requestId;
    std::string actionId;
    std::string action;
    uint64_t epoch = 0;
    Clock::time_point deadline;
};

class TakaroLink
{
public:
    explicit TakaroLink(TakaroLinkOptions options)
        : options_(std::move(options)),
          events_("starrupture-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()))
    {
    }

    bool Start()
    {
        native::WinHttpTransport::Options transport;
        transport.url = options_.url;
        transport.caFile = options_.caFile;
        transport_ = std::make_unique<native::WinHttpTransport>(transport);
        worker_ = std::thread([this] { Run(); });
        if (transport_->Start([this](Notice&& notice) { return Push(std::move(notice)); })) return true;
        Stop();
        return false;
    }

    void Stop()
    {
        stopping_ = true;
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (transport_) transport_->Stop();
    }

private:
    bool Push(Notice&& notice)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (inbox_.size() >= MaxInbox) return false;
            inbox_.push_back(std::move(notice));
        }
        wake_.notify_one();
        return true;
    }

    void Run()
    {
        auto nextEventPoll = Clock::now();
        while (!stopping_)
        {
            std::deque<Notice> notices;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, TickInterval, [this] { return stopping_.load() || !inbox_.empty(); });
                notices.swap(inbox_);
            }
            for (const auto& notice : notices) Handle(notice);
            FinishActions();
            RunShutdownWhenDue();
            if (Clock::now() < nextEventPoll) continue;
            nextEventPoll = Clock::now() + EventPollInterval;
            PumpEvents();
        }
        // The last players' disconnects are recorded as the server goes down.
        PumpEvents();
        std::this_thread::sleep_for(ShutdownFlushDelay);
    }

    static TakaroProtocol::Icon GameIcon() { return {TakaroGameIconRevision(), TakaroGameIconSize()}; }

    QueueStatus Send(FrameKind kind, std::string text, uint64_t epoch, uint64_t outboxId = 0)
    {
        native::OutFrame frame;
        frame.kind = kind;
        frame.text = std::make_shared<const std::string>(std::move(text));
        frame.epoch = epoch;
        frame.outboxId = outboxId;
        return transport_->Queue(std::move(frame));
    }

    void Handle(const Notice& notice)
    {
        if (notice.type == NoticeType::Open)
        {
            epoch_ = notice.epoch;
            identified_ = false;
            Send(FrameKind::Control, TakaroProtocol::BuildIdentify({options_.identityToken, options_.registrationToken,
                options_.serverName, TAKARO_PLUGIN_VERSION}, GameIcon()), notice.epoch);
            return;
        }
        if (notice.type == NoticeType::Closed || notice.type == NoticeType::Error)
        {
            identified_ = false;
            return PluginLog("takaro: connection lost: %s", notice.text.c_str());
        }
        if (notice.type != NoticeType::Frame) return;
        const auto type = ExtractJsonString(notice.text, "type");
        if (!type) return;
        if (*type == "request") return OnRequest(notice);
        if (*type == "identifyResponse") return OnIdentifyResponse(notice);
        if (*type == "eventAck") return OnEventAck(notice);
        if (*type == "eventError" || *type == "error")
            PluginLog("takaro: rejected: %s", notice.text.substr(0, 400).c_str());
    }

    void OnIdentifyResponse(const Notice& notice)
    {
        const auto payload = ExtractJsonObject(notice.text, "payload");
        const auto gameServerId = payload ? ExtractJsonString(*payload, "gameServerId") : std::nullopt;
        if (!gameServerId)
            return PluginLog("takaro: identify rejected: %s", notice.text.substr(0, 400).c_str());
        transport_->MarkIdentified(notice.epoch);
        identified_ = true;
        events_.MarkAllUnsent();
        PluginLog("takaro: connected as game server %s", gameServerId->c_str());
        SendEvents();
    }

    void OnEventAck(const Notice& notice)
    {
        const auto payload = ExtractJsonObject(notice.text, "payload");
        const auto streamId = payload ? ExtractJsonString(*payload, "streamId") : std::nullopt;
        const auto upTo = payload ? ExtractJsonPositiveInteger(*payload, "upTo") : std::nullopt;
        if (streamId && upTo) events_.Acknowledge(*streamId, static_cast<uint64_t>(*upTo));
    }

    void OnRequest(const Notice& notice)
    {
        const auto request = TakaroProtocol::ParseRequest(notice.text);
        if (!request) return;
        const auto respond = [&](const std::string& payload) {
            Send(FrameKind::Response, TakaroProtocol::BuildResponse(request->requestId, payload), notice.epoch);
        };
        const auto fail = [&](const char* code, const std::string& message) {
            Send(FrameKind::Response, TakaroProtocol::BuildError(request->requestId, code, message), notice.epoch);
        };
        if (const auto problem = TakaroProtocol::ValidateArgs(request->action, request->argsJson))
            return fail("invalid_args", *problem);

        auto& state = TakaroState();
        const auto gameId = ExtractJsonString(request->argsJson, "gameId").value_or("");
        if (request->action == "testReachability") return respond(state.ReachabilityJson());
        if (request->action == "getPlayers") return respond(state.PlayerListJson());
        if (request->action == "getPlayer") return respond(state.PlayerJsonFor(gameId));
        if (request->action == "getPlayerLocation") return respond(state.LocationJsonFor(gameId));
        if (request->action == "getPlayerProgression") return respond(state.ProgressionJsonFor(gameId));
        if (request->action == "getGameIcon")
            return respond("{\"revision\":\"" + std::string(TakaroGameIconRevision()) + "\",\"data\":\"" +
                TakaroGameIconBase64() + "\"}");
        if (!TakaroProtocol::RunsOnGameThread(request->action))
            return fail("unsupported", "StarRupture does not support " + request->action + ".");
        // Game-thread work only runs once the world ticks; until then it could only time out.
        if (state.WorldPhase() != TakaroWorldPhase::Gameplay)
            return fail("game_error", "The server is still starting; try again once it shows as reachable.");

        const auto deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::system_clock::now() + ActionTimeout).time_since_epoch()).count();
        const auto created = state.CreateAction(request->action, request->argsJson, deadline);
        const auto actionId = ExtractJsonString(created, "actionId");
        if (!actionId)
            return fail("game_error", ExtractJsonString(created, "error").value_or("the action was refused"));
        pending_.push_back({request->requestId, *actionId, request->action, notice.epoch,
            Clock::now() + ActionTimeout});
    }

    void FinishActions()
    {
        for (auto action = pending_.begin(); action != pending_.end();)
        {
            const auto outcome = TakaroState().ActionOutcome(action->actionId);
            const bool done = outcome.state == "completed";
            const bool failed = outcome.state == "failed" || outcome.state == "missing";
            const bool late = !done && !failed && Clock::now() > action->deadline;
            if (!done && !failed && !late)
            {
                ++action;
                continue;
            }
            if (done)
                Send(FrameKind::CriticalResponse,
                    TakaroProtocol::BuildResponse(action->requestId, outcome.resultJson), action->epoch);
            else
                Send(FrameKind::CriticalResponse, TakaroProtocol::BuildError(action->requestId, "game_error",
                    late ? "The game did not finish this in time." : outcome.error), action->epoch);
            if (done && action->action == "shutdown") ScheduleShutdown(action->actionId);
            action = pending_.erase(action);
        }
    }

    // The response must reach Takaro before the process exits, so the quit waits for it.
    void ScheduleShutdown(const std::string& actionId)
    {
        const auto identity = TakaroState().CompletedActionIdentity(actionId);
        if (!identity) return PluginLog("takaro: the finished shutdown action could not be found");
        TakaroPreparedShutdownAcknowledgement prepared;
        if (!PrepareTakaroShutdownAfterActionAcknowledgement(identity->workToken, identity->generation, prepared))
            return PluginLog("takaro: the game thread cannot take the shutdown");
        PluginLog("takaro: shutdown requested, the server is saving and will stop");
        shutdown_ = prepared;
        shutdownAt_ = Clock::now() + ShutdownFlushDelay;
    }

    void RunShutdownWhenDue()
    {
        if (!shutdown_.dispatch || Clock::now() < shutdownAt_) return;
        const auto dispatch = shutdown_.dispatch;
        auto* context = shutdown_.context;
        shutdown_ = {};
        dispatch(context);
    }

    void PumpEvents()
    {
        std::string instanceId;
        auto events = TakaroState().EventsAfter(eventCursor_, EventBatch, instanceId);
        if (instanceId != instanceId_)
        {
            instanceId_ = instanceId;
            eventCursor_ = 0;
            events = TakaroState().EventsAfter(0, EventBatch, instanceId);
        }
        for (const auto& event : events)
        {
            eventCursor_ = event.id;
            events_.Push(event.type, event.dataJson);
        }
        if (events_.Dropped() != reportedDropped_)
        {
            reportedDropped_ = events_.Dropped();
            PluginLog("takaro: event buffer full, %llu events dropped so far",
                static_cast<unsigned long long>(reportedDropped_));
        }
        SendEvents();
    }

    void SendEvents()
    {
        if (!identified_) return;
        while (auto* event = events_.NextUnsent())
        {
            if (Send(FrameKind::Event, event->frame, epoch_, event->seq) != QueueStatus::Accepted) return;
            event->sent = true;
        }
    }

    TakaroLinkOptions options_;
    std::unique_ptr<native::WinHttpTransport> transport_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Notice> inbox_;
    std::atomic<bool> stopping_{false};

    // Worker thread only.
    TakaroProtocol::EventStream events_;
    std::vector<PendingAction> pending_;
    uint64_t epoch_ = 0;
    bool identified_ = false;
    uint64_t eventCursor_ = 0;
    uint64_t reportedDropped_ = 0;
    std::string instanceId_;
    TakaroPreparedShutdownAcknowledgement shutdown_{};
    Clock::time_point shutdownAt_{};
};

// Deliberately not a smart pointer: a static owner would destroy a running link at DLL unload,
// and destroying a joinable std::thread ends the process in std::terminate.
TakaroLink* g_link = nullptr;
}

bool StartTakaroLink(const TakaroLinkOptions& options)
{
    g_link = new TakaroLink(options);
    if (g_link->Start()) return true;
    delete g_link;
    g_link = nullptr;
    return false;
}

void StopTakaroLink()
{
    if (!g_link) return;
    g_link->Stop();
    delete g_link;
    g_link = nullptr;
}
