// Takaro Generic Game Protocol v1 as plain strings: no sockets, no game symbols, so it runs in host tests.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace TakaroProtocol
{
// The build the pinned ModLoader accepts; it refuses to load on any other.
inline constexpr const char* GameBuild = "CL-127004";
// Takaro recommends at least 1,000.
inline constexpr size_t EventBufferLimit = 2000;

struct Identity
{
    std::string identityToken;
    std::string registrationToken;
    std::string serverName;
    std::string connectorVersion;
};

struct Icon
{
    std::string revision;
    size_t size = 0;
};

struct Request
{
    std::string requestId;
    std::string action;
    std::string argsJson;
};

std::string BuildCapabilities(const Icon& icon);
std::string BuildIdentify(const Identity& identity, const Icon& icon);
std::optional<Request> ParseRequest(const std::string& frame);
std::string BuildResponse(const std::string& requestId, const std::string& payloadJson);
std::string BuildError(const std::string& requestId, const std::string& code, const std::string& message);
// Why these arguments can never work, or nothing when they are fine.
std::optional<std::string> ValidateArgs(const std::string& action, const std::string& argsJson);
// Actions the game thread has to carry out; the rest are answered from cached snapshots.
bool RunsOnGameThread(const std::string& action);

struct BufferedEvent
{
    uint64_t seq = 0;
    std::string frame;
    bool sent = false;
};

// One stream per process. A reconnect keeps it and resends what Takaro has not acknowledged.
class EventStream
{
public:
    explicit EventStream(std::string streamId) : streamId_(std::move(streamId)) {}
    void Push(const std::string& type, const std::string& dataJson);
    void Acknowledge(const std::string& streamId, uint64_t upTo);
    void MarkAllUnsent();
    BufferedEvent* NextUnsent();
    size_t Size() const { return events_.size(); }
    uint64_t Dropped() const { return dropped_; }
    const std::string& StreamId() const { return streamId_; }

private:
    std::string streamId_;
    std::deque<BufferedEvent> events_;
    uint64_t nextSeq_ = 1;
    uint64_t dropped_ = 0;
};
}
