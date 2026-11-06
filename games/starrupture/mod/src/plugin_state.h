#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "starrupture_world_phase.h"

struct TakaroSkillReading
{
    std::string skill;
    int level = 0;
    double experience = 0;
};

struct TakaroPlayerSnapshot
{
    std::string gameId;
    std::string name;
    std::string platformId;
    std::string steamId;
    int ping = 0;
    bool hasLocation = false;
    // Raw Unreal world coordinates in centimetres. Conversion is forbidden until live measurement names one.
    double x = 0;
    double y = 0;
    double z = 0;
    // Empty until the server has loaded the player's saved skills, so a joining
    // player is never reported at a default level.
    std::optional<std::vector<TakaroSkillReading>> skills;
};

enum class TakaroOperationType
{
    GetPlayer,
    GetPlayers,
    GetPlayerLocation,
    GetPlayerInventory,
    GiveItem,
    ListItems,
    ListEntities,
    ListLocations,
    ExecuteConsoleCommand,
    SendMessage,
    TeleportPlayer,
    TestReachability,
    KickPlayer,
    BanPlayer,
    UnbanPlayer,
    ListBans,
    Shutdown,
    GetMapInfo,
    GetMapTile,
};

struct TakaroPositionDto { double x = 0; double y = 0; double z = 0; std::optional<std::string> dimension; };
struct TakaroItemDto { std::string name; std::string code; std::optional<std::string> description; std::optional<double> amount; std::optional<std::string> quality; };
struct TakaroEntityDto
{
    TakaroEntityDto() = default;
    TakaroEntityDto(std::string codeValue, std::string nameValue,
        std::optional<std::string> descriptionValue = std::nullopt,
        std::optional<std::string> typeValue = std::nullopt,
        std::optional<std::string> metadataJsonValue = std::nullopt)
        : code(std::move(codeValue)), name(std::move(nameValue)), description(std::move(descriptionValue)),
          type(std::move(typeValue)), metadataJson(std::move(metadataJsonValue)) {}
    std::string code;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> type;
    std::optional<std::string> metadataJson;
};
struct TakaroLocationDto
{
    TakaroLocationDto() = default;
    TakaroLocationDto(TakaroPositionDto positionValue, std::string nameValue,
        std::optional<std::string> codeValue = std::nullopt,
        std::optional<std::string> metadataJsonValue = std::nullopt)
        : position(std::move(positionValue)), name(std::move(nameValue)), code(std::move(codeValue)),
          metadataJson(std::move(metadataJsonValue)) {}
    TakaroPositionDto position;
    std::string name;
    std::optional<std::string> code;
    std::optional<std::string> metadataJson;
};
struct TakaroCommandOutputDto { std::string rawResult; bool success = false; std::optional<std::string> errorMessage; };
struct TakaroReachabilityDto { bool connectable = false; std::optional<std::string> reason; std::optional<double> latency; };
struct TakaroBanDto { TakaroPlayerSnapshot player; std::string reason; std::optional<std::string> expiresAt; };
struct TakaroMapInfoDto { bool enabled = false; double mapBlockSize = 0; double maxZoom = 0; double mapSizeX = 0; double mapSizeY = 0; double mapSizeZ = 0; };

using TakaroResultValue = std::variant<
    std::monostate, std::optional<TakaroPlayerSnapshot>, std::vector<TakaroPlayerSnapshot>,
    std::optional<TakaroPositionDto>, std::vector<TakaroItemDto>, std::vector<TakaroEntityDto>,
    std::vector<TakaroLocationDto>, TakaroCommandOutputDto, TakaroReachabilityDto,
    std::vector<TakaroBanDto>, TakaroMapInfoDto, std::string>;
