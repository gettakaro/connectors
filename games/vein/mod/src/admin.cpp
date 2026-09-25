// Lane L3b: server-side admin grants. See admin.h for why Game.ini cannot do this.
#include "admin.h"

#include "actions_util.h"
#include "gamethread.h"
#include "perf.h"
#include "reflect.h"
#include "resolve.h"
#include "state.h"

#include <cstring>
#include <atomic>
#include <memory>
#include <set>

using UE::FName;
using UE::FString;
using UE::TArray;

namespace {

const uint64_t kPassIntervalMs = 60000;  // L9: safety cadence; a join triggers a pass immediately
std::atomic<bool> g_joinEdge{true};      // true at boot so the first pass still runs

using FnProcessEvent = void (*)(void* obj, void* func, void* params);
using FnMalloc = void* (*)(size_t, uint32_t);
using FnFree = void (*)(void*);

template <typename T>
T Fn(const char* sym) {
    return (T)(uintptr_t)Resolve::Addr(sym);
}

std::string ErrJson(const std::string& msg) { return "{\"error\":" + JsonStr(msg) + "}"; }

void* ReadPtrAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 8)) return nullptr;
    return *(void* const*)p;
}

// Offset of a UPROPERTY on `obj`'s class, walking up the class chain. -1 when absent.
// Always FindPropertyByName - never the 0x340 / 0x350 constants lane L1 measured on this build.
int32_t Off(void* obj, const char* name) {
    void* cls = Reflect::ObjClass(obj);
    for (void* c = cls; c; c = Reflect::SuperStruct(c)) {
        void* p = Reflect::FindProperty(c, name);
        if (!p) continue;
        int32_t o = Reflect::PropertyOffset(p);
        return (o >= 0 && o < 0x100000) ? o : -1;
    }
    return -1;
}

std::string ReadFString(const FString& s) {
    if (s.Num <= 0 || s.Num > 1 << 16 || !MemReadable(s.Data, (size_t)s.Num * 2)) return "";
    return Reflect::Utf16To8(s.Data, s.Num);
}

// An FString whose buffer comes from the game allocator. SetAdmin takes its FString by value, so
// the callee may MoveTemp it into the array; Data is re-read before the free for exactly that case.
struct GameFString {
    FString fs{};
    bool ok = false;
    explicit GameFString(const std::string& s) {
        auto malloc_ = Fn<FnMalloc>("FMemory::Malloc");
        if (!malloc_) return;
        auto w = Reflect::Utf8To16(s);  // NUL-terminated
        void* mem = malloc_(w.size() * 2, 8);
        if (!mem) return;
        memcpy(mem, w.data(), w.size() * 2);
        fs.Data = (char16_t*)mem;
        fs.Num = (int32_t)w.size();
        fs.Max = fs.Num;
        ok = true;
    }
    ~GameFString() {
        auto free_ = Fn<FnFree>("FMemory::Free");
        if (ok && free_ && fs.Data) free_(fs.Data);
    }
    GameFString(const GameFString&) = delete;
    GameFString& operator=(const GameFString&) = delete;
};

// ---------------------------------------------------------------------------------------------
// configuration + cached diagnostics

Mutex g_lock;
std::vector<std::string> g_configured;        // TAKARO_ADMIN_STEAMIDS, validated
std::vector<std::string> g_configuredBad;     // tokens that were not SteamID64s
std::vector<std::string> g_superConfigured;   // TAKARO_SUPERADMIN_STEAMIDS, validated
std::vector<std::string> g_superConfiguredBad;
std::vector<std::string> g_applied;           // ids this boot has successfully handed to SetAdmin
std::set<std::string> g_grantedThisSession;   // per-connection re-grant guard (cleared on leave)
int32_t g_sessionArrayNum = -1;               // AdminSteamIDs.Num() as last read by reflection
int32_t g_superArrayNum = -1;
std::vector<std::string> g_sessionArray;      // the array contents, as read back
std::string g_lastError;
std::string g_status = "idle";  // idle | no-session | no-function | ok | error
uint64_t g_passes = 0, g_grants = 0;

std::string JsonArr(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i) o += ",";
        o += JsonStr(v[i]);
    }
    return o + "]";
}

