#include "chat_probe.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <deque>

namespace ChatProbe {

const char* const kOwnedTriggers[2] = {"!tkprobe204", "/tkprobe204"};

namespace {

const size_t kMaxTraces = 128;
const size_t kMaxEnvelopes = 64;
const size_t kMaxQueue = 32;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_stubUp{true};
std::atomic<uint64_t> g_seq{0};
std::atomic<uint64_t> g_consumed{0}, g_envelopes{0}, g_failedPrivate{0}, g_received{0}, g_notified{0},
    g_queueDropped{0};

Mutex g_lock;
std::deque<Trace> g_traces;
std::deque<Envelope> g_received_list;  // what the stub consumer took, newest last

struct Item {
    bool notify = false;
    Envelope env;
};
std::deque<Item> g_queue;
pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
std::function<void(const std::string&, const std::string&)> g_notifier;

uint64_t MonoNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// One worker step: the stub consumer. Takes the envelope (receipt) or sends the private failure.
void Handle(const Item& it) {
    if (it.notify) {
        if (g_notifier) g_notifier(it.env.gameId, "Takaro command unavailable (probe stub is down). Nothing was sent.");
        g_notified++;
        PluginLog("chat-probe: %s failed privately for %s (stub down)", it.env.trigger.c_str(), it.env.gameId.c_str());
        return;
    }
    {
        Guard g(g_lock);
        g_received_list.push_back(it.env);
        while (g_received_list.size() > kMaxEnvelopes) g_received_list.pop_front();
    }
    g_received++;
    PluginLog("chat-probe: stub received envelope %s from %s: %s %s", it.env.dedupeId.c_str(), it.env.gameId.c_str(),
              it.env.trigger.c_str(), it.env.rawArgs.c_str());
}

bool PopItem(Item& out, bool wait) {
    pthread_mutex_lock(&g_qm);
    while (wait && g_queue.empty()) pthread_cond_wait(&g_cv, &g_qm);
    bool have = !g_queue.empty();
    if (have) {
        out = g_queue.front();
        g_queue.pop_front();
    }
    pthread_mutex_unlock(&g_qm);
    return have;
}

void* Worker(void*) {
    for (;;) {
        Item it;
        if (PopItem(it, true)) Handle(it);
    }
    return nullptr;
}

void Push(Item it) {
    pthread_mutex_lock(&g_qm);
    if (g_queue.size() >= kMaxQueue) {
        g_queueDropped++;
    } else {
        g_queue.push_back(std::move(it));
        pthread_cond_signal(&g_cv);
    }
    pthread_mutex_unlock(&g_qm);
}

}  // namespace

void Init() {
    g_enabled = ConfigValue("TAKARO_CHAT_PROBE", "") == "1";
    g_stubUp = ConfigValue("TAKARO_CHAT_PROBE_STUB", "up") != "down";
    if (!g_enabled) return;
    pthread_t t;
    if (pthread_create(&t, nullptr, Worker, nullptr) == 0) pthread_detach(t);
    PluginLog("chat-probe: ENABLED (research #204), stub %s, owned triggers %s %s", g_stubUp ? "up" : "down",
              kOwnedTriggers[0], kOwnedTriggers[1]);
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
bool StubAvailable() { return g_stubUp.load(std::memory_order_relaxed); }

bool MatchOwned(const std::string& msg, std::string& trigger, std::string& args) {
    if (msg.size() > kMaxMessage) return false;
    std::string t = Trim(msg);
    size_t sp = 0;
    while (sp < t.size() && !IsSpace(t[sp])) sp++;
    std::string first = t.substr(0, sp);
    for (const char* k : kOwnedTriggers) {
        if (first != k) continue;
        trigger = first;
        args = Trim(t.substr(sp));
        if (args.size() > kMaxArgs) args.resize(kMaxArgs);
        return true;
    }
    return false;
}

void RecordTrace(Trace t) {
    if (t.message.size() > kMaxMessage) t.message.resize(kMaxMessage);
    t.seq = ++g_seq;
    t.monoNs = MonoNs();
    Guard g(g_lock);
    g_traces.push_back(std::move(t));
    while (g_traces.size() > kMaxTraces) g_traces.pop_front();
}

std::string Consume(const std::string& gameId, const std::string& playerName, int segment, const std::string& trigger,
                    const std::string& rawArgs) {
    g_consumed++;
    Item it;
    it.env.seq = g_consumed.load();
    it.env.monoNs = MonoNs();
    it.env.gameId = gameId;
    it.env.playerName = playerName;
    it.env.trigger = trigger;
    it.env.rawArgs = rawArgs;
    it.env.segment = segment;
    if (!StubAvailable()) {
        // No envelope, nothing queued for later: only the private notice.
        g_failedPrivate++;
        it.notify = true;
        Push(std::move(it));
        return "failed-private";
    }
    it.env.dedupeId = BootId() + ":" + std::to_string(it.env.seq);
    g_envelopes++;
    Push(std::move(it));
    return "envelope";
}

void SetPrivateNotifier(std::function<void(const std::string&, const std::string&)> fn) { g_notifier = std::move(fn); }

std::string Json() {
    std::string o = "{\"enabled\":" + std::string(Enabled() ? "true" : "false") +
                    ",\"stub\":" + JsonStr(StubAvailable() ? "up" : "down") + ",\"ownedTriggers\":[" +
                    JsonStr(kOwnedTriggers[0]) + "," + JsonStr(kOwnedTriggers[1]) + "]" +
                    ",\"counters\":{\"consumed\":" + std::to_string(g_consumed.load()) +
                    ",\"envelopes\":" + std::to_string(g_envelopes.load()) +
                    ",\"stubReceived\":" + std::to_string(g_received.load()) +
                    ",\"failedPrivate\":" + std::to_string(g_failedPrivate.load()) +
                    ",\"privateNotices\":" + std::to_string(g_notified.load()) +
                    ",\"queueDropped\":" + std::to_string(g_queueDropped.load()) + "}";
    Guard g(g_lock);
    o += ",\"traces\":[";
    bool first = true;
    for (auto& t : g_traces) {
        if (!first) o += ",";
        first = false;
        o += "{\"seq\":" + std::to_string(t.seq) + ",\"monoNs\":" + std::to_string(t.monoNs) +
             ",\"function\":" + JsonStr(t.function) + ",\"receiverClass\":" + JsonStr(t.receiverClass) +
             ",\"message\":" + JsonStr(t.message) + ",\"segment\":" + std::to_string(t.segment) +
             ",\"commandClass\":" + JsonStr(t.commandClass) + ",\"senderGameId\":" + JsonStr(t.senderGameId) +
             ",\"senderName\":" + JsonStr(t.senderName) + ",\"consumed\":" + (t.consumed ? "true" : "false") + "}";
    }
    o += "],\"envelopes\":[";
    first = true;
    for (auto& e : g_received_list) {
        if (!first) o += ",";
        first = false;
        o += "{\"dedupeId\":" + JsonStr(e.dedupeId) + ",\"seq\":" + std::to_string(e.seq) +
             ",\"monoNs\":" + std::to_string(e.monoNs) + ",\"gameId\":" + JsonStr(e.gameId) +
             ",\"playerName\":" + JsonStr(e.playerName) + ",\"trigger\":" + JsonStr(e.trigger) +
             ",\"rawArgs\":" + JsonStr(e.rawArgs) + ",\"segment\":" + std::to_string(e.segment) + "}";
    }
    o += "]}";
    return o;
}

#ifdef TAKARO_CHAT_PROBE_TEST
void TestEnable(bool enabled, bool stubUp) {
    g_enabled = enabled;
    g_stubUp = stubUp;
}
void TestDrain() {
    Item it;
    while (PopItem(it, false)) Handle(it);
}
#endif

}  // namespace ChatProbe