struct TakaroOperationSuccess { TakaroOperationType type; TakaroResultValue value; };
struct TakaroOperationFailure { std::string error; };
struct TakaroOperationDeferred
{
    // Why the work is not finished yet. Surfaced verbatim if the action later expires,
    // so an operator is told the postcondition never held rather than that the work was
    // never processed.
    std::string pendingReason;
};
using TakaroOperationResult = std::variant<TakaroOperationSuccess, TakaroOperationFailure, TakaroOperationDeferred>;

struct TakaroOperationRequest
{
    TakaroOperationType type;
    std::string action;
    std::string argsJson;
    uint64_t workToken;
    int64_t deadlineUnixMs;
    uint32_t attempt = 0;
    uint64_t generation = 0;
};

struct TakaroCompletedActionIdentity
{
    TakaroOperationType type;
    uint64_t workToken;
    uint64_t generation;
};

const std::array<TakaroOperationType, 19>& AllTakaroOperationTypes();
std::string TakaroOperationName(TakaroOperationType type);
std::optional<TakaroOperationType> ParseTakaroOperationType(const std::string& action);

struct TakaroQueuedEvent
{
    uint64_t id = 0;
    std::string type;
    std::string dataJson;
};

struct TakaroActionOutcome
{
    std::string state;
    std::string resultJson;
    std::string error;
};

class ITakaroGameOperations
{
public:
    virtual ~ITakaroGameOperations() = default;
    virtual TakaroOperationResult Execute(const TakaroOperationRequest& request) = 0;
};

struct TakaroActionRecord
{
    std::string id;
    uint64_t workToken = 0;
    uint64_t generation = 0;
    TakaroOperationType type = TakaroOperationType::TestReachability;
    std::string action;
    std::string argsJson;
    std::string state = "pending";
    std::optional<TakaroOperationSuccess> result;
    std::string error;
    std::chrono::steady_clock::time_point createdAt;
    std::chrono::steady_clock::time_point updatedAt;
    std::chrono::system_clock::time_point deadlineAt;
    uint32_t operationAttempts = 0;
    std::string pendingReason;
};

class TakaroPluginState
{
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using NowFunction = std::function<TimePoint()>;
    using WallTimePoint = std::chrono::system_clock::time_point;
    using WallNowFunction = std::function<WallTimePoint()>;