// ---------------------------------------------------------------------------------------------
// game-thread side

// The live AVeinGameSession. The class comes from its StaticClass thunk when the compiler emitted
// one and from the object path otherwise; the CDO is never returned (granting on it would write
// into the archetype, not the running session).
void* FindGameSession(std::string& why) {
    void* cls = Reflect::StaticClass("AVeinGameSession::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Vein", "VeinGameSession");
    if (!cls) cls = Reflect::StaticClass("AGameSession::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Engine", "GameSession");
    if (!cls) {
        why = "neither AVeinGameSession nor AGameSession could be found";
        return nullptr;
    }
    std::vector<void*> objs;
    if (!Reflect::GetObjectsOfClass(cls, objs, true)) {
        why = "GetObjectsOfClass unavailable";
        return nullptr;
    }
    for (void* o : objs) {
        if (!o || !MemReadable(o, 0x40)) continue;
        if (Reflect::ObjName(o).rfind("Default__", 0) == 0) continue;
        return o;
    }
    why = "no live game session yet (world not up)";
    return nullptr;
}

// Reads a TArray<FString> UPROPERTY by name. Returns false when the property does not exist.
bool ReadStringArray(void* obj, const char* prop, std::vector<std::string>& out, int32_t& num) {
    num = -1;
    int32_t off = Off(obj, prop);
    if (off < 0) return false;
    const void* p = (const char*)obj + off;
    if (!MemReadable(p, sizeof(TArray<FString>))) return false;
    TArray<FString> arr;
    memcpy(&arr, p, sizeof arr);
    num = arr.Num;
    if (!arr.Data || arr.Num <= 0 || arr.Num > 4096 || !MemReadable(arr.Data, (size_t)arr.Num * sizeof(FString)))
        return true;
    for (int32_t i = 0; i < arr.Num; i++) {
        std::string s = ReadFString(arr.Data[i]);
        if (!s.empty()) out.push_back(s);
    }
    return true;
}

// AVeinGameSession::SetAdmin(FString Id, bool bAdmin) through UObject::ProcessEvent.
//
// Parameter frame: an FString is 16 bytes aligned to 8 and the bool follows it, so the frame is
// {FString @0, bool @16} with PropertiesSize 24. The size is checked against the UFunction's own
// PropertiesSize before the call; a frame that does not look like that is refused rather than
// guessed at, and the buffer is 64 bytes so an unexpected-but-plausible frame cannot overrun.
bool CallSessionSetAdmin(void* session, const std::string& id, bool admin, std::string& err) {
    auto pe = Fn<FnProcessEvent>("UObject::ProcessEvent");
    if (!pe) {
        err = "UObject::ProcessEvent unresolved";
        return false;
    }
    void* func = Reflect::FindFunction(session, "SetAdmin");
    if (!func) {
        err = "AVeinGameSession has no SetAdmin UFUNCTION on this build";
        return false;
    }
    const void* sizeAt = (const char*)func + Reflect::Lay().structPropertiesSize;
    if (!MemReadable(sizeAt, 4)) {
        err = "could not read SetAdmin's parameter size";
        return false;
    }
    int32_t frame = 0;
    memcpy(&frame, sizeAt, 4);
    if (frame < 17 || frame > 48) {
        err = "SetAdmin's parameter frame is " + std::to_string(frame) + " bytes, expected {FString,bool} (24)";
        return false;
    }
    GameFString gs(id);
    if (!gs.ok) {
        err = "could not allocate the id string";
        return false;
    }
    alignas(16) unsigned char params[64];
    memset(params, 0, sizeof params);
    memcpy(params, &gs.fs, sizeof gs.fs);
    params[16] = admin ? 1 : 0;
    pe(session, func, params);
    // ProcessEvent may have moved the string into the array, in which case Data is now null and the
    // destructor must not free it again.
    FString after{};
    memcpy(&after, params, sizeof after);
    if (after.Data != gs.fs.Data) gs.fs.Data = after.Data;
    return true;
}

