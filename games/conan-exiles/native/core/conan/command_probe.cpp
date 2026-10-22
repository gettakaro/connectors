#include "conan/command_probe.h"

#include "common.h"
#include "conan/chat.h"
#include "conan/hook_dispatch.h"
#include "conan/text.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "ue/reflect.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace conan {
namespace cmdprobe {



bool ParseCatalog(const std::string& text, Catalog& out, std::string& err) {
    JsonValue v;
    if (!takaro::ParseJson(text, v) || v.type != JsonValue::Object) {
        err = "catalog is not a JSON object";
        return false;
    }
    Catalog c;
    c.revision = takaro::Str(v.get("revision")).value_or("");
    c.prefix = takaro::Lower(takaro::Str(v.get("prefix")).value_or(""));
    if (c.prefix.empty() || c.prefix.find(' ') != std::string::npos) {
        err = "catalog needs a prefix without spaces";
        return false;
    }
    const JsonValue* cmds = v.get("commands");
    if (!cmds || cmds->type != JsonValue::Array) {
        err = "catalog needs a commands array";
        return false;
    }
    for (const auto& cmd : cmds->arr) {
        const std::string trigger = takaro::Str(cmd.get("trigger")).value_or("");
        if (trigger.empty() || trigger.find(' ') != std::string::npos) continue;
        std::vector<std::string> names{trigger};
        if (const JsonValue* al = cmd.get("aliases"); al && al->type == JsonValue::Array)
            for (const auto& a : al->arr)
                if (a.isStr() && !a.str.empty() && a.str.find(' ') == std::string::npos) names.push_back(a.str);
        for (const auto& n : names) {
            if (!c.trigger.emplace(takaro::Lower(n), trigger).second) c.duplicates++;
        }
    }
    if (c.trigger.empty()) {
        err = "catalog has no usable commands";
        return false;
    }
    out = std::move(c);
    return true;
}

std::vector<std::string> SplitArgs(const std::string& raw) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (size_t i = 0; i < raw.size(); i++) {
        const char ch = raw[i];
        if (ch == '\\' && i + 1 < raw.size() && raw[i + 1] == '"') {
            cur += '"';
            any = true;
            i++;
        } else if (ch == '"') {
            quoted = !quoted;
            any = true;
        } else if (ch == ' ' && !quoted) {
            if (any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += ch;
            any = true;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

Classified Classify(const Catalog& c, const std::string& message) {
    Classified k;
    const std::string msg = takaro::Trim(message);
    if (c.prefix.empty() || msg.size() < c.prefix.size()) return k;
    if (takaro::Lower(msg.substr(0, c.prefix.size())) != c.prefix) return k;
    if (msg.size() > c.prefix.size() && msg[c.prefix.size()] != ' ') return k;
    k.namespaced = true;
    const std::string rest = takaro::Trim(msg.substr(c.prefix.size()));
    const size_t sp = rest.find(' ');
    k.word = rest.substr(0, sp);
    k.argsRaw = sp == std::string::npos ? "" : takaro::Trim(rest.substr(sp + 1));
    auto it = c.trigger.find(takaro::Lower(k.word));
    if (it == c.trigger.end()) return k;
    k.known = true;
    k.trigger = it->second;
    k.args = SplitArgs(k.argsRaw);
    return k;
}

std::string InvocationJson(const std::string& id, const std::string& revision, const Classified& k,
                           const events::PlayerId& p, const std::string& channel, int64_t atUnixMs) {
    std::string args = "[";
    for (size_t i = 0; i < k.args.size(); i++) args += (i ? "," : "") + JsonStr(k.args[i]);
    args += "]";
    return takaro::ObjBuilder()
        .S("id", id)
        .S("catalogRevision", revision)
        .S("trigger", k.trigger)
        .S("typed", k.word)
        .S("argsRaw", k.argsRaw)
        .Raw("args", args)
        .Raw("player", takaro::JsonDump(events::PlayerJson(p)))
        .S("channel", channel)
        .N("atMs", (double)atUnixMs)
        .Done();
}

namespace {
constexpr uint64_t kStubStaleMs = 3000;
constexpr uint64_t kReplyTimeoutMs = 5000;
constexpr uint64_t kLateWatchMs = 60000;
constexpr size_t kRecentTexts = 16;
constexpr size_t kMaxQueue = 64;

struct Offered {
    events::PlayerId p;
    bool identified = false;
    std::string channel;
    Classified k;
    std::string revision;
};
struct Pending {
    std::string id, steam64, trigger;
    uint64_t sentMs = 0;
};

struct State {
    std::atomic<bool> enabled{false};
    std::string dir, bootId;
    std::mutex mu;  // catalog, queue, recent
    std::condition_variable cv;
    std::shared_ptr<const Catalog> catalog;
    std::string catalogText, catalogError;
    std::deque<Offered> queue;
    std::deque<std::pair<std::string, uint64_t>> recent;  // claimed texts (game thread only)
    bool stopping = false;
    std::thread worker;
    uint64_t seq = 0;
    std::atomic<uint64_t> claimed{0}, passedNamespaced{0}, unidentified{0}, queueFull{0}, offline{0}, sent{0},
        replies{0}, timeouts{0}, lateDropped{0}, replyFailed{0}, broadcastsSeen{0}, claimedBroadcasts{0},
        catalogLoads{0};
};
State& S() {
    static State* s = new State;  // leaked: the worker may outlive static destruction
    return *s;
}

int64_t UnixMs() {
    int64_t sec;
    long nsec;
    WallClock(sec, nsec);
    return sec * 1000 + nsec / 1000000;
}

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    if (!takaro::RandomBytes(b.data(), b.size()))
        for (size_t i = 0; i < bytes; i++) b[i] = (unsigned char)((NowNs() >> (i % 8)) & 0xff);
    static const char* h = "0123456789abcdef";
    std::string out;
    for (unsigned char c : b) {
        out += h[c >> 4];
        out += h[c & 15];
    }
    return out;
}

void Reply(const std::string& steam64, const std::string& text) {
    ChatRequest r;
    r.recipient = steam64;
    r.sender = "Takaro";
    r.message = text;
    ChatOutcome o = SendChat(r);
    if (o.success) S().replies++;
    else {
        S().replyFailed++;
        NativeLog("command-probe: private reply to %s failed: %s", steam64.c_str(), o.error.c_str());
    }
}

bool StubOnline() {
    std::string text, err;
    bool exists = false;
    if (!takaro::ReadWholeFile(takaro::JoinPath(S().dir, "stub.json"), text, exists, err) || !exists) return false;
    JsonValue v;
    if (!takaro::ParseJson(text, v)) return false;
    auto hb = takaro::Num(v.get("heartbeatMs"));
    if (!hb) return false;
    const int64_t age = UnixMs() - (int64_t)*hb;
    return age >= -(int64_t)kStubStaleMs && age <= (int64_t)kStubStaleMs;
}

void ReloadCatalog() {
    std::string text, err;
    bool exists = false;
    if (!takaro::ReadWholeFile(takaro::JoinPath(S().dir, "catalog.json"), text, exists, err) || !exists) return;
    {
        std::lock_guard<std::mutex> g(S().mu);
        if (text == S().catalogText) return;
    }
    Catalog c;
    std::string why;
    // A half-written or broken catalog keeps the last good one, so known commands never go public.
    if (!ParseCatalog(text, c, why)) {
        std::lock_guard<std::mutex> g(S().mu);
        S().catalogText = text;
        S().catalogError = why;
        NativeLog("command-probe: catalog ignored: %s", why.c_str());
        return;
    }
    NativeLog("command-probe: catalog revision %s, prefix %s, %zu names, %d duplicates", c.revision.c_str(),
              c.prefix.c_str(), c.trigger.size(), c.duplicates);
    std::lock_guard<std::mutex> g(S().mu);
    S().catalogText = text;
    S().catalogError.clear();
    S().catalog = std::make_shared<const Catalog>(std::move(c));
    S().catalogLoads++;
}

void Handle(const Offered& o, std::vector<Pending>& pending) {
    if (!o.identified) return;  // claimed and counted in Offer; nobody to answer
    if (!StubOnline()) {
        S().offline++;
        NativeLog("command-probe: %s from %s failed: stub offline (not queued)", o.k.trigger.c_str(),
                  o.p.steam64.c_str());
        Reply(o.p.steam64, "Takaro is offline: " + o.k.trigger + " was not run. Try again later.");
        return;
    }
    Pending p;
    p.id = S().bootId + "-" + std::to_string(++S().seq);
    p.steam64 = o.p.steam64;
    p.trigger = o.k.trigger;
    p.sentMs = NowMs();
    std::string err;
    const std::string body = InvocationJson(p.id, o.revision, o.k, o.p, o.channel, UnixMs());
    if (!takaro::AtomicWriteFile(takaro::JoinPath(takaro::JoinPath(S().dir, "inbox"), p.id + ".json"), body, err)) {
        NativeLog("command-probe: invocation write failed: %s", err.c_str());
        Reply(o.p.steam64, "Takaro could not take " + o.k.trigger + "; it was not run.");
        return;
    }
    S().sent++;
    NativeLog("command-probe: sent %s %s from %s (%zu args)", p.id.c_str(), p.trigger.c_str(), p.steam64.c_str(),
              o.k.args.size());
    pending.push_back(p);
}

void Worker() {
    std::vector<Pending> pending;
    std::vector<std::pair<std::string, uint64_t>> expired;  // id, expiry time
    uint64_t nextCatalog = 0;
    for (;;) {
        std::deque<Offered> work;
        {
            std::unique_lock<std::mutex> lk(S().mu);
            S().cv.wait_for(lk, std::chrono::milliseconds(100), [] { return S().stopping || !S().queue.empty(); });
            if (S().stopping) return;
            work.swap(S().queue);
        }
        if (NowMs() >= nextCatalog) {
            ReloadCatalog();
            nextCatalog = NowMs() + 1000;
        }
        for (const auto& o : work) Handle(o, pending);
        const std::string outbox = takaro::JoinPath(S().dir, "outbox");
        for (size_t i = 0; i < pending.size();) {
            const Pending& p = pending[i];
            const std::string path = takaro::JoinPath(outbox, p.id + ".json");
            std::string text, err;
            bool exists = false;
            JsonValue v;
            if (takaro::ReadWholeFile(path, text, exists, err) && exists && takaro::ParseJson(text, v)) {
                takaro::RemoveFileIfExists(path);
                const JsonValue* r = v.get("replies");
                int n = 0;
                if (r && r->type == JsonValue::Array)
                    for (const auto& line : r->arr)
                        if (line.isStr() && !line.str.empty()) {
                            Reply(p.steam64, line.str);
                            n++;
                        }
                NativeLog("command-probe: %s answered with %d private lines after %llu ms", p.id.c_str(), n,
                          (unsigned long long)(NowMs() - p.sentMs));
                pending.erase(pending.begin() + (long)i);
            } else if (NowMs() - p.sentMs > kReplyTimeoutMs) {
                S().timeouts++;
                NativeLog("command-probe: %s timed out", p.id.c_str());
                Reply(p.steam64, "Takaro did not answer " + p.trigger + " in time; it may not have run.");
                expired.push_back({p.id, NowMs() + kLateWatchMs});
                pending.erase(pending.begin() + (long)i);
            } else {
                i++;
            }
        }
        for (size_t i = 0; i < expired.size();) {
            // RemoveFileIfExists also reports success for a missing file on Windows: stat first.
            const std::string late = takaro::JoinPath(outbox, expired[i].first + ".json");
            if (takaro::StatFile(late).exists && takaro::RemoveFileIfExists(late)) {
                S().lateDropped++;
                NativeLog("command-probe: late reply for %s dropped", expired[i].first.c_str());
            }
            if (NowMs() > expired[i].second) expired.erase(expired.begin() + (long)i);
            else i++;
        }
    }
}

// Game thread: every line the game itself sends to a client (the connector's own sends use the
// trampoline and are not seen here). A claimed text showing up would mean the veto leaked.
bool OnBroadcast(const HookDispatch::Call& c, void*) {
    if (!c.parms) return true;
    S().broadcastsSeen++;
    const std::string msg = takaro::Trim(UER::ReadFString((uintptr_t)c.parms + ChatRpc::kMessage, 4096));
    const uint64_t now = NowMs();
    for (const auto& r : S().recent)
        if (now - r.second < kLateWatchMs && r.first == msg) {
            S().claimedBroadcasts++;
            NativeLog("command-probe: LEAK: a claimed command was broadcast by the game");
            break;
        }
    return true;
}
}  // namespace

bool Enabled() { return S().enabled.load(std::memory_order_acquire); }

bool Offer(const events::PlayerId& p, bool identified, const std::string& channel, const std::string& message) {
    if (!Enabled()) return false;
    std::shared_ptr<const Catalog> cat;
    {
        std::lock_guard<std::mutex> g(S().mu);
        cat = S().catalog;
    }
    if (!cat) return false;
    Classified k = Classify(*cat, message);
    if (!k.known) {
        if (k.namespaced) S().passedNamespaced++;
        return false;
    }
    S().claimed++;
    if (!identified) S().unidentified++;
    std::lock_guard<std::mutex> g(S().mu);
    S().recent.push_back({takaro::Trim(message), NowMs()});
    if (S().recent.size() > kRecentTexts) S().recent.pop_front();
    if (S().queue.size() >= kMaxQueue) {
        S().queueFull++;  // still claimed: a flood must not leak into public chat
        return true;
    }
    Offered o;
    o.p = p;
    o.identified = identified;
    o.channel = channel;
    o.k = std::move(k);
    o.revision = cat->revision;
    S().queue.push_back(std::move(o));
    S().cv.notify_one();
    return true;
}

void Start() {
    const char* dir = getenv("TAKARO_CONAN_COMMAND_PROBE");
    if (!dir || !*dir) return;
    State& s = S();
    s.dir = dir;
    s.bootId = RandomHex(4);
    std::string err;
    takaro::EnsureDirectory(takaro::JoinPath(s.dir, "inbox"), err);
    takaro::EnsureDirectory(takaro::JoinPath(s.dir, "outbox"), err);
    ReloadCatalog();
    HookDispatch::Subscription b;
    b.owner = "command-probe";
    b.baseClass = "ConanPlayerController";
    b.function = "ClientReceiveChatMessage";
    b.phase = HookDispatch::Phase::Before;
    b.fn = OnBroadcast;
    HookDispatch::Subscribe(b);
    s.worker = std::thread(Worker);
    s.enabled.store(true, std::memory_order_release);
    NativeLog("command-probe: RESEARCH PROBE ENABLED (dir %s, boot %s)", s.dir.c_str(), s.bootId.c_str());
}

void Stop() {
    State& s = S();
    if (!s.enabled.exchange(false)) return;
    {
        std::lock_guard<std::mutex> g(s.mu);
        s.stopping = true;
    }
    s.cv.notify_all();
    if (s.worker.joinable()) s.worker.join();
}

std::string HealthJson() {
    State& s = S();
    if (!Enabled()) return "null";
    std::string revision, error;
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (s.catalog) revision = s.catalog->revision;
        error = s.catalogError;
    }
    auto n = [](const std::atomic<uint64_t>& a) { return (double)a.load(std::memory_order_relaxed); };
    return takaro::ObjBuilder()
        .S("bootId", s.bootId)
        .S("catalogRevision", revision)
        .S("catalogError", error)
        .N("catalogLoads", n(s.catalogLoads))
        .N("claimed", n(s.claimed))
        .N("passedNamespaced", n(s.passedNamespaced))
        .N("unidentified", n(s.unidentified))
        .N("queueFull", n(s.queueFull))
        .N("offline", n(s.offline))
        .N("sent", n(s.sent))
        .N("replies", n(s.replies))
        .N("replyFailed", n(s.replyFailed))
        .N("timeouts", n(s.timeouts))
        .N("lateDropped", n(s.lateDropped))
        .N("broadcastsSeen", n(s.broadcastsSeen))
        .N("claimedBroadcasts", n(s.claimedBroadcasts))
        .Done();
}

}  // namespace cmdprobe
}  // namespace conan