    explicit TakaroPluginState(
        NowFunction now = [] { return std::chrono::steady_clock::now(); },
        WallNowFunction wallNow = [] { return std::chrono::system_clock::now(); });
    void Configure(std::vector<std::string> exactCommands, std::vector<std::string> commandPrefixes,
        int maxEvents, int maxActionAgeSeconds, int maxPendingActions = 128);
    void SetGameOperations(ITakaroGameOperations* operations);
    // Records the world the game thread last observed. Readiness can only be
    // reported while the gameplay world (ChimeraMain) has begun play; the
    // dedicated server's DedicatedServerStart lobby is never reachable.
    void SetWorldPhase(TakaroWorldPhase phase, std::string worldName);
    TakaroWorldPhase WorldPhase() const;
    // Marks the world reconciled. Ready is only honoured in the gameplay phase.
    void SetReady(bool ready);
    void BeginShutdown();
    void Reset();
    std::string HealthJson() const;
    std::string PlayersJson() const;
    std::string EventsJson(uint64_t after, size_t limit) const;
    // Answers for Takaro, all from the cached snapshot: no game-thread work.
    std::string ReachabilityJson() const;
    std::string PlayerListJson() const;
    std::string PlayerJsonFor(const std::string& gameId) const;
    std::string LocationJsonFor(const std::string& gameId) const;
    std::string ProgressionJsonFor(const std::string& gameId) const;
    std::vector<TakaroQueuedEvent> EventsAfter(uint64_t after, size_t limit, std::string& instanceId) const;
    TakaroActionOutcome ActionOutcome(const std::string& actionId) const;
    std::string CreateAction(const std::string& action, const std::string& argsJson,
        int64_t deadlineUnixMs = 4102444800000LL);
    std::string ActionJson(const std::string& actionId) const;
    std::optional<TakaroCompletedActionIdentity> CompletedActionIdentity(const std::string& actionId) const;
    void ProcessPendingActions();
    void AddLogEvent(const std::string& level, const std::string& message);
    void AddPlayerConnected(const TakaroPlayerSnapshot& player);
    void AddPlayerDisconnected(const TakaroPlayerSnapshot& player);
    // Ends every session still in the snapshot because the server itself is
    // going away (deferred quit committed, gameplay world ending). Emits one
    // player-disconnected per player carrying `reason` as the event message and
    // empties the snapshot. A session is ended at most once per world; a later
    // reconcile that re-observes the same connection before the process exits
    // does not produce a second disconnect.
    std::vector<TakaroPlayerSnapshot> EndAllPlayerSessions(const std::string& reason);
    void AddChatMessage(const TakaroPlayerSnapshot& player, const std::string& message);
    void AddPlayerDeath(const TakaroPlayerSnapshot& player,
        const std::optional<TakaroPlayerSnapshot>& attacker,
        const std::optional<TakaroPositionDto>& position);
    void AddEntityKilled(const TakaroPlayerSnapshot& player,
        const std::string& entity, const std::string& weapon);
    void UpsertPlayer(const TakaroPlayerSnapshot& player);
    void RemovePlayer(const std::string& gameId);
    void ReplacePlayers(const std::vector<TakaroPlayerSnapshot>& players);
    void ClearWorldState();
    size_t RetainedActionCount() const;

private:
    bool IsCommandAllowedLocked(const std::string& command) const;
    void CompleteActionLocked(TakaroActionRecord& record, const TakaroOperationSuccess& result);
    void FailActionLocked(TakaroActionRecord& record, const std::string& error);
    void TrimActionResultsLocked(TimePoint now);
    void AddEventLocked(const std::string& type, const std::string& dataJson);

    mutable std::mutex mutex_;
    NowFunction now_;
    WallNowFunction wallNow_;
    ITakaroGameOperations* operations_ = nullptr;
    bool ready_ = false;
    TakaroWorldPhase worldPhase_ = TakaroWorldPhase::Unknown;
    std::string worldName_;
    struct RememberedPlayer
    {
        std::optional<TakaroPositionDto> position;
        std::optional<std::vector<TakaroSkillReading>> skills;
        TimePoint seenAt{};
    };
    void RememberLocked(const TakaroPlayerSnapshot& player);
    std::map<std::string, RememberedPlayer> remembered_;
    std::string instanceId_;
    uint64_t nextEventId_ = 1;
    uint64_t nextActionId_ = 1;
    uint64_t nextWorkToken_ = 1;
    uint64_t generation_ = 1;
    bool acceptingActions_ = false;
    int maxEvents_ = 512;
    int maxActionAgeSeconds_ = 30;
    int maxPendingActions_ = 128;
    int maxRetainedActions_ = 256;
    std::vector<std::string> exactCommands_;
    std::vector<std::string> commandPrefixes_;
    std::map<std::string, TakaroPlayerSnapshot> players_;
    std::set<std::string> endedSessionGameIds_;
    std::deque<std::pair<uint64_t, std::pair<std::string, std::string>>> events_;
    std::map<std::string, TakaroActionRecord> actions_;
};

TakaroPluginState& TakaroState();
std::string JsonEscape(const std::string& value);
std::optional<std::string> ExtractJsonString(const std::string& json, const std::string& key);
std::optional<std::string> ExtractJsonObject(const std::string& json, const std::string& key);
std::optional<int64_t> ExtractJsonPositiveInteger(const std::string& json, const std::string& key);
std::optional<double> ExtractJsonFiniteNumber(const std::string& json, const std::string& key);
bool ParseActionRequest(const std::string& json, std::string& action, std::string& argsJson,
    int64_t& deadlineUnixMs, std::string& error);