// AVeinPlayerState::SetAdmin(bool) on a *live* player state, so the flag the client's admin panel
// reads flips without a rejoin. Best-effort: the session array is the authoritative grant.
void CallPlayerStateSetAdmin(void* ps, bool admin) {
    auto pe = Fn<FnProcessEvent>("UObject::ProcessEvent");
    if (!pe || !ps) return;
    void* func = Reflect::FindFunction(ps, "SetAdmin");
    if (!func) return;
    const void* sizeAt = (const char*)func + Reflect::Lay().structPropertiesSize;
    if (!MemReadable(sizeAt, 4)) return;
    int32_t frame = 0;
    memcpy(&frame, sizeAt, 4);
    if (frame < 1 || frame > 16) return;
    alignas(16) unsigned char params[32];
    memset(params, 0, sizeof params);
    params[0] = admin ? 1 : 0;
    pe(ps, func, params);
}

// Online player states, keyed by SteamID64. Cheap enough to redo every 2 s.
std::map<std::string, void*> OnlinePlayerStates() {
    std::map<std::string, void*> out;
    void* cls = Reflect::StaticClass("UWorld::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Engine", "World");
    if (!cls) return out;
    std::vector<void*> worlds;
    Reflect::GetObjectsOfClass(cls, worlds, true);
    void* gs = nullptr;
    for (void* w : worlds) {
        void* cand = ReadPtrAt(w, Off(w, "GameState"));
        if (cand) {
            gs = cand;
            break;
        }
    }
    if (!gs) return out;
    int32_t arrOff = Off(gs, "PlayerArray");
    if (arrOff < 0 || !MemReadable((const char*)gs + arrOff, 16)) return out;
    TArray<void*> arr;
    memcpy(&arr, (const char*)gs + arrOff, sizeof arr);
    if (!arr.Data || arr.Num <= 0 || arr.Num > 256 || !MemReadable(arr.Data, (size_t)arr.Num * 8)) return out;
    for (int32_t i = 0; i < arr.Num; i++) {
        void* ps = arr.Data[i];
        if (!ps || !MemReadable(ps, 0x40)) continue;
        int32_t strOff = -1;
        for (const char* n : {"PlayerID", "OnlineID", "PlayerUniqueID"}) {
            strOff = Off(ps, n);
            if (strOff >= 0) break;
        }
        if (strOff < 0 || !MemReadable((const char*)ps + strOff, sizeof(FString))) continue;
        FString s;
        memcpy(&s, (const char*)ps + strOff, sizeof s);
        std::string id = ActionsUtil::NormalizeGameId(ReadFString(s));
        if (id.size() == 17) out[id] = ps;
    }
    return out;
}

