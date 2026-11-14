#include "conan/blueprint_probe.h"

#include "common.h"
#include "conan/actions.h"
#include "conan/actions_ue.h"
#include "conan/events_payload.h"
#include "conan/chat.h"
#include "conan/hook_dispatch.h"
#include "conan/logtail.h"
#include "conan/text.h"
#include "gamethread.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

namespace conan {
namespace bpprobe {

namespace {
constexpr const char* kMarker = "Command not recognized: ";
constexpr const char* kVerbRoot = "takaro";

std::string NextWord(const std::string& s, size_t& pos) {
    while (pos < s.size() && s[pos] == ' ') pos++;
    const size_t start = pos;
    while (pos < s.size() && s[pos] != ' ') pos++;
    return s.substr(start, pos - start);
}
std::string RestOf(const std::string& s, size_t pos) {
    while (pos < s.size() && s[pos] == ' ') pos++;
    return takaro::Trim(s.substr(pos));
}
bool IsSteam64(const std::string& s) {
    if (s.size() != 17) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return s.compare(0, 4, "7656") == 0;
}
}  // namespace

Request ParseLogLine(const std::string& line) {
    Request r;
    const size_t at = line.find(kMarker);
    if (at == std::string::npos) return r;
    const std::string cmd = takaro::Trim(line.substr(at + strlen(kMarker)));
    size_t pos = 0;
    if (takaro::Lower(NextWord(cmd, pos)) != kVerbRoot) return r;
    r.verb = takaro::Lower(NextWord(cmd, pos));
    if (r.verb == "run") {
        r.steam64 = NextWord(cmd, pos);
        r.text = RestOf(cmd, pos);
        if (!IsSteam64(r.steam64)) r.error = "run needs a Steam64 player id";
        else if (r.text.empty()) r.error = "run needs a command text";
    } else if (r.verb == "event") {
        std::string who = NextWord(cmd, pos);
        r.name = NextWord(cmd, pos);
        r.json = RestOf(cmd, pos);
        if (who != "-") r.steam64 = who;
        if (who != "-" && !IsSteam64(who)) r.error = "event needs a Steam64 player id or -";
        else if (r.name.empty()) r.error = "event needs a name";
    } else {
        r.error = "unknown verb '" + r.verb + "'";
    }
    r.ok = r.error.empty();
    return r;
}

std::string RequestJson(const std::string& id, const Request& r, int64_t atUnixMs, uint64_t logLagMs) {
    return takaro::ObjBuilder()
        .S("id", id)
        .S("verb", r.verb)
        .S("steam64", r.steam64)
        .S("text", r.text)
        .S("name", r.name)
        .S("json", r.json)
        .S("route", "console-log")
        .N("atMs", (double)atUnixMs)
        .N("logLagMs", (double)logLagMs)
        .Done();
}

namespace {
constexpr uint64_t kStubStaleMs = 3000;
constexpr uint64_t kReplyTimeoutMs = 5000;

struct Pending {
    std::string id, steam64;
    uint64_t sentMs = 0;
};
struct Driven {
    std::string command;
    uint64_t calledMs = 0;
};

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<bool> stopping{false};
    std::string dir, bootId, logPath;
    std::thread worker;
    uint64_t seq = 0;
    std::mutex mu;
    std::deque<Driven> driven;  // stand-in calls waiting for their log line (lag measurement)
    std::string lastDrive;
    std::atomic<uint64_t> linesSeen{0}, requests{0}, malformed{0}, offline{0}, sent{0}, replies{0}, replyFailed{0},
        timeouts{0}, drives{0}, driveFailed{0}, oracleHits{0};
    std::atomic<uint64_t> lastLagMs{0}, maxLagMs{0}, lastDriveUs{0}, maxDriveUs{0};
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
    if (steam64.empty()) return;
    ChatRequest r;
    r.recipient = steam64;
    r.sender = "Takaro";
    r.message = text;
    ChatOutcome o = SendChat(r);
    if (o.success) S().replies++;
    else {
        S().replyFailed++;
        NativeLog("blueprint-probe: private reply to %s failed: %s", steam64.c_str(), o.error.c_str());
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

// The stand-in for a Blueprint node: the same KismetSystemLibrary.ExecuteConsoleCommand call
// with the arguments a mod would pass. Game thread for the call only; file I/O stays here.
void Drive(const std::string& name, const std::string& command, const std::string& mode) {
    struct Out {
        bool ok = false;
        std::string error, context;
        int64_t frame = -1;
        uint64_t ns = 0;
    } out;
    rx::Lookups l;
    std::string err;
    const takaro::FileStat before = takaro::StatFile(S().logPath);
    if (!rx::EnsureWorld(err) || !rx::EnsureLookups(l, err)) {
        out.error = err;
    } else {
        std::u16string cmd = ChatText(command, 1024);
        {
            std::lock_guard<std::mutex> g(S().mu);
            S().driven.push_back({takaro::Trim(command), NowMs()});
            if (S().driven.size() > 32) S().driven.pop_front();
        }
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                const uintptr_t lib = l.systemLibrary;
                rx::Func exec, frameFn;
                if (!rx::ResolveFunc(rx::ClassOf(lib), "ExecuteConsoleCommand",
                                     {{"WorldContextObject", "ObjectProperty", 8},
                                      {"Command", "StrProperty", 16},
                                      {"SpecificPlayer", "ObjectProperty", 8}},
                                     exec, out.error) ||
                    !rx::ResolveFunc(rx::ClassOf(lib), "GetFrameCount", {{"ReturnValue", "Int64Property", 8}}, frameFn,
                                     out.error))
                    return;
                rx::Frame fc(frameFn);
                if (rx::Call(lib, frameFn, fc, &out.error)) out.frame = fc.GetInt64("ReturnValue");
                auto pcs = rx::OnlinePcs();
                const uintptr_t pc = pcs.empty() ? 0 : pcs.front().pc;
                const uintptr_t world = pc ? pc : UE::LiveGameState();
                if (!world) {
                    out.error = "no live world";
                    return;
                }
                rx::Param admin;
                uintptr_t adminByte = 0;
                uint8_t saved = 0;
                if (mode == "pc-admin" && pc && rx::FindProperty(rx::ClassOf(pc), "m_IsAdmin", "BoolProperty", 1, admin)) {
                    adminByte = pc + (uintptr_t)admin.offset + admin.boolByteOffset;
                    saved = rx::Rd<uint8_t>(adminByte);
                    rx::Wr<uint8_t>(adminByte, (uint8_t)(saved | (admin.boolByteMask ? admin.boolByteMask : 1)));
                }
                rx::Frame f(exec);
                f.Obj("WorldContextObject", world).Str("Command", cmd).Obj("SpecificPlayer", mode == "none" ? 0 : pc);
                out.ok = rx::Call(lib, exec, f, &out.error);
                if (adminByte && rx::Alive(pc)) rx::Wr<uint8_t>(adminByte, saved);
                out.context = mode == "none" ? "no SpecificPlayer" : pc ? "player " + pcs.front().steam64 : "engine";
                out.ns = NowNs() - t0;
            },
            2000);
        if (!ran && out.error.empty()) out.error = "game thread did not respond";
    }
    // Let the log flush, then keep this call's frame lines as evidence.
    for (int i = 0; i < 6; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string text;
    const takaro::FileStat after = takaro::StatFile(S().logPath);
    if (after.exists) {
        const uint64_t from =
            before.exists && before.identity == after.identity && before.size <= after.size ? before.size : 0;
        takaro::ReadFileRange(S().logPath, from, std::min<uint64_t>(after.size - from, 64 * 1024), text);
    }
    std::string lines = "[";
    size_t n = 0;
    for (const auto& ln : LinesOfFrame(text, out.frame))
        if (n < 40) lines += (n++ ? "," : "") + JsonStr(ln);
    lines += "]";
    const uint64_t us = out.ns / 1000;
    S().lastDriveUs = us;
    if (us > S().maxDriveUs) S().maxDriveUs = us;
    if (out.ok) S().drives++;
    else S().driveFailed++;
    NativeLog("blueprint-probe: drive %s mode=%s '%s' -> %s, game-thread %llu us (%s)", name.c_str(), mode.c_str(),
              command.c_str(), out.ok ? "called" : "failed", (unsigned long long)us,
              out.ok ? out.context.c_str() : out.error.c_str());
    const std::string body = takaro::ObjBuilder()
                                 .S("name", name)
                                 .S("mode", mode)
                                 .S("command", command)
                                 .B("called", out.ok)
                                 .S("error", out.error)
                                 .S("context", out.context)
                                 .N("frame", (double)out.frame)
                                 .N("gameThreadUs", (double)us)
                                 .Raw("logLines", lines)
                                 .Done();
    std::string werr;
    takaro::AtomicWriteFile(takaro::JoinPath(S().dir, "drive-result-" + name + ".json"), body, werr);
}

void CheckDrive() {
    std::string text, err;
    bool exists = false;
    const std::string path = takaro::JoinPath(S().dir, "drive.json");
    if (!takaro::ReadWholeFile(path, text, exists, err) || !exists || text == S().lastDrive) return;
    S().lastDrive = text;
    JsonValue v;
    if (!takaro::ParseJson(text, v)) return;
    const std::string name = takaro::Str(v.get("name")).value_or("");
    const std::string command = takaro::Str(v.get("command")).value_or("");
    const std::string mode = takaro::Str(v.get("mode")).value_or("pc");
    if (name.empty() || command.empty()) return;
    Drive(name, command, mode);
}

void Handle(const Request& r, std::vector<Pending>& pending) {
    S().requests++;
    uint64_t lag = 0;
    {
        std::lock_guard<std::mutex> g(S().mu);
        // The newest stand-in call with this text (the same text may have been driven before
        // without producing a line, e.g. without the admin flag). The worker blocks ~600 ms in
        // Drive(), so the value is an upper bound.
        const std::string& key = r.verb == "run" ? r.text : r.name;
        for (auto it = S().driven.rbegin(); it != S().driven.rend(); ++it) {
            if (it->command.find(key) != std::string::npos) {
                lag = NowMs() - it->calledMs;
                S().driven.erase(std::next(it).base());
                break;
            }
        }
    }
    if (lag) {
        S().lastLagMs = lag;
        if (lag > S().maxLagMs) S().maxLagMs = lag;
    }
    if (!StubOnline()) {
        S().offline++;
        NativeLog("blueprint-probe: %s from %s failed: stub offline (not queued)", r.verb.c_str(), r.steam64.c_str());
        Reply(r.steam64, "Takaro is offline: the request was not run.");
        return;
    }
    Pending p;
    p.id = S().bootId + "-" + std::to_string(++S().seq);
    p.steam64 = r.steam64;
    p.sentMs = NowMs();
    std::string err;
    if (!takaro::AtomicWriteFile(takaro::JoinPath(takaro::JoinPath(S().dir, "inbox"), p.id + ".json"),
                                 RequestJson(p.id, r, UnixMs(), lag), err)) {
        NativeLog("blueprint-probe: request write failed: %s", err.c_str());
        return;
    }
    S().sent++;
    NativeLog("blueprint-probe: sent %s %s for %s (log lag %llu ms)", p.id.c_str(), r.verb.c_str(),
              r.steam64.empty() ? "-" : r.steam64.c_str(), (unsigned long long)lag);
    pending.push_back(p);
}

void Worker() {
    std::vector<Pending> pending;
    events::LineSplitter split;
    takaro::FileStat st = takaro::StatFile(S().logPath);
    uint64_t offset = st.exists ? st.size : 0;  // only lines written after the probe started
    std::string identity = st.identity;
    while (!S().stopping.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CheckDrive();
        st = takaro::StatFile(S().logPath);
        if (st.exists) {
            if (st.identity != identity || st.size < offset) {
                identity = st.identity;
                offset = 0;
            }
            if (st.size > offset) {
                std::string chunk;
                const uint64_t len = std::min<uint64_t>(st.size - offset, 256 * 1024);
                if (takaro::ReadFileRange(S().logPath, offset, len, chunk)) {
                    offset += chunk.size();
                    for (const auto& line : split.Feed(chunk)) {
                        S().linesSeen++;
                        Request r = ParseLogLine(line);
                        if (r.ok) Handle(r, pending);
                        else if (!r.error.empty()) {
                            S().malformed++;
                            NativeLog("blueprint-probe: malformed request: %s", r.error.c_str());
                        }
                    }
                }
            }
        }
        const std::string outbox = takaro::JoinPath(S().dir, "outbox");
        for (size_t i = 0; i < pending.size();) {
            const Pending& p = pending[i];
            const std::string path = takaro::JoinPath(outbox, p.id + ".json");
            std::string text, err;
            bool exists = false;
            JsonValue v;
            if (takaro::ReadWholeFile(path, text, exists, err) && exists && takaro::ParseJson(text, v)) {
                takaro::RemoveFileIfExists(path);
                int n = 0;
                if (const JsonValue* rs = v.get("replies"); rs && rs->type == JsonValue::Array)
                    for (const auto& line : rs->arr)
                        if (line.isStr() && !line.str.empty()) {
                            Reply(p.steam64, line.str);
                            n++;
                        }
                NativeLog("blueprint-probe: %s answered with %d private lines after %llu ms", p.id.c_str(), n,
                          (unsigned long long)(NowMs() - p.sentMs));
                pending.erase(pending.begin() + (long)i);
            } else if (NowMs() - p.sentMs > kReplyTimeoutMs) {
                S().timeouts++;
                NativeLog("blueprint-probe: %s timed out", p.id.c_str());
                Reply(p.steam64, "Takaro did not answer in time; the request may not have run.");
                pending.erase(pending.begin() + (long)i);
            } else {
                i++;
            }
        }
    }
}

bool CountOnly(const HookDispatch::Call&, void*) {
    S().oracleHits++;
    return true;
}

// Which calls reach ProcessEvent. HookDispatch health reports hits per subscription.
void SubscribeOracle() {
    struct O {
        const char* cls;
        const char* fn;
    };
    static const O kOracle[] = {
        // Native library functions that Blueprints call directly (EX_FinalFunction / EX_CallMath).
        // Expected: only the connector's own ProcessEvent calls show up.
        {"KismetSystemLibrary", "ExecuteConsoleCommand"},
        {"KismetSystemLibrary", "Delay"},
        {"KismetSystemLibrary", "K2_SetTimerDelegate"},
        {"KismetSystemLibrary", "PrintString"},
        {"KismetSystemLibrary", "IsValid"},
        {"GameplayStatics", "GetPlayerController"},
        {"GameplayStatics", "GetAllActorsOfClass"},
        {"KismetMathLibrary", "RandomFloatInRange"},
        {"KismetStringLibrary", "Concat_StrStr"},
        // Native -> Blueprint events: positive controls (the engine calls them through ProcessEvent).
        {"Actor", "ReceiveBeginPlay"},
        {"Actor", "ReceiveEndPlay"},
        {"ActorComponent", "ReceiveBeginPlay"},
        // The SDK mod (inert until it is loaded): a dispatcher-bound handler, a timer target, a custom
        // event called on the SDK actor, and the library function itself.
        {"BP_TakaroSDK_C", "OnTakaroRequestHandler"},
        {"BP_TakaroSDK_C", "OnTakaroTimer"},
        {"BP_TakaroSDK_C", "TakaroRequest"},
        {"BP_TakaroSDK_C", "OnTakaroReply"},
        {"BPL_Takaro_C", "RunCommandAsPlayer"},
    };
    for (const auto& o : kOracle) {
        HookDispatch::Subscription s;
        s.owner = "blueprint-probe";
        s.baseClass = o.cls;
        s.function = o.fn;
        s.phase = HookDispatch::Phase::After;
        s.fn = CountOnly;
        HookDispatch::Subscribe(s);
    }
}
}  // namespace

bool Enabled() { return S().enabled.load(std::memory_order_acquire); }

void Start(const std::string& savedDir) {
    const char* dir = getenv("TAKARO_CONAN_BLUEPRINT_PROBE");
    if (!dir || !*dir) return;
    State& s = S();
    s.dir = dir;
    s.bootId = RandomHex(4);
    s.logPath = ServerLogPath(savedDir);
    std::string err;
    takaro::EnsureDirectory(takaro::JoinPath(s.dir, "inbox"), err);
    takaro::EnsureDirectory(takaro::JoinPath(s.dir, "outbox"), err);
    if (EnvOr("TAKARO_CONAN_BLUEPRINT_PROBE_ORACLE", "1") != "0") SubscribeOracle();
    s.worker = std::thread(Worker);
    s.enabled.store(true, std::memory_order_release);
    NativeLog("blueprint-probe: RESEARCH PROBE ENABLED (dir %s, boot %s, log %s)", s.dir.c_str(), s.bootId.c_str(),
              s.logPath.c_str());
}

void Stop() {
    State& s = S();
    if (!s.enabled.exchange(false)) return;
    s.stopping = true;
    if (s.worker.joinable()) s.worker.join();
}

std::string HealthJson() {
    State& s = S();
    if (!Enabled()) return "null";
    auto n = [](const std::atomic<uint64_t>& a) { return (double)a.load(std::memory_order_relaxed); };
    return takaro::ObjBuilder()
        .S("bootId", s.bootId)
        .N("linesSeen", n(s.linesSeen))
        .N("requests", n(s.requests))
        .N("malformed", n(s.malformed))
        .N("offline", n(s.offline))
        .N("sent", n(s.sent))
        .N("replies", n(s.replies))
        .N("replyFailed", n(s.replyFailed))
        .N("timeouts", n(s.timeouts))
        .N("drives", n(s.drives))
        .N("driveFailed", n(s.driveFailed))
        .N("lastDriveUs", n(s.lastDriveUs))
        .N("maxDriveUs", n(s.maxDriveUs))
        .N("lastLogLagMs", n(s.lastLagMs))
        .N("maxLogLagMs", n(s.maxLagMs))
        .N("oracleHits", n(s.oracleHits))
        .Done();
}

}  // namespace bpprobe
}  // namespace conan
