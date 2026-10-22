// Research probe for gettakaro/connectors#204: can a vanilla VEIN client invoke a Takaro-owned chat
// trigger and have the server consume it before the public broadcast?
//
// OFF by default. Nothing here runs unless TAKARO_CHAT_PROBE=1 is set at boot; with it unset the
// ProcessEvent detour behaves exactly as before. With it set:
//   * every chat RPC the detour sees is traced (function, receiver class, message, segment, the
//     UChatCommand class argument, sender identity, whether it was consumed) into a bounded ring;
//   * an inbound Server_Say whose first word is one of the hard-coded owned triggers is consumed:
//     the original function is NOT called, so VEIN never broadcasts it, and one stub envelope is
//     handed to a probe worker thread. Nothing is sent to Takaro.
//   * TAKARO_CHAT_PROBE_STUB=down simulates an unavailable backend: the trigger is still consumed,
//     no envelope is created or queued, and the sender gets a private notification only.
// GET /debug/chat-probe (TAKARO_PLUGIN_DEBUG=1) returns the traces, envelopes and counters.
#pragma once
#include "common.h"

#include <functional>
#include <string>

namespace ChatProbe {

// The exact owned triggers. Case-sensitive, whole first word only.
extern const char* const kOwnedTriggers[2];

// Reads TAKARO_CHAT_PROBE / TAKARO_CHAT_PROBE_STUB once. Call off the game thread before hooks fire.
void Init();
bool Enabled();
bool StubAvailable();

// Pure: true when `msg` (trimmed) starts with an owned trigger as a whole word, whatever its length.
// `args` is the rest, trimmed, capped at kMaxArgs bytes. Traces keep at most kMaxMessage bytes.
static const size_t kMaxMessage = 512;
static const size_t kMaxArgs = 256;
bool MatchOwned(const std::string& msg, std::string& trigger, std::string& args);

// Pure: what the detour does with one chat RPC. Only an inbound Server_Say with an owned trigger is
// consumed; without a resolvable sender it is still consumed (kept private) but gets no envelope.
enum class Decision { kPassThrough, kConsume, kConsumeNoIdentity };
Decision Decide(const std::string& function, bool haveIdentity, const std::string& msg, std::string& trigger,
                std::string& args);

struct Trace {
    uint64_t seq = 0;
    uint64_t monoNs = 0;
    std::string function;      // Server_Say / NetMulticast_SendChat / ...
    std::string receiverClass; // class of the object ProcessEvent ran on
    std::string message;       // capped at kMaxMessage
    int segment = -1;          // raw EChatSegment, -1 when absent
    std::string commandClass;  // "null", the class name, or "unreadable"
    std::string senderGameId;
    std::string senderName;
    bool consumed = false;
};
void RecordTrace(Trace t);

struct Envelope {
    uint64_t seq = 0;
    std::string dedupeId;  // "<bootId>:<seq>": unique per server boot and invocation
    uint64_t monoNs = 0;
    std::string gameId, playerName, trigger, rawArgs;
    int segment = -1;
};

// Called from the ProcessEvent detour (game thread) for a matched inbound trigger. Bounded work
// only: builds the envelope and pushes it onto the worker queue, or, when the stub is down, queues
// a private notification. Returns "envelope", "failed-private" or "dropped" (queue full).
std::string Consume(const std::string& gameId, const std::string& playerName, int segment,
                    const std::string& trigger, const std::string& rawArgs);

// A consumed trigger whose sender could not be identified: counted, nothing else.
void ConsumeWithoutIdentity();

// The worker uses this to send a private message to one player; returns whether it was delivered.
// Set once at init.
void SetPrivateNotifier(std::function<bool(const std::string& gameId, const std::string& text)> fn);

std::string Json();

#ifdef TAKARO_CHAT_PROBE_TEST
void TestEnable(bool enabled, bool stubUp);
void TestDrain();  // runs the worker step synchronously
#endif

}  // namespace ChatProbe