// One pass. Game thread only. `force` grants `forceId` even when the array already lists it.
void Pass(const std::string& forceId, bool forceAdmin, bool force, std::string* forceErr, bool* forceOk) {
    std::vector<std::string> configured;
    {
        Guard g(g_lock);
        configured = g_configured;
        g_passes++;
    }
    if (configured.empty() && !force) {
        Guard g(g_lock);
        g_status = "idle";
        return;
    }

    std::string why;
    void* session = FindGameSession(why);
    if (!session) {
        Guard g(g_lock);
        g_status = "no-session";
        g_lastError = why;
        if (forceErr) *forceErr = why;
        return;
    }

    std::vector<std::string> current;
    int32_t num = -1, superNum = -1;
    bool haveProp = ReadStringArray(session, "AdminSteamIDs", current, num);
    std::vector<std::string> superCurrent;
    ReadStringArray(session, "SuperAdminSteamIDs", superCurrent, superNum);

    auto listed = [&](const std::string& id) {
        for (const std::string& s : current)
            if (s == id) return true;
        return false;
    };

    std::map<std::string, void*> online = OnlinePlayerStates();
    std::vector<std::string> granted;
    std::string err;

    auto grant = [&](const std::string& id, bool admin) -> bool {
        std::string e;
        if (!CallSessionSetAdmin(session, id, admin, e)) {
            if (err.empty()) err = e;
            return false;
        }
        auto it = online.find(id);
        if (it != online.end()) CallPlayerStateSetAdmin(it->second, admin);
        granted.push_back(id);
        PluginLog("admin: SetAdmin(%s, %s) via ProcessEvent on the live AVeinGameSession", id.c_str(),
                  admin ? "true" : "false");
        return true;
    };

    for (const std::string& id : configured) {
        bool needed = !haveProp || !listed(id);
        // A player who has (re)joined since the last grant gets it again even when the array already
        // lists them: the client-side panel keys off the replicated player-state flag, which is set
        // from the array at login and can therefore miss a grant made mid-session.
        bool rejoined = online.count(id) && !g_grantedThisSession.count(id);
        if (needed || rejoined) grant(id, true);
    }
    // Forget the per-connection guard for anyone who has left, so a rejoin re-grants.
    for (auto it = g_grantedThisSession.begin(); it != g_grantedThisSession.end();)
        it = online.count(*it) ? std::next(it) : g_grantedThisSession.erase(it);
    for (const std::string& id : configured)
        if (online.count(id)) g_grantedThisSession.insert(id);

    bool okForce = false;
    if (force && !forceId.empty()) {
        okForce = grant(forceId, forceAdmin);
        if (!okForce && forceErr) *forceErr = err.empty() ? "SetAdmin failed" : err;
    }
    if (forceOk) *forceOk = okForce;

    // Read the arrays back so /health reports what the game holds, not what we asked for.
    std::vector<std::string> after;
    int32_t afterNum = -1;
    ReadStringArray(session, "AdminSteamIDs", after, afterNum);

    Guard g(g_lock);
    g_sessionArray = after;
    g_sessionArrayNum = afterNum;
    g_superArrayNum = superNum;
    for (const std::string& id : granted) {
        bool seen = false;
        for (const std::string& a : g_applied) seen = seen || a == id;
        if (!seen) g_applied.push_back(id);
        g_grants++;
    }
    if (!haveProp) {
        g_status = "error";
        g_lastError = "the live game session has no AdminSteamIDs property";
    } else if (!err.empty()) {
        g_status = "error";
        g_lastError = err;
    } else {
        g_status = "ok";
        g_lastError.clear();
    }
}

}  // namespace

// ================================================================================================

void Admin::Init() {
    std::string raw = ConfigValue("TAKARO_ADMIN_STEAMIDS", "adminSteamIds");
    std::string superRaw = ConfigValue("TAKARO_SUPERADMIN_STEAMIDS", "superAdminSteamIds");
    Guard g(g_lock);
    g_configured = ActionsUtil::ParseSteamIdList(raw, &g_configuredBad);
    g_superConfigured = ActionsUtil::ParseSteamIdList(superRaw, &g_superConfiguredBad);

    std::string detail;
    if (g_configured.empty()) {
        detail =
            "no TAKARO_ADMIN_STEAMIDS configured. VEIN's AdminSteamIDs/SuperAdminSteamIDs are NOT "
            "UPROPERTY(Config), so Game.ini cannot grant admin on this build; set the env var and the plugin "
            "calls AVeinGameSession::SetAdmin through ProcessEvent instead.";
        PluginState::Get().SetCapability("adminGrant", "degraded", detail);
    } else {
        detail = std::to_string(g_configured.size()) +
                 " SteamID64(s) will be granted with AVeinGameSession::SetAdmin(FString,bool) through "
                 "ProcessEvent once the world is up, and re-granted on rejoin. UNVERIFIED: whether the "
                 "in-game admin panel then opens, and whether the UAdminComponent RPCs gate on IsAdmin().";
        PluginState::Get().SetCapability("adminGrant", "ok", detail);
    }
    if (!g_configuredBad.empty())
        PluginLog("admin: %zu TAKARO_ADMIN_STEAMIDS token(s) ignored (not a SteamID64)", g_configuredBad.size());
    if (!g_superConfigured.empty())
        PluginLog(
            "admin: TAKARO_SUPERADMIN_STEAMIDS has %zu id(s) but this build ships no super-admin setter "
            "(no SetSuperAdmin of any spelling in the depot .sym); nothing will be written",
            g_superConfigured.size());
    PluginLog("admin: %zu admin id(s) configured", g_configured.size());
}

