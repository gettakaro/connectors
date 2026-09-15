#include "hooks.h"
#include "moderation.h"
#include "world.h"
#include "scan.h"
#include "state.h"

#include "MinHook.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>

namespace {

struct Resolved {
    std::string name;
    uint32_t rva = 0;
    std::string how;
    bool hooked = false;
};
SrwLock g_diagLock;
std::vector<Resolved> g_resolved;

void Record(const std::string& name, uint32_t rva, const std::string& how, bool hooked) {
    Guard g(g_diagLock);
    g_resolved.push_back({name, rva, how, hooked});
    PluginLog("resolve %-12s rva=0x%x hooked=%d (%s)", name.c_str(), rva, hooked ? 1 : 0, how.c_str());
}

// ------------------------------------------------------------------------------------------------
// Log sink:  void sink(uint8 threshold /*cl*/, uint8 level /*dl*/, const TextRef* text /*r8*/)
// Called by keen::log after formatting, before level filtering. Text includes a trailing '\n'.
struct TextRef {
    const char* ptr;
    uint64_t len;
};
using SinkFn = uint64_t(__fastcall*)(uint64_t, uint64_t, const TextRef*, uint64_t);
SinkFn g_origSink = nullptr;
thread_local int t_inSink = 0;

uint64_t __fastcall SinkDetour(uint64_t threshold, uint64_t level, const TextRef* text, uint64_t r9) {
    uint64_t ret = g_origSink(threshold, level, text, r9);
    if (t_inSink) return ret;
    t_inSink++;
    uint8_t th = (uint8_t)threshold, lv = (uint8_t)level;
    if (text && text->ptr && text->len > 0 && text->len < (1u << 20) && th >= lv) {
        std::string s(text->ptr, (size_t)text->len);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        // the sink may receive multiple lines at once
        size_t start = 0;
        while (start <= s.size()) {
            size_t nl = s.find('\n', start);
            std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty()) PluginState::Get().OnLogLine(lv, line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    t_inSink--;
    return ret;
}

// ------------------------------------------------------------------------------------------------
// Game-thread tick: session update (the function that calls the "-------------- Session" stats dump).
using TickFn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
TickFn g_origTick = nullptr;

struct GameTask {
    std::function<std::string()> fn;
    std::string result;
    HANDLE done = nullptr;
    ~GameTask() {
        if (done) CloseHandle(done);
    }
};
SrwLock g_queueLock;
std::deque<std::shared_ptr<GameTask>> g_queue;
volatile LONG64 g_tickCount = 0;
volatile LONG g_tickThread = 0;
volatile LONG g_tickThreadChanges = 0;

uint64_t __fastcall TickDetour(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t ret = g_origTick(a, b, c, d);
    InterlockedIncrement64(&g_tickCount);
    LONG tid = (LONG)GetCurrentThreadId();
    LONG prev = InterlockedExchange(&g_tickThread, tid);
    if (prev != 0 && prev != tid) InterlockedIncrement(&g_tickThreadChanges);
    for (int budget = 0; budget < 16; budget++) {
        std::shared_ptr<GameTask> task;
        {
            Guard g(g_queueLock);
            if (g_queue.empty()) break;
            task = g_queue.front();
            g_queue.pop_front();
        }
        task->result = task->fn();
        SetEvent(task->done);
    }
    return ret;
}

bool Hook(uint32_t rva, LPVOID detour, LPVOID* orig, std::string& err) {
    if (!scan::InText(rva)) {
        err = "target not in executable section";
        return false;
    }
    MH_STATUS s = MH_CreateHook(scan::Ptr(rva), detour, orig);
    if (s != MH_OK) {
        err = std::string("MH_CreateHook: ") + MH_StatusToString(s);
        return false;
    }
    s = MH_EnableHook(scan::Ptr(rva));
    if (s != MH_OK) {
        err = std::string("MH_EnableHook: ") + MH_StatusToString(s);
        MH_RemoveHook(scan::Ptr(rva));
        return false;
    }
    return true;
}

char g_hex[32];
const char* Hex(uint32_t v) {
    snprintf(g_hex, sizeof g_hex, "0x%x", v);
    return g_hex;
}

// Logger = most common E8 target shortly after `lea reg, <fmt>` across known log format strings.
uint32_t ResolveLogger(std::string& how) {
    static const char* anchors[] = {
        "[online] Added peer %k (%llu)", "[online] Removed peer %k", "[server] Remove Player '%k'",
        "[online] Server connected to Steam successfully", "-------------- Session ----------------",
        "[session] Player removed. Player handle: %k",
    };
    std::map<uint32_t, int> votes;
    int anchorsFound = 0;
    for (auto* a : anchors) {
        uint32_t s = scan::FindCString(a);
        if (!s) continue;
        anchorsFound++;
        for (uint32_t x : scan::FindLeaXrefs(s)) {
            // first call after the lea (within 0x100 bytes) is the logger call
            auto calls = scan::CallTargetsIn(x, x + 0x100);
            if (!calls.empty()) votes[calls.front()]++;
        }
    }
    uint32_t best = 0;
    int bestVotes = 0;
    for (auto& kv : votes)
        if (kv.second > bestVotes) {
            best = kv.first;
            bestVotes = kv.second;
        }
    char b[128];
    snprintf(b, sizeof b, "anchors=%d votes=%d", anchorsFound, bestVotes);
    how = b;
    return bestVotes >= 3 ? best : 0;
}

// Sink = call target inside the logger whose prologue matches the sink shape.
uint32_t ResolveSink(uint32_t logger, std::string& how) {
    static const char* kSinkPattern = "40 53 55 56 48 83 EC ?? 49 83 78 08 00";
    uint32_t end = 0;
    uint32_t root = scan::FunctionRoot(logger, &end);
    if (root != logger || !end) {
        how = "logger not a pdata function start";
        return 0;
    }
    for (uint32_t t : scan::CallTargetsIn(logger, end)) {
        if (scan::MatchAt(t, kSinkPattern)) {
            how = std::string("sink pattern matched in logger call targets");
            return t;
        }
    }
    how = "no call target in logger matched sink pattern";
    return 0;
}

uint32_t ResolveTick(std::string& how) {
    uint32_t s = scan::FindCString("-------------- Session ----------------");
    if (!s) {
        how = "session stats string missing";
        return 0;
    }
    auto xr = scan::FindLeaXrefs(s);
    if (xr.size() != 1) {
        how = "session stats string xrefs=" + std::to_string(xr.size());
        return 0;
    }
    uint32_t stats = scan::FunctionRoot(xr[0]);
    if (!stats) {
        how = "no pdata for stats function";
        return 0;
    }
    auto callers = scan::FindDirectCalls(stats);
    if (callers.size() != 1) {
        how = "stats function direct callers=" + std::to_string(callers.size());
        return 0;
    }
    uint32_t tick = scan::FunctionRoot(callers[0]);
    how = std::string("session update calling stats fn ") + Hex(stats);
    return tick;
}

// ------------------------------------------------------------------------------------------------
// Moderation (see research/re-moderation.md).
//   handleAccountAction(Server* server, u64 accountId, u8 type /*Kick=0,Ban=1,UnBan=2*/)  (0x683510 @1024233)
//   moderation system  sys(ctx, ?, Server* server /*r8*/, time /*r9*/) calls it per AccountActionEvent (0x69cd40)
// We hook the moderation system: it hands us the live `server` pointer in r8, and runs on the thread/phase
// where the game itself calls handleAccountAction. Queued moderation tasks are drained right after the
// original returns, so a programmatic kick/ban/unban is indistinguishable from the in-game admin action.
using AccountActionFn = void(__fastcall*)(uint64_t server, uint64_t accountId, uint64_t type);
using ModSysFn = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
AccountActionFn g_accountAction = nullptr;
ModSysFn g_origModSys = nullptr;
volatile LONG64 g_modTicks = 0;
volatile LONG64 g_server = 0;
SrwLock g_modLock;
std::deque<std::shared_ptr<GameTask>> g_modQueue;

uint64_t __fastcall ModSysDetour(uint64_t a, uint64_t b, uint64_t server, uint64_t d) {
    uint64_t ret = g_origModSys(a, b, server, d);
    InterlockedIncrement64(&g_modTicks);
    // the original bails out early unless these server members are set; require the same before acting
    if (server && *(uint64_t*)(server + 0x10) && *(uint64_t*)(server + 0x38)) InterlockedExchange64(&g_server, (LONG64)server);
    for (int budget = 0; budget < 8; budget++) {
        std::shared_ptr<GameTask> task;
        {
            Guard g(g_modLock);
            if (g_modQueue.empty()) break;
            task = g_modQueue.front();
            g_modQueue.pop_front();
        }
        task->result = task->fn();
        SetEvent(task->done);
    }
    return ret;
}

uint32_t ResolveAccountAction(std::string& how) {
    uint32_t s = scan::FindCString("[server] Account %s/%s was unbanned.");
    if (!s) {
        how = "unban log string missing";
        return 0;
    }
    std::vector<uint32_t> roots;
    for (uint32_t x : scan::FindLeaXrefs(s)) {
        uint32_t r = scan::FunctionRoot(x);
        if (r && std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(r);
    }
    if (roots.size() != 1) {
        how = "unban string function roots=" + std::to_string(roots.size());
        return 0;
    }
    // self-check: prologue shape + `cmp r8b, 2` (UnBan) near the top
    if (!scan::MatchAt(roots[0], "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? FF FF") ||
        !scan::MatchAt(roots[0] + 0x2f, "41 80 F8 02")) {
        how = std::string("handler prologue self-check failed at ") + Hex(roots[0]);
        return 0;
    }
    how = "unban log string -> function root, prologue+cmp r8b,2 verified";
    return roots[0];
}

uint32_t ResolveModerationSystem(uint32_t handler, std::string& how) {
    std::vector<uint32_t> roots;
    uint32_t site = 0;
    for (uint32_t c : scan::FindDirectCalls(handler)) {
        uint32_t r = scan::FunctionRoot(c);
        if (r && std::find(roots.begin(), roots.end(), r) == roots.end()) {
            roots.push_back(r);
            site = c;
        }
    }
    if (roots.size() != 1) {
        how = "handler caller roots=" + std::to_string(roots.size());
        return 0;
    }
    // self-check: server arrives in r8 (`cmp qword [r8+38h],0`), and the call site passes it (`mov rcx,r14`).
    if (!scan::MatchAt(roots[0], "48 89 4C 24 08 55 53 56 57 41 55 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 49 83 78 38 00 49 8B F1 4D 8B F0") ||
        !scan::MatchAt(site - 7, "49 8B CE 48 8B 52 08")) {
        how = std::string("moderation system self-check failed at ") + Hex(roots[0]);
        return 0;
    }
    how = std::string("unique caller of handler, call site ") + Hex(site) + " verified";
    return roots[0];
}

// ------------------------------------------------------------------------------------------------
// Graceful shutdown: the server's own console control handler (registered via SetConsoleCtrlHandler) sets the
// gameflow quit state and waits for the save; invoking it reproduces Ctrl-C / docker stop exactly.
using CtrlHandlerFn = BOOL(WINAPI*)(DWORD);
CtrlHandlerFn g_ctrlHandler = nullptr;

uint32_t ResolveCtrlHandler(std::string& how) {
    uint32_t slot = scan::FindImportSlot("KERNEL32.dll", "SetConsoleCtrlHandler");
    if (!slot) {
        how = "SetConsoleCtrlHandler import slot missing";
        return 0;
    }
    auto sites = scan::FindIndirectCalls(slot);
    for (uint32_t site : sites) {
        for (uint32_t back = 7; back <= 0x14; back++) {  // lea rcx, [rip+handler] shortly before the call
            if (!scan::MatchAt(site - back, "48 8D 0D")) continue;
            int32_t disp;
            memcpy(&disp, scan::Ptr(site - back + 3), 4);
            uint32_t h = (uint32_t)((int64_t)site - back + 7 + disp);
            if (scan::InText(h) &&
                scan::MatchAt(h, "48 83 EC 28 48 8B 05 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 80 B8 ?? ?? 00 00 00 0F 85")) {
                how = std::string("SetConsoleCtrlHandler call site ") + Hex(site) + ", handler shape verified";
                return h;
            }
        }
    }
    how = "no SetConsoleCtrlHandler call site with the game handler shape (sites=" + std::to_string(sites.size()) + ")";
    return 0;
}

std::string g_gameBuild;
uint32_t g_modSysRva = 0;

}  // namespace

bool InstallHook(uint32_t rva, LPVOID detour, LPVOID* orig, std::string& err) { return Hook(rva, detour, orig, err); }
void RecordResolve(const std::string& name, uint32_t rva, const std::string& how, bool hooked) { Record(name, rva, how, hooked); }
uint64_t CapturedServer() { return (uint64_t)g_server; }
uint32_t ModerationSystemRva() { return g_modSysRva; }


moderation::ProtectLayout g_protect;

// Machine handle (session table +0x190) of the unique active record with this account hash, 0 if none/ambiguous.
static uint32_t MachineHandleForAccount(uint64_t server, uint64_t accountId) {
    uint64_t mgr = *(uint64_t*)(server + 0x10);
    if (!mgr) return 0;
    uint32_t ver = *(uint32_t*)(mgr + 0x3c);
    if (ver > 1) return 0;
    uint64_t base = mgr + 0x48 + (uint64_t)ver * 0x2578;
    uint32_t found = 0;
    int matches = 0;
    for (int i = 0; i < 64; i++) {
        uint64_t rec = base + (uint64_t)i * 0x140;
        uint32_t h = *(uint32_t*)(rec + 0x188);
        if (!h || (h & 0x3f) != (uint32_t)i) continue;
        if (*(uint64_t*)(rec + 0x198) == accountId) {
            found = *(uint32_t*)(rec + 0x190);
            matches++;
        }
    }
    return matches == 1 ? found : 0;
}

bool RunOnModerationThread(std::function<std::string(uint64_t server)> fn, std::string& result, DWORD timeoutMs) {
    if (!g_origModSys || !g_server) return false;
    auto task = std::make_shared<GameTask>();
    task->fn = [fn]() { return fn((uint64_t)g_server); };
    task->done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    {
        Guard g(g_modLock);
        g_modQueue.push_back(task);
    }
    if (WaitForSingleObject(task->done, timeoutMs) != WAIT_OBJECT_0) return false;
    result = task->result;
    return true;
}

bool ModerationReady() { return g_accountAction && g_origModSys && g_server; }

bool AccountAction(uint64_t accountId, uint8_t type, std::string& err, DWORD timeoutMs, int* httpStatus,
                   bool* bypassedAdminProtection) {
    if (httpStatus) *httpStatus = 503;
    if (bypassedAdminProtection) *bypassedAdminProtection = false;
    if (!g_accountAction || !g_origModSys) {
        err = "moderation signatures not resolved";
        return false;
    }
    if (!g_server) {
        err = "server object not captured yet (moderation system has not run)";
        return false;
    }
    auto task = std::make_shared<GameTask>();
    task->fn = [accountId, type]() -> std::string {
        uint64_t server = (uint64_t)g_server;
        int slot = -1;
        bool prot = false;
        if (type != 2) {
            // Kick/Ban: the handler acts on the player slot of the account's machine; find it the same way.
            if (!g_protect.ok()) return std::string("!admin-protection layout unresolved (handler self-check failed)");
            uint32_t mh = MachineHandleForAccount(server, accountId);
            slot = moderation::FindSlotByMachine((const uint8_t*)server, g_protect, mh);
            if (slot < 0) return std::string("!no player slot for this account (the game would ignore the action)");
            prot = (*moderation::ProtByte((uint8_t*)server, g_protect, slot) & 1) != 0;
        }
        PluginLog("moderation: handleAccountAction(server=%p, account=%llu, type=%u, slot=%d, canKickBanTarget=%d)",
                  (void*)server, (unsigned long long)accountId, (unsigned)type, slot, prot ? 1 : 0);
        if (prot) {
            // The game skips targets whose group has canKickBan. Clear the bit only around this synchronous call.
            uint8_t* b = moderation::ProtByte((uint8_t*)server, g_protect, slot);
            *b &= (uint8_t)~1u;
            g_accountAction(server, accountId, type);
            *b |= 1u;
            return std::string("bypass");
        }
        g_accountAction(server, accountId, type);
        return std::string("ok");
    };
    task->done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    {
        Guard g(g_modLock);
        g_modQueue.push_back(task);
    }
    if (WaitForSingleObject(task->done, timeoutMs) != WAIT_OBJECT_0) {
        err = "moderation system did not run the task in time";
        return false;
    }
    if (!task->result.empty() && task->result[0] == '!') {
        err = task->result.substr(1);
        if (httpStatus) *httpStatus = task->result.find("no player slot") != std::string::npos ? 409 : 503;
        return false;
    }
    if (bypassedAdminProtection) *bypassedAdminProtection = task->result == "bypass";
    return true;
}

static DWORD WINAPI ShutdownThread(LPVOID) {
    Sleep(500);  // let the HTTP response go out first
    PluginLog("shutdown: invoking game console control handler (CTRL_C_EVENT)");
    g_ctrlHandler(CTRL_C_EVENT);
    return 0;
}

bool RequestShutdown(std::string& err) {
    if (!g_ctrlHandler) {
        err = "console control handler not resolved";
        return false;
    }
    HANDLE h = CreateThread(nullptr, 0, ShutdownThread, nullptr, 0, nullptr);
    if (!h) {
        err = "CreateThread failed";
        return false;
    }
    CloseHandle(h);
    return true;
}

std::string GameBuild() { return g_gameBuild; }

static void ReadGameBuild() {
    std::string path = PluginBaseDir() + "\\enshrouded_server.kfc";
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        std::string s(buf, n);
        size_t p = s.find("|^/");
        if (p != std::string::npos) {
            size_t b = p;
            while (b > 0 && s[b - 1] >= '0' && s[b - 1] <= '9') b--;
            size_t e = s.find('\0', p);
            g_gameBuild = s.substr(b, p - b);
            std::string full = s.substr(b, e == std::string::npos ? 80 : e - b);
            PluginLog("game build: %s", full.c_str());
        }
    }
    if (g_gameBuild.empty()) g_gameBuild = "unknown";
}

void HooksInit() {
    auto& st = PluginState::Get();
    WorldInitCapabilities();
    st.SetCapability("logEvents", "degraded", "initializing");
    st.SetCapability("players", "degraded", "initializing");
    st.SetCapability("gameThread", "degraded", "initializing");
    for (auto* c : {"kick", "ban", "unban", "shutdown"}) st.SetCapability(c, "degraded", "initializing");
    st.SetCapability("listBans", "ok", "reads bannedAccounts from enshrouded_server.json");

    ReadGameBuild();
    if (!scan::Init()) {
        st.SetCapability("logEvents", "degraded", "PE scan init failed");
        st.SetCapability("players", "degraded", "PE scan init failed");
        st.SetCapability("gameThread", "degraded", "PE scan init failed");
        return;
    }
    MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK) {
        std::string e = std::string("MH_Initialize: ") + MH_StatusToString(ms);
        st.SetCapability("logEvents", "degraded", e);
        st.SetCapability("players", "degraded", e);
        st.SetCapability("gameThread", "degraded", e);
        return;
    }

    // --- logger sink ---
    std::string how;
    uint32_t logger = ResolveLogger(how);
    Record("logger", logger, how, false);
    uint32_t sink = 0;
    if (logger) {
        sink = ResolveSink(logger, how);
        Record("logSink", sink, how, false);
    }
    if (sink) {
        std::string err;
        if (Hook(sink, (LPVOID)&SinkDetour, (LPVOID*)&g_origSink, err)) {
            Record("logSink", sink, "hook installed", true);
            st.SetCapability("logEvents", "ok");
            st.SetCapability("players", "ok", "log-derived (online peer + login lines)");
        } else {
            st.SetCapability("logEvents", "degraded", err);
            st.SetCapability("players", "degraded", err);
        }
    } else {
        st.SetCapability("logEvents", "degraded", "log sink not resolved: " + how);
        st.SetCapability("players", "degraded", "log sink not resolved");
    }

    // --- game thread tick ---
    uint32_t tick = ResolveTick(how);
    Record("sessionTick", tick, how, false);
    if (tick) {
        std::string err;
        if (Hook(tick, (LPVOID)&TickDetour, (LPVOID*)&g_origTick, err)) {
            Record("sessionTick", tick, "hook installed", true);
            st.SetCapability("gameThread", "degraded", "hook installed, waiting for first tick");
        } else {
            st.SetCapability("gameThread", "degraded", err);
        }
    } else {
        st.SetCapability("gameThread", "degraded", "tick not resolved: " + how);
    }

    // --- moderation ---
    uint32_t handler = ResolveAccountAction(how);
    Record("accountAction", handler, how, false);
    std::string modErr;
    if (handler) {
        uint32_t sys = ResolveModerationSystem(handler, how);
        Record("moderationSys", sys, how, false);
        if (sys) {
            g_modSysRva = sys;
            if (Hook(sys, (LPVOID)&ModSysDetour, (LPVOID*)&g_origModSys, modErr)) {
                g_accountAction = (AccountActionFn)scan::Ptr(handler);
                Record("moderationSys", sys, "hook installed", true);
                // the handler's .pdata is split into chained chunks, so scan a fixed window (anchors must be unique in it)
                g_protect = moderation::ParseHandler((const uint8_t*)scan::Ptr(handler), 0x1000);
                char pb[160];
                snprintf(pb, sizeof pb, "canKickBan skip test: flags+0x%x bit0, machineHandle+0x%x, stride 0x%x%s",
                         g_protect.protOff, g_protect.mhOff, g_protect.slotStride,
                         g_protect.ok() ? "" : " (self-check FAILED: kick/ban refuse to run)");
                Record("adminProtect", handler, pb, false);
            }
        } else {
            modErr = "moderation system not resolved: " + how;
        }
    } else {
        modErr = "account action handler not resolved: " + how;
    }
    for (auto* c : {"kick", "ban", "unban"})
        st.SetCapability(c, "degraded", g_origModSys ? "hook installed, waiting for moderation system to run" : modErr);

    // --- shutdown ---
    uint32_t ctrl = ResolveCtrlHandler(how);
    Record("ctrlHandler", ctrl, how, false);
    if (ctrl) {
        g_ctrlHandler = (CtrlHandlerFn)scan::Ptr(ctrl);
        st.SetCapability("shutdown", "ok", "game console control handler (graceful save + quit)");
    } else {
        st.SetCapability("shutdown", "degraded", how);
    }

    // --- world / items / chat / combat (world.cpp) ---
    WorldHooksInit();
}

bool RunOnGameThread(std::function<std::string()> fn, std::string& result, DWORD timeoutMs) {
    if (!g_origTick || g_tickCount == 0) return false;
    auto task = std::make_shared<GameTask>();
    task->fn = std::move(fn);
    task->done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    {
        Guard g(g_queueLock);
        g_queue.push_back(task);
    }
    if (WaitForSingleObject(task->done, timeoutMs) != WAIT_OBJECT_0) return false;
    result = task->result;
    return true;
}

void HooksHousekeep() {
    static LONG64 lastCount = -1;
    if (g_origTick) {
        LONG64 c = g_tickCount;
        if (c > 0 && lastCount != c && PluginState::Get().CapabilitiesJson().find("\"gameThread\":\"ok\"") == std::string::npos)
            PluginState::Get().SetCapability("gameThread", "ok", "session update tick hooked");
        lastCount = c;
    }
    WorldHousekeep();
    static bool modOk = false;
    if (!modOk && ModerationReady()) {
        modOk = true;
        for (auto* c : {"kick", "ban", "unban"})
            PluginState::Get().SetCapability(c, "ok", "handleAccountAction via moderation system hook");
    }
}

std::string HookDiagnosticsJson() {
    std::string o = "{\"tickCount\":" + std::to_string((long long)g_tickCount) +
                     ",\"world\":" + WorldDiagnosticsJson() + ",\"moderationTicks\":" + std::to_string((long long)g_modTicks) +
                     ",\"serverCaptured\":" + (g_server ? "true" : "false") +
                     ",\"tickThreadId\":" + std::to_string((long)g_tickThread) +
                     ",\"tickThreadChanges\":" + std::to_string((long)g_tickThreadChanges) +
                     ",\"logLines\":" + std::to_string(PluginState::Get().logLines) + ",\"imageTimestamp\":" +
                     JsonStr(Hex(scan::TimeDateStamp())) + ",\"resolved\":[";
    Guard g(g_diagLock);
    for (size_t i = 0; i < g_resolved.size(); i++) {
        auto& r = g_resolved[i];
        char rva[24];
        snprintf(rva, sizeof rva, "0x%x", r.rva);
        o += (i ? "," : "") + std::string("{\"name\":") + JsonStr(r.name) + ",\"rva\":" + JsonStr(rva) +
             ",\"hooked\":" + (r.hooked ? "true" : "false") + ",\"how\":" + JsonStr(r.how) + "}";
    }
    return o + "]}";
}