void Admin::Housekeep() {
    {
        Guard g(g_lock);
        if (g_configured.empty()) return;
    }
    if (GameThread::TickCount() == 0) return;
    // LANE L9 (game-thread policy): a grant pass reads and writes the live game session through
    // ProcessEvent, so it belongs on the game thread - but it only has anything to do when a player
    // connects (a grant is re-applied on rejoin). It now runs on that edge, with a 60 s safety pass
    // instead of the old unconditional entry every 2 s.
    static uint64_t lastPass = 0;
    uint64_t now = NowMs();
    bool joined = Admin::ConsumeJoinEdge();
    if (!joined && lastPass && now - lastPass < kPassIntervalMs) return;
    lastPass = now;
    GameThread::Run([] { Perf::Scope sc("admin.pass"); Pass("", true, false, nullptr, nullptr); }, 5000);
}

// Set from the events lane when a player is announced: the only moment a grant pass can matter.
void Admin::NoteJoin() { g_joinEdge.store(true); }
bool Admin::ConsumeJoinEdge() { return g_joinEdge.exchange(false); }

std::string Admin::DiagnosticsJson() {
    Guard g(g_lock);
    std::string superReason =
        g_superConfigured.empty()
            ? "not configured"
            : "this build ships no super-admin setter: the depot .sym has AVeinGameSession::SetAdmin, "
              "AVeinPlayerState::SetAdmin and UAdminComponent::Server_SetAdmin but no SetSuperAdmin, so "
              "SuperAdminSteamIDs cannot be populated without writing the TArray directly - this lane does not";
    return std::string("{\"configured\":") + JsonArr(g_configured) + ",\"applied\":" + JsonArr(g_applied) +
           ",\"sessionArrayNum\":" + std::to_string(g_sessionArrayNum) +
           ",\"sessionArray\":" + JsonArr(g_sessionArray) + ",\"status\":" + JsonStr(g_status) +
           ",\"lastError\":" + JsonStr(g_lastError) + ",\"rejected\":" + JsonArr(g_configuredBad) +
           ",\"passes\":" + std::to_string(g_passes) + ",\"grants\":" + std::to_string(g_grants) +
           ",\"superAdmins\":{\"configured\":" + JsonArr(g_superConfigured) + ",\"applied\":[]" +
           ",\"sessionArrayNum\":" + std::to_string(g_superArrayNum) + ",\"rejected\":" +
           JsonArr(g_superConfiguredBad) + ",\"reason\":" + JsonStr(superReason) + "}" +
           ",\"via\":\"AVeinGameSession::SetAdmin(FString,bool) through UObject::ProcessEvent; arrays read "
           "back with UStruct::FindPropertyByName\"}";
}

Actions::Result Admin::SetAdminEndpoint(const JsonValue& body) {
    const JsonValue* gid = body.get("gameId");
    if (!gid || !gid->isStr() || gid->str.empty()) return {400, ErrJson("gameId is required")};
    std::string id = ActionsUtil::NormalizeGameId(gid->str);
    if (id.size() != 17) return {400, ErrJson("gameId must be a SteamID64")};
    const JsonValue* a = body.get("admin");
    if (a && a->type != JsonValue::Bool) return {400, ErrJson("admin must be true or false")};
    bool admin = a ? a->b : true;

    struct SetResult { std::string err; bool ok = false; };
    auto result = std::make_shared<SetResult>();
    bool ran = GameThread::Run([id, admin, result] { Pass(id, admin, true, &result->err, &result->ok); }, 5000);
    if (!ran) return {503, ErrJson("game thread unavailable")};
    if (!result->ok) return {501, ErrJson(result->err.empty() ? "SetAdmin unavailable" : result->err)};

    Guard g(g_lock);
    bool listed = false;
    for (const std::string& s : g_sessionArray) listed = listed || s == id;
    return {200, "{\"success\":true,\"gameId\":" + JsonStr(id) + ",\"admin\":" + (admin ? "true" : "false") +
                     ",\"sessionArrayNum\":" + std::to_string(g_sessionArrayNum) +
                     ",\"listedInSessionArray\":" + (listed ? "true" : "false") +
                     ",\"via\":\"AVeinGameSession::SetAdmin via ProcessEvent\"}"};
}
