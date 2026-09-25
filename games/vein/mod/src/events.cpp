// Event sources — lane L2 owns this file. Our own code, adapted from the Takaro Dragonwilds plugin.
//
// Six event types feed the ring buffer in `state`:
//   player-connected     PostLogin vtable-slot hook (swept across every exported vtable holding it)
//                        + a pending-join resolver that waits for the SteamID64 to be valid
//   player-disconnected  APlayerController::OnNetCleanup + AActor::Destroyed + AGameModeBase::Logout
//                        + a player-state-vanish reaper; deduped 30 s per gameId
//   chat-message         ProcessEvent slot hook on live objects, filtered on the chat RPC's FName,
//                        with the `LogVeinChat:` log line as a second source (deduped)
//   player-death         ProcessEvent on the character death event + a Health/Dead edge per cycle,
//                        deduped 3 s, with attacker / killerEntity attribution
//   entity-killed        ProcessEvent on the AI death path; the killer is the damage event's own
//                        instigator/causer ONLY (lane L2c removed the sensed-target and
//                        only-player-online guesses), the entity is the victim named as
//                        GET /entities names it, and the weapon is the killer's equipped item
//   log                  rotation-aware tail of Vein/Saved/Logs/Vein.log, redacted
//
// Rules obeyed here (the Dragonwilds discipline, and the reason that plugin never faulted again):
//   * every UObject touch happens on the game thread - inside a hook (which already runs there) or
//     inside GameThread::Run from the housekeeping thread;
//   * no FName is stringified from an unvalidated pointer: ValidObject() walks the class chain to
//     UObject first, and hot paths compare *raw* FName values instead of strings;
//   * every UPROPERTY offset comes from UStruct::FindPropertyByName at runtime, never a constant;
//   * every dereference of a game pointer is behind MemReadable();
//   * a failure degrades one capability with a reason in /health and never throws out of a hook;
//   * a capability may only claim "ok" when a hook is actually installed - a resolved symbol on its
//     own proves nothing.
//
// Which symbols/UFunctions are bound and what is still UNVERIFIED: docs/events-design.md.

#include "events.h"

#include "actions.h"
#include "admin.h"
#include "actions_util.h"
#include "events_parse.h"
#include "gamethread.h"
#include "perf.h"
#include "hooks.h"
#include "reflect.h"
#include "resolve.h"
#include "state.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>

using namespace UE;

namespace {

// LANE L9: "something changed" - a player joined or left, or a class we care about was first seen.
// Housekeeping sweeps run on this edge plus a slow safety cadence, instead of unconditionally every
// 2 s. New AI of an already-seen Blueprint share that class's vtable, which the sweep has already
// hooked, so a spawn is not a reason to sweep; a NEW class is, and that only happens on streaming.
std::atomic<bool> g_worldDirty{true};

// =================================================================================================
// diagnostics

struct SourceStat {
    std::atomic<uint64_t> fired{0};
    std::atomic<uint64_t> emitted{0};
    std::atomic<bool> hooked{false};
    std::string note;
};
SourceStat g_join, g_leave, g_chat, g_death, g_kill, g_log;
Mutex g_noteLock;

void Note(SourceStat& s, const std::string& n) {
    Guard g(g_noteLock);
    s.note = n;
}
std::string NoteOf(SourceStat& s) {
    Guard g(g_noteLock);
    return s.note;
}
void Degrade(const char* cap, SourceStat& s, const std::string& why) {
    Note(s, why);
    PluginState::Get().SetCapability(cap, "degraded", why);
    PluginLog("events: %s degraded: %s", cap, why.c_str());
}
void Ok(const char* cap, SourceStat& s, const std::string& how) {
    Note(s, how);
    PluginState::Get().SetCapability(cap, "ok", "");
    PluginLog("events: %s ok (%s)", cap, how.c_str());
}

// =================================================================================================
// reflection helpers (game thread only)

Mutex g_offLock;
std::map<std::pair<void*, std::string>, int32_t> g_offCache;

int32_t PropOff(void* strct, const char* name) {
    if (!strct) return -1;
    std::pair<void*, std::string> key{strct, name};
    {
        Guard g(g_offLock);
        auto it = g_offCache.find(key);
        if (it != g_offCache.end()) return it->second;
    }
    void* p = Reflect::FindProperty(strct, name);
    int32_t off = p ? Reflect::PropertyOffset(p) : -1;
    if (off < 0 || off > 0x100000) off = -1;
    Guard g(g_offLock);
    g_offCache[key] = off;
    return off;
}
int32_t PropOffOf(void* obj, const char* name) { return PropOff(Reflect::ObjClass(obj), name); }

template <typename T>
bool ReadAt(void* base, int32_t off, T& out) {
    if (!base || off < 0) return false;
    const void* p = (const char*)base + off;
    if (!MemReadable(p, sizeof(T))) return false;
    memcpy(&out, p, sizeof(T));
    return true;
}

// Reads a numeric UPROPERTY by its *declared* type. UE 5.6 mixes float and double freely and this
// build's `UHealthComponent::SetHealth(double)` says health is a double, so reading a fixed `float`
// would silently produce nonsense. Returns false when the property is absent or not numeric.
bool ReadNumericProp(void* obj, const char* name, double& out) {
    void* cls = Reflect::ObjClass(obj);
    void* prop = nullptr;
    for (void* c = cls; c && !prop; c = Reflect::SuperStruct(c)) prop = Reflect::FindProperty(c, name);
    if (!prop) return false;
    int32_t off = Reflect::PropertyOffset(prop);
    if (off < 0) return false;
    std::string type = Reflect::PropertyTypeName(prop);
    if (type == "DoubleProperty") {
        double v = 0;
        if (!ReadAt(obj, off, v)) return false;
        out = v;
    } else if (type == "FloatProperty") {
        float v = 0;
        if (!ReadAt(obj, off, v)) return false;
        out = v;
    } else if (type == "IntProperty") {
        int32_t v = 0;
        if (!ReadAt(obj, off, v)) return false;
        out = v;
    } else {
        return false;
    }
    return true;
}

// FText -> display string. Never faults: the getter is a plain inspector and every pointer is
// bounds-checked first. Used for AVeinAnimalCharacter::UsableName, the only readable creature name
// this build has (DWARF: FText UsableName @ 0x958; there is no AIName/DisplayName anywhere).
std::string ReadFTextAt(void* base, int32_t off) {
    if (!base || off < 0) return "";
    auto disp = (const FString* (*)(const void*))(uintptr_t)Resolve::Addr("FTextInspector::GetDisplayString");
    const void* p = (const char*)base + off;
    if (!disp || !MemReadable(p, 16)) return "";
    const FString* s = disp(p);
    if (!s || !MemReadable(s, 16) || s->Num <= 0 || s->Num > (1 << 16)) return "";
    return Reflect::Utf16To8(s->Data, s->Num);
}

std::string ReadFStringAt(void* base, int32_t off) {
    FString s;
    if (!ReadAt(base, off, s)) return "";
    if (s.Num <= 0 || s.Num > 65536) return "";
    return Reflect::Utf16To8(s.Data, s.Num);
}

void WalkProps(void* strct, std::vector<void*>& out, size_t max) {
    const auto& lay = Reflect::Lay();
    void* f = nullptr;
    if (!ReadAt(strct, (int32_t)lay.structChildProperties, f)) return;
    for (size_t i = 0; f && i < max; i++) {
        out.push_back(f);
        void* nxt = nullptr;
        if (!ReadAt(f, (int32_t)lay.fieldNext, nxt)) break;
        f = nxt;
    }
}

std::string FieldName(void* field) {
    FName n;
    if (!ReadAt(field, (int32_t)Reflect::Lay().fieldName, n)) return "";
    return Reflect::NameToString(n);
}

// Cached class pointers. Comparing *pointers* never stringifies anything, which is what killed the
// Dragonwilds server once (SIGSEGV inside FName::ToString on a bogus name index).
void* g_clsUObject = nullptr;
void* g_clsPlayerState = nullptr;
void* g_clsPlayerController = nullptr;
void* g_clsPawn = nullptr;
void* g_clsVeinBaseCharacter = nullptr;
void* g_clsVeinPlayerCharacter = nullptr;
void* g_clsAiCharacters[4] = {nullptr, nullptr, nullptr, nullptr};  // Zombie / Animal / AI base ...
void* g_clsHealthComponent = nullptr;
void* g_clsDamageType = nullptr;

// True when `obj` really looks like a live UObject: its Class pointer resolves and the class chain
// terminates at UClass(UObject). Nothing here reads an FName, so a bogus pointer cannot fault the
// engine name pool. Every ObjName()/ClassName() call in this file is gated on this.
bool ValidObject(void* obj) {
    if (!obj || !MemReadable(obj, 0x40)) return false;
    void* cls = Reflect::ObjClass(obj);
    if (!cls || !MemReadable(cls, 0x120) || !g_clsUObject) return false;
    for (int i = 0; cls && i < 32; i++) {
        if (cls == g_clsUObject) return true;
        void* sup = Reflect::SuperStruct(cls);
        if (sup == cls) return false;
        cls = sup;
    }
    return false;
}

std::string SafeObjName(void* obj) { return ValidObject(obj) ? Reflect::ObjName(obj) : std::string(); }
std::string SafeClassName(void* obj) { return ValidObject(obj) ? Reflect::ClassName(obj) : std::string(); }

// Raw FName of a UObject (no stringification) - the safe way to identify a UFunction in a hot hook.
bool ObjNameRaw(void* obj, FName& out) { return ReadAt(obj, (int32_t)Reflect::Lay().objName, out); }

bool IsAnyAi(void* obj) {
    if (!ValidObject(obj)) return false;
    for (void* c : g_clsAiCharacters)
        if (c && Reflect::IsA(obj, c)) return true;
    return false;
}

// UClass / UScriptStruct by plain name, trying the packages this game uses.
void* FindByName(const char* name) {
    static const char* kPkgs[] = {"/Script/Vein", "/Script/Engine", "/Script/CoreUObject",
                                  "/Script/RamjetNetworking", "/Script/RamjetSteam"};
    for (auto* p : kPkgs) {
        void* s = Reflect::FindObjectByPath(p, name);
        if (s) return s;
    }
    return nullptr;
}

template <typename T>
T SymFn(const char* name) {
    return (T)(uintptr_t)Resolve::Addr(name);
}

using FnProcessEvent = void (*)(void* self, void* func, void* params);
FnProcessEvent g_processEvent = nullptr;

// =================================================================================================
// identity
//
// gameId is the SteamID64 (docs/API.md). Three routes, in order of confidence:
//   1. AVeinPlayerState's own `PlayerID` property - the value the server prints as
//      `LogVein: PlayerState ID changed to <SteamID64>`. It may be an FString or an integer, so the
//      property's *type* decides how it is read; nothing is assumed.
//   2. APlayerState::UniqueId (FUniqueNetIdRepl) - the first eight words are scanned for a pointer
//      whose vtable is `_ZTV19FUniqueNetIdSteam`, which is then stringified by
//      FUniqueNetIdSteam::ToString. The TSharedPtr offset is never assumed.
//   3. the character-name / id cache fed by the log tail.

void* g_steamIdVptr = nullptr;
using FnNetIdToString = void (*)(FString* ret, const void* self);
FnNetIdToString g_steamIdToString = nullptr;

struct Ident {
    std::string gameId;         // SteamID64
    std::string name;           // what Takaro shows: the character name, else the Steam persona
    std::string platformName;   // Steam persona ("Limon")
    std::string characterName;  // VEIN character ("Takaro Tester"), may be empty
    bool valid() const { return !gameId.empty(); }
};

std::string PlayerJson(const Ident& id) {
    std::string o = "{\"gameId\":" + JsonStr(id.gameId) +
                    ",\"name\":" + JsonStr(id.name.empty() ? id.platformName : id.name);
    if (!id.characterName.empty()) o += ",\"characterName\":" + JsonStr(id.characterName);
    if (!id.platformName.empty()) o += ",\"platformName\":" + JsonStr(id.platformName);
    if (!id.gameId.empty()) {
        o += ",\"steamId\":" + JsonStr(id.gameId);
        o += ",\"platformId\":" + JsonStr("steam:" + id.gameId);
    }
    return o + "}";
}

std::string SteamIdFromNetIdRepl(void* base, int32_t off) {
    if (!base || off < 0 || !g_steamIdVptr || !g_steamIdToString) return "";
    for (int i = 0; i < 8; i++) {
        void* cand = nullptr;
        if (!ReadAt(base, off + i * 8, cand)) continue;
        if (!cand || !MemReadable(cand, 8)) continue;
        if (*(void**)cand != g_steamIdVptr) continue;
        FString ret{};
        g_steamIdToString(&ret, cand);
        std::string s = Reflect::ToStd(ret, true);
        // "Steam:76561198765432109" / "76561198765432109" / "[0x110000...]" are all seen in the wild.
        size_t colon = s.rfind(':');
        if (colon != std::string::npos) s = s.substr(colon + 1);
        if (EventsParse::LooksLikeSteamId64(s)) return s;
    }
    return "";
}

// The `PlayerID` property, whatever its declared type is.
std::string SteamIdFromProperty(void* state) {
    void* cls = Reflect::ObjClass(state);
    if (!cls) return "";
    // DWARF (`VeinServer-Linux-Test.debug`): `AVeinPlayerState::OnlineID` is an `FString` at 0x3d0 -
    // there is NO member called `PlayerID`, despite the UFunctions being named SetPlayerID /
    // GetPlayerUniqueID and the log line reading "PlayerState ID changed to". `OnlineID` is tried
    // first; the rest are kept as cheap insurance against a rename in a future build.
    for (const char* pn : {"OnlineID", "OnlineId", "PlayerID", "PlayerId", "SteamID", "SteamId"}) {
        void* prop = nullptr;
        for (void* c = cls; c && !prop; c = Reflect::SuperStruct(c)) prop = Reflect::FindProperty(c, pn);
        if (!prop) continue;
        int32_t off = Reflect::PropertyOffset(prop);
        if (off < 0) continue;
        std::string type = Reflect::PropertyTypeName(prop);
        if (type == "StrProperty") {
            std::string s = ReadFStringAt(state, off);
            if (EventsParse::LooksLikeSteamId64(s)) return s;
        } else if (type == "NameProperty") {
            FName n;
            if (ReadAt(state, off, n)) {
                std::string s = Reflect::NameToString(n);
                if (EventsParse::LooksLikeSteamId64(s)) return s;
            }
        } else if (type == "UInt64Property" || type == "Int64Property") {
            uint64_t v = 0;
            if (ReadAt(state, off, v) && v > 76561197960265728ULL) {
                char b[32];
                snprintf(b, sizeof b, "%llu", (unsigned long long)v);
                if (EventsParse::LooksLikeSteamId64(b)) return b;
            }
        }
    }
    return "";
}

std::string SteamIdFromPlayerState(void* state) {
    std::string id = SteamIdFromProperty(state);
    if (!id.empty()) return id;
    int32_t off = PropOffOf(state, "UniqueId");
    if (off < 0) off = PropOffOf(state, "UniqueID");
    return SteamIdFromNetIdRepl(state, off);
}

// A chat component, pawn or controller -> the APlayerState that owns it.
void* PlayerStateOf(void* actor) {
    if (!ValidObject(actor)) return nullptr;
    if (g_clsPlayerState && Reflect::IsA(actor, g_clsPlayerState)) return actor;
    int32_t off = PropOffOf(actor, "PlayerState");
    void* st = nullptr;
    if (off >= 0 && ReadAt(actor, off, st) && ValidObject(st)) return st;
    // A controller sitting on a pawn, or a component Outered to its owner.
    off = PropOffOf(actor, "Controller");
    void* ctrl = nullptr;
    if (off >= 0 && ReadAt(actor, off, ctrl) && ValidObject(ctrl)) {
        off = PropOffOf(ctrl, "PlayerState");
        if (off >= 0 && ReadAt(ctrl, off, st) && ValidObject(st)) return st;
    }
    void* outer = Reflect::ObjOuter(actor);
    if (outer && outer != actor && ValidObject(outer)) {
        if (g_clsPlayerState && Reflect::IsA(outer, g_clsPlayerState)) return outer;
        off = PropOffOf(outer, "PlayerState");
        if (off >= 0 && ReadAt(outer, off, st) && ValidObject(st)) return st;
    }
    return nullptr;
}

bool IdentFromPlayerState(void* state, Ident& out) {
    if (!ValidObject(state)) return false;
    out.gameId = SteamIdFromPlayerState(state);
    out.platformName = ReadFStringAt(state, PropOffOf(state, "PlayerNamePrivate"));
    out.characterName = ReadFStringAt(state, PropOffOf(state, "CharacterName"));
    if (out.characterName.empty()) out.characterName = ::state::CharacterName(out.gameId);
    if (!out.gameId.empty() && !out.characterName.empty())
        ::state::NoteCharacterName(out.gameId, out.characterName);
    out.name = out.characterName.empty() ? out.platformName : out.characterName;
    return out.valid();
}

Ident IdentFromController(void* controller) {
    Ident id;
    void* st = PlayerStateOf(controller);
    if (st) IdentFromPlayerState(st, id);
    return id;
}

// =================================================================================================
// connection tracking

struct Conn {
    void* controller = nullptr;
    void* playerState = nullptr;
    Ident id;
    uint64_t firstSeenMs = 0;
    bool announced = false;
    bool left = false;
};

Mutex g_connLock;
std::vector<Conn> g_conns;
std::map<std::string, uint64_t> g_recentLeaves;  // gameId -> ms of the emitted disconnect
const uint64_t kLeaveDedupeMs = 30000;
// DWARF: AVeinPlayerState has NO character-name member - the display name is
// APlayerState::PlayerNamePrivate (the Steam persona) and the VEIN character name lives on the
// *pawn* (AVeinPlayerCharacter::PlayerCharacterData), which does not exist yet at PostLogin. So the
// grace window is short: it is waiting for the log's `selected character ... (aka <name>)` line to
// land in the name cache, not for a property that is never going to appear.
const uint64_t kNameGraceMs = 3000;
const uint64_t kJoinGiveUpMs = 120000;

Conn* FindConn(void* c) {
    for (auto& k : g_conns)
        if (k.controller == c) return &k;
    return nullptr;
}

void ClearLeaveMarker(const std::string& gameId) {
    Guard g(g_connLock);
    g_recentLeaves.erase(gameId);
}

bool LeaveAlreadyEmitted(const std::string& gameId) {
    Guard g(g_connLock);
    uint64_t now = NowMs();
    for (auto it = g_recentLeaves.begin(); it != g_recentLeaves.end();)
        it = (now - it->second > kLeaveDedupeMs) ? g_recentLeaves.erase(it) : ++it;
    return g_recentLeaves.find(gameId) != g_recentLeaves.end();
}

void MarkLeft(const std::string& gameId) {
    Guard g(g_connLock);
    g_recentLeaves[gameId] = NowMs();
}

// Every gameId we have announced a join for, so the log fallback below can tell "the hook already
// did this" from "nothing has reported this player at all".
std::set<std::string> g_announced;

bool AlreadyAnnounced(const std::string& gameId) {
    Guard g(g_connLock);
    return g_announced.count(gameId) > 0;
}

void EmitJoin(const Ident& id) {
    g_worldDirty = true;
    Admin::NoteJoin();  // L9: the admin grant pass runs on this edge, not on a 2 s timer
    ClearLeaveMarker(id.gameId);
    {
        Guard g(g_connLock);
        g_announced.insert(id.gameId);
    }
    PluginState::Get().EmitEventDeferred("player-connected", [id] { return "{\"player\":" + PlayerJson(id) + "}"; });
    g_join.emitted++;
    PluginLog("events: player-connected %s (%s)", id.gameId.c_str(), id.name.c_str());
}

void EmitLeave(const Ident& id) {
    g_worldDirty = true;
    {
        Guard g(g_connLock);
        g_announced.erase(id.gameId);
    }
    PluginState::Get().EmitEventDeferred("player-disconnected", [id] { return "{\"player\":" + PlayerJson(id) + "}"; });
    g_leave.emitted++;
    PluginLog("events: player-disconnected %s (%s)", id.gameId.c_str(), id.name.c_str());
}

enum class Live { Unknown, Alive, Dead };
Mutex g_healthLock;
std::map<std::string, Live> g_liveness;

void OnLeave(void* controller, const char* which) {
    Ident id;
    bool announced = false;
    {
        Guard g(g_connLock);
        Conn* c = FindConn(controller);
        if (c) {
            if (c->left) return;
            c->left = true;
            id = c->id;
            announced = c->announced;
        }
    }
    if (!id.valid()) id = IdentFromController(controller);
    if (!id.valid()) {
        if (DebugEnabled()) PluginLog("events: %s for controller %p with no resolvable identity", which, controller);
        return;
    }
    if (LeaveAlreadyEmitted(id.gameId)) {
        if (DebugEnabled()) PluginLog("events: %s for %s already reported", which, id.gameId.c_str());
        return;
    }
    PluginLog("events: disconnect for %s reported by %s", id.gameId.c_str(), which);
    {
        Guard g(g_healthLock);
        g_liveness.erase(id.gameId);
    }
    if (!announced) EmitJoin(id);  // never report a leave for a player we never announced
    EmitLeave(id);
    MarkLeft(id.gameId);
    Guard g(g_connLock);
    for (size_t i = 0; i < g_conns.size(); i++)
        if (g_conns[i].controller == controller) {
            g_conns.erase(g_conns.begin() + (long)i);
            break;
        }
}

// =================================================================================================
// vtable hooks: PostLogin / Logout / OnNetCleanup / Destroyed
//
// Swaps the slot holding `symName` in *every* exported vtable that contains it. Hooking only the
// declaring class never fires when the live object is a subclass carrying its own vtable - the
// lesson L1 learned on the engine Tick hook, and the reason 37 322 exported _ZTV* symbols matter.
size_t SwapEverywhere(const char* symName, void* detour, size_t& slotOut, std::string& tablesOut) {
    uint64_t target = Resolve::Addr(symName);
    if (!target) return 0;
    size_t hooked = 0;
    std::string tables;
    for (auto& vt : VTableSymbols()) {
        uint64_t addr = vt.second.first, vsize = vt.second.second;
        auto* words = (const uint64_t*)(uintptr_t)addr;
        size_t count = (size_t)(vsize / 8);
        if (count < 3 || !MemReadable(words, (size_t)vsize)) continue;
        for (size_t i = 2; i < count; i++) {
            if (words[i] != target) continue;
            std::string err;
            if (Hooks::SwapVTableSlot(symName, (void*)(uintptr_t)addr, i - 2, detour, nullptr, err)) {
                hooked++;
                slotOut = i - 2;
                if (tables.size() < 160) tables += (tables.empty() ? "" : ",") + vt.first;
            } else if (DebugEnabled()) {
                PluginLog("events: %s slot %zu in %s not swapped: %s", symName, i - 2, vt.first.c_str(), err.c_str());
            }
        }
    }
    tablesOut = tables;
    return hooked;
}

using FnPostLogin = void (*)(void* self, void* pc);
using FnLogout = void (*)(void* self, void* controller);
using FnNetCleanup = void (*)(void* self, void* connection);
using FnDestroyed = void (*)(void* self);

// One original per declaring class. AVeinGameModeBase overrides PostLogin and Logout, so the live
// game mode's vtable slot holds the *override*, and each vtable must be handed back its own
// original - swapping both addresses with a single shared `orig` would call the wrong body.
FnPostLogin g_origPostLoginVein = nullptr;
FnPostLogin g_origPostLoginBase = nullptr;
FnLogout g_origLogoutVein = nullptr;
FnLogout g_origLogoutBase = nullptr;
FnLogout g_origLogoutGm = nullptr;
FnNetCleanup g_origNetCleanup = nullptr;
FnDestroyed g_origDestroyedPc = nullptr;
FnDestroyed g_origDestroyedCtrl = nullptr;
std::atomic<bool> g_liveModeHooked{false};

void TrackJoin(void* pc) {
    g_join.fired++;
    try {
        Guard g(g_connLock);
        if (!FindConn(pc)) {
            Conn c;
            c.controller = pc;
            c.firstSeenMs = NowMs();
            g_conns.push_back(c);
        }
    } catch (...) {
    }
}

void DetourPostLoginVein(void* self, void* pc) {
    if (g_origPostLoginVein) g_origPostLoginVein(self, pc);
    Hooks::MarkFired("AVeinGameModeBase::PostLogin");
    TrackJoin(pc);
}

void DetourPostLoginBase(void* self, void* pc) {
    if (g_origPostLoginBase) g_origPostLoginBase(self, pc);
    Hooks::MarkFired("AGameModeBase::PostLogin");
    TrackJoin(pc);
}

void DetourLogoutVein(void* self, void* controller) {
    try {
        Hooks::MarkFired("AVeinGameModeBase::Logout");
        g_leave.fired++;
        OnLeave(controller, "AVeinGameModeBase::Logout");
    } catch (...) {
    }
    if (g_origLogoutVein) g_origLogoutVein(self, controller);
}

void DetourLogoutBase(void* self, void* controller) {
    try {
        Hooks::MarkFired("AGameModeBase::Logout");
        g_leave.fired++;
        OnLeave(controller, "AGameModeBase::Logout");
    } catch (...) {
    }
    if (g_origLogoutBase) g_origLogoutBase(self, controller);
}

void DetourLogoutGm(void* self, void* controller) {
    try {
        Hooks::MarkFired("AGameMode::Logout");
        g_leave.fired++;
        OnLeave(controller, "AGameMode::Logout");
    } catch (...) {
    }
    if (g_origLogoutGm) g_origLogoutGm(self, controller);
}

void DetourNetCleanup(void* self, void* connection) {
    try {
        Hooks::MarkFired("APlayerController::OnNetCleanup");
        g_leave.fired++;
        OnLeave(self, "OnNetCleanup");
    } catch (...) {
    }
    if (g_origNetCleanup) g_origNetCleanup(self, connection);
}

// Destroyed() is on every controller; only a tracked player controller is interesting, so the
// detour costs one vector scan for everything else.
void DestroyedCommon(void* self, const char* which) {
    try {
        bool tracked;
        {
            Guard g(g_connLock);
            tracked = FindConn(self) != nullptr;
        }
        if (tracked) {
            Hooks::MarkFired(which);
            OnLeave(self, "Destroyed");
        }
    } catch (...) {
    }
}

void DetourDestroyedPc(void* self) {
    DestroyedCommon(self, "APlayerController::Destroyed");
    if (g_origDestroyedPc) g_origDestroyedPc(self);
}

void DetourDestroyedCtrl(void* self) {
    DestroyedCommon(self, "AController::Destroyed");
    if (g_origDestroyedCtrl) g_origDestroyedCtrl(self);
}

// Insurance for the case the exported-vtable sweep does not reach the live game mode (it did not on
// Dragonwilds for PreLogin: the slot's contents were not what the file image said). Scans the live
// AVeinGameModeBase object's own vtable for either PostLogin or either Logout address and swaps the
// slot directly. Runs on the game thread, retried from housekeeping until it succeeds.
void HookLiveGameMode() {
    if (g_liveModeHooked.load()) return;
    uint64_t postVein = Resolve::Addr("AVeinGameModeBase::PostLogin");
    uint64_t postBase = Resolve::Addr("AGameModeBase::PostLogin");
    uint64_t outVein = Resolve::Addr("AVeinGameModeBase::Logout");
    uint64_t outBase = Resolve::Addr("AGameModeBase::Logout");
    if (!postVein && !postBase) return;
    void* cls = Reflect::FindObjectByPath("/Script/Vein", "VeinGameModeBase");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Engine", "GameModeBase");
    if (!cls) return;
    std::vector<void*> modes;
    if (!Reflect::GetObjectsOfClass(cls, modes, true)) return;
    for (void* gm : modes) {
        if (!ValidObject(gm)) continue;
        void* vt = *(void**)gm;
        if (!vt || !MemReadable(vt, 8 * 512)) continue;
        auto* words = (const uint64_t*)vt;
        bool any = false;
        for (size_t i = 0; i < 512; i++) {
            const char* name = nullptr;
            void* detour = nullptr;
            void** slotOrig = nullptr;
            if (postVein && words[i] == postVein) {
                name = "AVeinGameModeBase::PostLogin";
                detour = (void*)&DetourPostLoginVein;
                slotOrig = (void**)&g_origPostLoginVein;
            } else if (postBase && words[i] == postBase) {
                name = "AGameModeBase::PostLogin";
                detour = (void*)&DetourPostLoginBase;
                slotOrig = (void**)&g_origPostLoginBase;
            } else if (outVein && words[i] == outVein) {
                name = "AVeinGameModeBase::Logout";
                detour = (void*)&DetourLogoutVein;
                slotOrig = (void**)&g_origLogoutVein;
            } else if (outBase && words[i] == outBase) {
                name = "AGameModeBase::Logout";
                detour = (void*)&DetourLogoutBase;
                slotOrig = (void**)&g_origLogoutBase;
            } else {
                continue;
            }
            void* orig = nullptr;
            std::string err;
            if (!Hooks::HookObjectVTable(name, gm, i, detour, &orig, err)) {
                PluginLog("events: live game-mode hook %s at slot %zu failed: %s", name, i, err.c_str());
                continue;
            }
            if (orig) *slotOrig = orig;
            any = true;
            PluginLog("events: live game mode %s hooked at slot %zu (orig=%p)", name, i, orig);
        }
        if (any) {
            g_liveModeHooked = true;
            g_join.hooked = true;
            g_leave.hooked = true;
            return;
        }
    }
}

// The SteamID64 is only valid once the client finished the Steam auth handshake, and the character
// name arrives later still (research/2026-09-17-log-grammar.md). The join event waits for the id and
// gives the name a grace window, but never blocks on it.
void ResolvePendingJoins() {
    std::vector<Ident> toAnnounce;
    std::vector<void*> drop;
    std::vector<std::string> banned;
    {
        Guard g(g_connLock);
        for (auto& c : g_conns) {
            if (c.announced || c.left) continue;
            Ident id = IdentFromController(c.controller);
            uint64_t age = NowMs() - c.firstSeenMs;
            if (!id.valid()) {
                if (age > kJoinGiveUpMs) drop.push_back(c.controller);
                continue;
            }
            if (id.characterName.empty() && age < kNameGraceMs) continue;
            c.id = id;
            c.playerState = PlayerStateOf(c.controller);
            c.announced = true;
            toAnnounce.push_back(id);
            if (::state::IsBanned(id.gameId)) banned.push_back(id.gameId);
        }
        for (void* d : drop)
            for (size_t i = 0; i < g_conns.size(); i++)
                if (g_conns[i].controller == d) {
                    PluginLog("events: giving up on join for controller %p (no SteamID64 after %llums)", d,
                              (unsigned long long)kJoinGiveUpMs);
                    g_conns.erase(g_conns.begin() + (long)i);
                    break;
                }
    }
    for (auto& id : toAnnounce) EmitJoin(id);
    for (auto& id : banned) {
        PluginLog("events: banned player %s is online; kicking", id.c_str());
        Actions::KickBanned(id);
    }
}

// Safety net: if the APlayerState we saw at join is gone from the live object array, the connection
// is gone even when no hook fired (hard drop, or a subclass we did not bind).
void ReapGoneConnections() {
    std::vector<std::pair<void*, void*>> announced;
    {
        Guard g(g_connLock);
        for (auto& c : g_conns)
            if (c.announced && !c.left && c.playerState) announced.push_back({c.controller, c.playerState});
    }
    if (announced.empty() || !g_clsPlayerState) return;
    std::vector<void*> states;
    if (!Reflect::GetObjectsOfClass(g_clsPlayerState, states, true)) return;
    for (auto& a : announced) {
        bool alive = false;
        for (void* s : states)
            if (s == a.second) { alive = true; break; }
        if (!alive) OnLeave(a.first, "player state gone");
    }
}

// =================================================================================================
// ProcessEvent hooks (chat / death / kill)
//
// VEIN's exact UFunction names are not confirmed on a live server yet, so the filter is a *candidate
// set* per event rather than one hard-coded name: every candidate that exists in the engine name
// pool is watched, the first one that fires wins, and its parameter list is dumped to plugin.log
// once so the next lane can narrow it without guessing again. docs/events-design.md lists them.

struct PeEntry {
    void* vtable = nullptr;
    void* orig = nullptr;
};
PeEntry g_pe[256];
std::atomic<size_t> g_peCount{0};

// `kDeath` is the *function* kind for every death event; whether it becomes a player-death or an
// entity-killed is decided from the dying actor's class at runtime, because VEIN routes both through
// the same UHealthComponent::NetMulticast_OnDeath. `kKill` only ever labels a *class* target, i.e.
// which vtables are hooked on behalf of the entity-killed capability.
enum Kind { kNone = 0, kChat = 1, kDeath = 2, kKill = 3 };

struct FnCandidate {
    const char* name;
    Kind kind;
    FName fname{};
    std::atomic<uint64_t> seen{0};
};

// The names below are the ones the depot `.sym` proves exist as UFUNCTIONs, via their
// `Z_Construct_UFunction_<Class>_<Name>()` registrars (the .usmap carries no function table at all,
// so it could not answer this). docs/events-design.md lists the evidence line for each.
//
//   chat in   AVeinPlayerController::Server_Say(FString const&, EChatSegment,
//                                               TSubclassOf<UChatCommand>)   - the server RPC
//   chat out  AVeinGameStateBase::NetMulticast_SendChat(AVeinPlayerState*, FString const&,
//                                               EChatSegment, TSubclassOf<UChatCommand>,
//                                               FVector_NetQuantize)         - the broadcast
//   death     UHealthComponent::NetMulticast_OnDeath(float, UDamageType const*, FVector, FName,
//                                               FPointDamageEvent, FRadialDamageEvent, AActor*,
//                                               AController*, int)           - universal
//             AVeinBaseCharacter::OnDeath(... same signature ...)            - per character
//
// The remaining entries are cheap insurance: a name that is not in the engine's name pool resolves
// to FName 0 and is skipped, so an extra candidate costs one comparison at boot and nothing after.
FnCandidate g_candidates[] = {
    {"Server_Say", kChat},          {"Say", kChat},            {"NetMulticast_SendChat", kChat},
    {"Server_SendChatMessage", kChat},

    {"NetMulticast_OnDeath", kDeath}, {"OnDeath", kDeath},     {"UpdateOnDeath", kDeath},
    {"CheckDeath", kDeath},
};
const size_t kCandidateCount = sizeof(g_candidates) / sizeof(g_candidates[0]);
bool g_fnNamesReady = false;
size_t g_chatCandidates = 0, g_deathCandidates = 0, g_killCandidates = 0;

// The FName the chat hook actually saw fire, for /health.
Mutex g_liveNameLock;
std::string g_chatFn, g_deathFn, g_killFn;
std::atomic<bool> g_liveNameSeen[4] = {};  // L9: one flag per Kind, so the hot path takes no lock


void NoteLiveFn(Kind k, const std::string& n) {
    Guard g(g_liveNameLock);
    if (k == kChat && g_chatFn.empty()) g_chatFn = n;
    // One UFunction feeds both the player-death and the entity-killed capability - the dying actor's
    // class is what separates them - so a death event names itself in both places.
    if (k == kDeath || k == kKill) {
        if (g_deathFn.empty()) g_deathFn = n;
        if (g_killFn.empty()) g_killFn = n;
    }
}
std::string LiveFn(Kind k) {
    Guard g(g_liveNameLock);
    return k == kChat ? g_chatFn : (k == kDeath ? g_deathFn : g_killFn);
}

// Dedupe windows.
Mutex g_deathLock;
std::map<std::string, uint64_t> g_lastDeathMs;
std::map<void*, uint64_t> g_lastKillMs;
const uint64_t kDeathDedupeMs = 3000;
const uint64_t kKillDedupeMs = 5000;
// chat: the ProcessEvent hook and the log tail both see the same message.
Mutex g_chatSeenLock;
std::vector<std::pair<std::string, uint64_t>> g_recentChat;
const uint64_t kChatDedupeMs = 8000;

// DWARF: `enum class EChatSegment : unsigned char { All=0, Local=1, Global=2, Radio=3 }`. Defined
// with the chat parameter reader further down; declared here because EmitChat reports it.
const char* ChatSegmentName(int v);
const char* ChatChannelName(int v);  // LANE L3e

bool DeathAllowed(const std::string& gameId) {
    Guard g(g_deathLock);
    uint64_t now = NowMs();
    auto it = g_lastDeathMs.find(gameId);
    if (it != g_lastDeathMs.end() && now - it->second < kDeathDedupeMs) return false;
    g_lastDeathMs[gameId] = now;
    return true;
}

bool KillAllowed(void* actor) {
    Guard g(g_deathLock);
    uint64_t now = NowMs();
    for (auto it = g_lastKillMs.begin(); it != g_lastKillMs.end();)
        it = (now - it->second > kKillDedupeMs) ? g_lastKillMs.erase(it) : ++it;
    if (g_lastKillMs.count(actor)) return false;
    g_lastKillMs[actor] = now;
    return true;
}

// Returns true the first time this (gameId, msg) is seen inside the window.
bool ChatAllowed(const std::string& gameId, const std::string& msg) {
    std::string key = gameId + "\x1f" + msg;
    Guard g(g_chatSeenLock);
    uint64_t now = NowMs();
    for (size_t i = 0; i < g_recentChat.size();) {
        if (now - g_recentChat[i].second > kChatDedupeMs) g_recentChat.erase(g_recentChat.begin() + (long)i);
        else i++;
    }
    for (auto& e : g_recentChat)
        if (e.first == key) return false;
    if (g_recentChat.size() > 64) g_recentChat.erase(g_recentChat.begin());
    g_recentChat.push_back({key, now});
    return true;
}

void EmitChat(const Ident& id, const std::string& msg, const std::string& channel, const char* source,
              int segment = -1) {
    if (msg.empty()) return;
    if (::state::ConsumeInjectedMessage(msg)) {
        if (DebugEnabled()) PluginLog("events: ignoring our own injected chat message (%s)", source);
        return;
    }
    if (!ChatAllowed(id.gameId, msg)) return;
    PluginState::Get().EmitEventDeferred("chat-message", [id, msg, channel, segment, source = std::string(source)] {
    std::string o = "{\"msg\":" + JsonStr(msg) + ",\"channel\":" + JsonStr(channel.empty() ? "global" : channel);
    if (segment >= 0) {
        const char* seg = ChatSegmentName(segment);
        o += ",\"chatSegment\":" + (seg ? JsonStr(seg) : std::to_string(segment));
    }
    if (id.valid()) o += ",\"player\":" + PlayerJson(id);
    o += ",\"source\":" + JsonStr(source) + "}";
    return o;
    });
    g_chat.emitted++;
    PluginLog("events: chat-message from %s via %s: %s", id.gameId.c_str(), source, msg.c_str());
}

void EmitDeath(const Ident& id, bool havePos, double x, double y, double z, const Ident& attacker,
               const std::string& killerEntity, const std::string& cause, const char* source) {
    if (!id.valid() || !DeathAllowed(id.gameId)) return;
    PluginState::Get().EmitEventDeferred("player-death", [id, havePos, x, y, z, attacker, killerEntity, cause,
                                                         source = std::string(source)] {
    std::string o = "{\"player\":" + PlayerJson(id);
    if (havePos) o += ",\"position\":{\"x\":" + JsonNum(x) + ",\"y\":" + JsonNum(y) + ",\"z\":" + JsonNum(z) + "}";
    if (attacker.valid()) o += ",\"attacker\":" + PlayerJson(attacker);
    if (!killerEntity.empty()) o += ",\"killerEntity\":" + JsonStr(killerEntity);
    if (!cause.empty()) o += ",\"cause\":" + JsonStr(cause);
    o += ",\"source\":" + JsonStr(source) + "}";
    return o;
    });
    g_death.emitted++;
    PluginLog("events: player-death %s via %s (killer '%s', cause '%s')", id.gameId.c_str(), source,
              killerEntity.c_str(), cause.c_str());
}

// Dumps a UFunction's parameter list to plugin.log, once per function.
std::set<void*> g_dumpedFns;
Mutex g_dumpLock;
void DumpParamsOnce(void* func, const char* label) {
    {
        Guard g(g_dumpLock);
        if (g_dumpedFns.count(func)) return;
        g_dumpedFns.insert(func);
    }
    std::vector<void*> props;
    WalkProps(func, props, 24);
    std::string list;
    for (void* p : props)
        list += FieldName(p) + ":" + Reflect::PropertyTypeName(p) + "@" +
                std::to_string(Reflect::PropertyOffset(p)) + " ";
    PluginLog("events: %s params = %s", label, list.c_str());
}

// Reads an RPC's parameters by reflection. Nothing is assumed: the parameter list is the
// UFunction's own FField chain, every offset comes from FProperty::Offset_Internal, and a parameter
// that is not there simply stays empty.
//
//   Server_Say(FString Message, EChatSegment Segment, TSubclassOf<UChatCommand> Command)
//   NetMulticast_SendChat(AVeinPlayerState* Sender, FString Message, EChatSegment Segment,
//                         TSubclassOf<UChatCommand> Command, FVector_NetQuantize Location)
//
// so the *first* FString is the message in both, and the multicast additionally names its sender.
// DWARF: `enum class EChatSegment : unsigned char { All=0, Local=1, Global=2, Radio=3 }`.
// Takaro's chat schema only knows global/team/friends/whisper, so Local and Radio have no honest
// Takaro equivalent and every VEIN segment maps to "global"; the *name* rides along as
// `chatSegment` so the distinction is not lost, and a value outside the enum is reported as a
// number rather than mislabelled.
// LANE L3e: both mappings moved to events_parse.cpp so the unit tests exercise the shipped code.
const char* ChatSegmentName(int v) { return EventsParse::ChatSegmentName(v); }
const char* ChatChannelName(int v) { return EventsParse::ChatChannelName(v); }

struct ChatParams {
    std::string msg;
    void* sender = nullptr;  // an AVeinPlayerState, only on the multicast form
    int segment = -1;        // the raw EChatSegment value
    bool haveSegment = false;
};

ChatParams ReadChatParams(void* func, void* params) {
    ChatParams out;
    std::vector<void*> props;
    WalkProps(func, props, 24);
    for (void* prop : props) {
        std::string type = Reflect::PropertyTypeName(prop);
        std::string pname = FieldName(prop);
        int32_t off = Reflect::PropertyOffset(prop);
        if (off < 0) continue;
        if (out.msg.empty() && type == "StrProperty") {
            out.msg = ReadFStringAt(params, off);
        } else if (out.msg.empty() && type == "TextProperty") {
            auto disp = SymFn<const FString* (*)(const void*)>("FTextInspector::GetDisplayString");
            const void* t = (const char*)params + off;
            if (disp && MemReadable(t, 16)) {
                const FString* str = disp(t);
                if (str && MemReadable(str, 16) && str->Num > 0 && str->Num < 65536)
                    out.msg = Reflect::Utf16To8(str->Data, str->Num);
            }
        } else if (!out.sender && type.find("Object") != std::string::npos) {
            void* o = nullptr;
            // A TSubclassOf is an ObjectProperty too, so the candidate must really be a player state.
            if (ReadAt(params, off, o) && ValidObject(o) && PlayerStateOf(o)) out.sender = o;
        }
        if (!out.haveSegment && (type == "ByteProperty" || type == "EnumProperty") &&
            (pname.find("Segment") != std::string::npos || pname.find("Channel") != std::string::npos ||
             pname.find("ChatType") != std::string::npos)) {
            uint8_t v = 0;
            if (ReadAt(params, off, v)) {
                out.segment = (int)v;
                out.haveSegment = true;
            }
        }
    }
    return out;
}

void HandleChat(void* self, void* func, void* params) {
    DumpParamsOnce(func, "chat RPC");
    ChatParams cp = ReadChatParams(func, params);
    if (cp.msg.empty()) return;
    Ident id;
    // The multicast form names its sender; the Server_Say form is called *on* the controller.
    void* st = cp.sender ? PlayerStateOf(cp.sender) : PlayerStateOf(self);
    if (st) IdentFromPlayerState(st, id);
    // LANE L3e (minimal edit): the channel is the EChatSegment the call carried, not a constant.
    // L6b finding 3 - a message typed on Local came back as "global", so `onlyGlobalChat` would
    // relay local chat too. ChatChannelName() maps All/Local/Global/Radio -> all/local/global/radio;
    // an unknown or absent segment still falls back to "global". The raw name still rides along as
    // `chatSegment`.
    EmitChat(id, cp.msg, ChatChannelName(cp.haveSegment ? cp.segment : -1), "processevent",
             cp.haveSegment ? cp.segment : -1);
}

// Reads a location out of an actor: RootComponent -> RelativeLocation (FVector of doubles in UE5).
bool ActorLocation(void* actor, double& x, double& y, double& z) {
    void* root = nullptr;
    int32_t off = PropOffOf(actor, "RootComponent");
    if (off < 0 || !ReadAt(actor, off, root) || !ValidObject(root)) return false;
    int32_t lo = PropOffOf(root, "RelativeLocation");
    if (lo < 0 || !MemReadable((const char*)root + lo, 24)) return false;
    const double* v = (const double*)((const char*)root + lo);
    x = v[0];
    y = v[1];
    z = v[2];
    return true;
}

// An object property on the actor (or in the params block) naming who dealt the damage.
void* InstigatorOf(void* actor) {
    static const char* kHints[] = {"LastDamageInstigator", "LastDamageCauser", "DamageInstigator",
                                   "DamageCauser", "Killer", "Instigator", "LastAttacker"};
    for (const char* h : kHints) {
        int32_t off = PropOffOf(actor, h);
        void* o = nullptr;
        if (off >= 0 && ReadAt(actor, off, o) && ValidObject(o)) return o;
    }
    return nullptr;
}

std::string CauseOf(void* actor) {
    for (const char* h : {"DeathCause", "DeathReason", "ZombieDeathCause"}) {
        void* cls = Reflect::ObjClass(actor);
        void* prop = nullptr;
        for (void* c = cls; c && !prop; c = Reflect::SuperStruct(c)) prop = Reflect::FindProperty(c, h);
        if (!prop) continue;
        int32_t off = Reflect::PropertyOffset(prop);
        if (off < 0) continue;
        std::string type = Reflect::PropertyTypeName(prop);
        if (type == "StrProperty") return ReadFStringAt(actor, off);
        if (type == "NameProperty") {
            FName n;
            if (ReadAt(actor, off, n)) return Reflect::NameToString(n);
        }
        if (type == "ByteProperty" || type == "EnumProperty") {
            uint8_t v = 0;
            if (ReadAt(actor, off, v)) return std::string(h) + ":" + std::to_string((int)v);
        }
    }
    return "";
}

// The display name of an AI actor, exactly as GET /entities prints it: the cached catalogue name,
// else AVeinAnimalCharacter::UsableName (the only readable creature name on this build - zombies
// have none), else the HUMANISED class name (`BP_Zombie_C` -> "Zombie"). Lane L2c: the old last
// resort was the RAW class name, which made entity-killed disagree with the catalogue it is
// supposed to reference. Whatever is read is cached per class so the two always agree.
std::string EntityDisplayName(void* actor) {
    std::string cls = SafeClassName(actor);
    if (cls.empty()) return "";
    std::string name = ::state::EntityName(cls);
    if (name.empty()) name = ReadFTextAt(actor, PropOffOf(actor, "UsableName"));
    if (!name.empty()) {
        ::state::NoteEntityName(cls, name);
        return name;
    }
    return ActionsUtil::HumaniseCode(cls);
}

// UHealthComponent::NetMulticast_OnDeath and AVeinBaseCharacter::OnDeath share one signature:
//   (float Damage, UDamageType const* DamageType, FVector HitLocation, FName BoneName,
//    FPointDamageEvent, FRadialDamageEvent, AActor* DamageCauser, AController* InstigatorController,
//    int HitCount)
// so the killer is *in the event* and needs no heuristic at all. Every field is still located by
// FindPropertyByName, so a signature change degrades one field instead of reading garbage.
struct DeathParams {
    void* causer = nullptr;       // AActor* that dealt the damage (a weapon, a zombie, a vehicle)
    void* instigator = nullptr;   // AController* behind it - a player controller for a player kill
    void* damageType = nullptr;
    double x = 0, y = 0, z = 0;
    bool havePos = false;
    float damage = 0.0f;
    bool any = false;
};

DeathParams ReadDeathParams(void* func, void* params) {
    DeathParams out;
    if (!func || !params) return out;
    std::vector<void*> props;
    WalkProps(func, props, 24);
    for (void* prop : props) {
        std::string pname = FieldName(prop);
        std::string type = Reflect::PropertyTypeName(prop);
        int32_t off = Reflect::PropertyOffset(prop);
        if (off < 0) continue;
        if (type.find("Object") != std::string::npos) {
            void* o = nullptr;
            if (!ReadAt(params, off, o) || !ValidObject(o)) continue;
            if (pname.find("Causer") != std::string::npos) { out.causer = o; out.any = true; }
            else if (pname.find("Instigator") != std::string::npos) { out.instigator = o; out.any = true; }
            else if (pname.find("DamageType") != std::string::npos) { out.damageType = o; }
        } else if (type == "StructProperty" && !out.havePos &&
                   (pname.find("Location") != std::string::npos || pname.find("HitLoc") != std::string::npos)) {
            if (MemReadable((const char*)params + off, 24)) {
                const double* v = (const double*)((const char*)params + off);
                out.x = v[0];
                out.y = v[1];
                out.z = v[2];
                out.havePos = (out.x != 0.0 || out.y != 0.0 || out.z != 0.0);
                out.any = out.any || out.havePos;
            }
        } else if (type == "FloatProperty" && pname.find("Damage") != std::string::npos && out.damage == 0.0f) {
            ReadAt(params, off, out.damage);
        }
    }
    return out;
}


// The controller/actor that dealt the damage -> a player Ident, else a readable creature name.
//
// LANE L2c / L8 finding: VEIN passes the VICTIM'S OWN PAWN as `DamageCauser` and his own controller
// as `DamageInstigator` for a death with no killer - a fall, drowning, the cold. The game's own log
// says so in as many words: "Player Limon (765…875) was killed by Limon (765…875) with
// BP_VeinPlayerCharacter_C_2147469505". The old code copied that faithfully and every environmental
// death arrived in Takaro with `attacker` = the dead player, which reads as a suicide and, worse,
// scores as a player-vs-player kill. A candidate that IS the victim is therefore skipped: an
// environmental death has no attacker at all, and saying nothing is the only honest answer this
// event can give. (A genuine self-inflicted death is indistinguishable from a fall on this build -
// the wire carries exactly the same three pointers - so it, too, is reported as environmental.)
void AttributeFrom(const DeathParams& dp, void* victimActor, const Ident& victim,
                   Ident& attackerResult, std::string& killerEntity, Ident* killerOut) {
    void* candidates[3] = {dp.instigator, dp.causer, victimActor ? InstigatorOf(victimActor) : nullptr};
    auto isVictim = [&](void* c) {
        if (c == victimActor) return true;
        void* st = PlayerStateOf(c);
        Ident who;
        if (st && IdentFromPlayerState(st, who) && who.valid() && victim.valid())
            return who.gameId == victim.gameId;
        return false;
    };
    for (void* c : candidates) {
        if (!ValidObject(c) || isVictim(c)) continue;
        Ident attacker;
        void* st = PlayerStateOf(c);
        if (st && IdentFromPlayerState(st, attacker) && attacker.valid()) {
            attackerResult = attacker;
            if (killerOut) *killerOut = attacker;
            return;
        }
    }
    for (void* c : candidates) {
        if (!ValidObject(c) || isVictim(c)) continue;
        // A creature killer is named the way the catalogue names it ("Zombie", "Wolf"), not by its
        // Blueprint class - the same rule entity-killed now follows, so the two events agree.
        killerEntity = IsAnyAi(c) ? EntityDisplayName(c) : SafeClassName(c);
        if (!killerEntity.empty()) return;
    }
}

void HandlePlayerDeath(void* actor, const DeathParams& dp, const char* via) {
    if (!ValidObject(actor)) return;
    void* st = PlayerStateOf(actor);
    Ident victim;
    if (!st || !IdentFromPlayerState(st, victim) || !victim.valid()) return;

    double x = dp.x, y = dp.y, z = dp.z;
    bool havePos = dp.havePos;
    if (!havePos) havePos = ActorLocation(actor, x, y, z);
    Ident attacker;
    std::string killerEntity;
    AttributeFrom(dp, actor, victim, attacker, killerEntity, nullptr);
    // LANE L2c: when nothing and nobody killed him, `cause` is what is left to say. VEIN's own
    // DeathCause/DeathReason first, then the damage type the event carried (humanised, e.g.
    // "Vein Damage Type Fall"), and "environment" when the build exposes neither.
    std::string cause = CauseOf(actor);
    if (cause.empty() && !attacker.valid() && killerEntity.empty()) {
        if (ValidObject(dp.damageType)) cause = ActionsUtil::HumaniseCode(SafeClassName(dp.damageType));
        if (cause.empty()) cause = "environment";
    }
    EmitDeath(victim, havePos, x, y, z, attacker, killerEntity, cause, via);
    {
        Guard g(g_healthLock);
        g_liveness[victim.gameId] = Live::Dead;
    }
}

void* OwnerOf(void* comp);

// Who killed this AI. In order of confidence; the route used goes into the event's `attribution`.
Ident KillerOf(void* actor, const DeathParams& dp, std::string& how) {
    Ident id;
    // 1. the death event's own InstigatorController / DamageCauser - the game's own answer.
    for (auto pair : {std::make_pair(dp.instigator, "the death event's instigator controller"),
                      std::make_pair(dp.causer, "the death event's damage causer")}) {
        if (!ValidObject(pair.first)) continue;
        void* st = PlayerStateOf(pair.first);
        if (st && IdentFromPlayerState(st, id) && id.valid()) {
            how = pair.second;
            return id;
        }
        id = Ident();
    }
    // 2. a property on the AI naming who hit it last.
    void* inst = InstigatorOf(actor);
    if (inst) {
        void* st = PlayerStateOf(inst);
        if (st && IdentFromPlayerState(st, id) && id.valid()) {
            how = "the AI's recorded damage instigator";
            return id;
        }
        id = Ident();
    }
    // Routes 3 and 4 are GONE (lane L2c). They were "what the AI was chasing" (the sensed-target
    // component) and "there is exactly one player online, so it was them". Both are guesses, and
    // the second one is what made every wolf a zombie ate on 2026-09-17 arrive in Takaro as a kill
    // by Tester - 12 fabricated kills against 3 real ones in five minutes, with the game's own
    // kill log ("Player <name> killed non-player character ...") naming only the 3. An AI killed by
    // another AI is not a Takaro `entity-killed` at all, so the caller drops it instead.
    how = "unattributed";
    return Ident();
}

std::atomic<bool> g_aiDumped{false};
Mutex g_debugReportLock;
std::function<void()> g_debugReport;
// Deaths of non-player, non-AI actors (doors, item instances, built actors share the same event).
std::atomic<uint64_t> g_deathsIgnored{0};
// Lane L2c: AI deaths with no player behind them (a zombie eating a wolf). Not Takaro events.
std::atomic<uint64_t> g_killsUnattributed{0};
// Lane L2c: the victim of the most recent POST /debug/kill-nearest, so the death hook can report
// `weapon: "debug"` for it. Nothing was swung, and the endpoint hands the player's own pawn to the
// engine as DamageCauser, which is indistinguishable on the wire from a punch.
std::atomic<void*> g_debugKillVictim{nullptr};

void HandleAiDeath(void* actor, const DeathParams& dp, const char* via) {
    if (!IsAnyAi(actor)) return;
    if (!KillAllowed(actor)) return;

    if (DebugEnabled() && !g_aiDumped.exchange(true)) {
        auto snapshot = Reflect::DumpObject(actor, 256);
        std::string name = SafeClassName(actor);
        Guard guard(g_debugReportLock);
        g_debugReport = [snapshot = std::move(snapshot), name] {
            PluginLog("events: first AI death, %s properties = %s", name.c_str(), snapshot().c_str());
        };
    }

    std::string cls = SafeClassName(actor);
    std::string entity = EntityDisplayName(actor);

    std::string how;
    Ident killer = KillerOf(actor, dp, how);
    // Lane L2c: Takaro's `entity-killed` is "a PLAYER killed this entity" - it is what the kill
    // leaderboard counts. An AI eaten by another AI has no player behind it, and inventing one is
    // worse than reporting nothing, so it is counted here and dropped.
    if (!killer.valid()) {
        g_killsUnattributed++;
        if (DebugEnabled())
            PluginLog("events: %s died with no player behind it (causer %s) - not a Takaro kill",
                      SafeClassName(actor).c_str(), SafeClassName(dp.causer).c_str());
        return;
    }
    double x = dp.x, y = dp.y, z = dp.z;
    bool havePos = dp.havePos;
    if (!havePos) havePos = ActorLocation(actor, x, y, z);
    // Lane L2c: `weapon` is the KILLER'S WEAPON, never the causing actor's class and never the
    // victim's. The death event's DamageCauser is an actor: an AEquippedItem for a swing or a shot
    // (-> the item's display name, "Baseball Bat"), the killer's own pawn for the debug kill and
    // for a bite (-> no weapon at all). The killer's CURRENT loadout is deliberately not consulted
    // as a fallback: what he happens to hold now is not evidence of what dealt this damage. When
    // the causer names no weapon the field is simply OMITTED.
    std::string weapon;
    if (ValidObject(dp.causer))
        weapon = ActionsUtil::KillWeaponName(SafeClassName(dp.causer), Actions::EquippedItemName(dp.causer));
    // Takaro's `EventEntityKilled` REQUIRES `weapon` to be a string and drops the whole event when
    // it is absent (measured 2026-09-17: "property weapon has failed the following constraints:
    // isString"), so the unknown cases get a word instead of silence - never a pawn or victim class.
    //   "debug"   - POST /debug/kill-nearest killed this actor; nothing was swung.
    //   "unknown" - a real hit whose causer names no item. Deliberately NOT "unarmed": a punch and
    //               an unnamed weapon look identical on this build, and "unarmed" would be a claim.
    if (weapon.empty()) weapon = (g_debugKillVictim.exchange(nullptr) == actor) ? "debug" : "unknown";

    // The actor instance name (BP_Zombie_Male_C_2147482301) is evidence, not identity - it makes a
    // kill traceable back to one spawned actor in plugin.log and /events.
    std::string instance = SafeObjName(actor);
    PluginState::Get().EmitEventDeferred("entity-killed", [entity, instance, cls, how, weapon, havePos, x, y, z,
                                                          killer, via = std::string(via)] {
    std::string o = "{\"entity\":" + JsonStr(entity) + ",\"entityInstance\":" + JsonStr(instance) +
                    ",\"entityCode\":" + JsonStr(cls) +
                    ",\"entityClass\":" + JsonStr(cls) +
                    ",\"source\":" + JsonStr(via) + ",\"attribution\":" + JsonStr(how);
    o += ",\"weapon\":" + JsonStr(weapon);
    if (havePos) o += ",\"position\":{\"x\":" + JsonNum(x) + ",\"y\":" + JsonNum(y) + ",\"z\":" + JsonNum(z) + "}";
    if (killer.valid()) o += ",\"player\":" + PlayerJson(killer);
    o += "}";
    return o;
    });
    g_kill.emitted++;
    PluginLog("events: entity-killed '%s' (%s) via %s, killer %s [%s]", entity.c_str(), cls.c_str(), via,
              killer.gameId.c_str(), how.c_str());
}

// The actor a component belongs to (components are Outered to their owner).
void* OwnerOf(void* comp) {
    if (!ValidObject(comp)) return nullptr;
    int32_t ownerOff = PropOffOf(comp, "Owner");
    void* o = nullptr;
    if (ownerOff >= 0 && ReadAt(comp, ownerOff, o) && ValidObject(o)) return o;
    void* outer = Reflect::ObjOuter(comp);
    return ValidObject(outer) ? outer : nullptr;
}

// A death reported by a generic hook: route it to the player path or the AI path by what `self` is.
void HandleDeathDispatch(void* self, void* func, void* params, const char* via) {
    // `self` is the UHealthComponent for NetMulticast_OnDeath and the character itself for OnDeath;
    // either way the *actor* is what decides whether this is a player death or an entity kill.
    void* actor = self;
    if (ValidObject(actor) && g_clsHealthComponent && Reflect::IsA(actor, g_clsHealthComponent))
        actor = OwnerOf(actor);
    if (!ValidObject(actor)) return;
    DeathParams dp = ReadDeathParams(func, params);
    if (IsAnyAi(actor)) {
        HandleAiDeath(actor, dp, via);
        return;
    }
    // A player character, or anything else with a player state behind it.
    if (PlayerStateOf(actor)) {
        HandlePlayerDeath(actor, dp, via);
        return;
    }
    // Neither: a door, an item instance or a built actor also runs through NetMulticast_OnDeath.
    // Those are not Takaro events, so they are counted and dropped rather than mis-reported.
    g_deathsIgnored++;
}

// LANE L9 (performance): this detour sits on the engine's own RPC path, so its cost is paid by the
// server on every replicated call of every hooked object. Three things were measured and removed:
//
//  1. the linear scan over up to 256 hooked vtables -> an open-addressed pointer table;
//  2. the FName read + candidate loop on EVERY call -> a UFunction* -> Kind cache. UFunction objects
//     are permanent, so the second call for a given function is a single pointer probe. Misses are
//     cached too, which is the overwhelming case (a server RPC we do not care about);
//  3. MemReadable()'s mutex + /proc/self/maps rescan -> a lock-free snapshot (see resolve.cpp).
//
// The filter still never stringifies an FName: a cache miss compares raw FName values exactly as
// before, and the cache is keyed on the pointer the engine itself just dispatched.
const size_t kPeSlots = 1024;  // power of two; open addressing, never resized
struct PeSlot {
    std::atomic<void*> vtable{nullptr};
    void* orig = nullptr;
};
PeSlot g_peSlots[kPeSlots];

inline size_t PtrHash(void* p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdull; v ^= v >> 33;
    return (size_t)v & (kPeSlots - 1);
}

void PeRemember(void* vt, void* orig) {
    size_t i = PtrHash(vt);
    for (size_t n = 0; n < 32; n++, i = (i + 1) & (kPeSlots - 1)) {
        void* cur = g_peSlots[i].vtable.load(std::memory_order_acquire);
        if (cur == vt) return;
        if (!cur) {
            g_peSlots[i].orig = orig;
            g_peSlots[i].vtable.store(vt, std::memory_order_release);
            return;
        }
    }
}

inline void* PeLookup(void* vt) {
    size_t i = PtrHash(vt);
    for (size_t n = 0; n < 32; n++, i = (i + 1) & (kPeSlots - 1)) {
        void* cur = g_peSlots[i].vtable.load(std::memory_order_acquire);
        if (!cur) return nullptr;
        if (cur == vt) return g_peSlots[i].orig;
    }
    return nullptr;
}

// UFunction* -> decision cache. `kind` is the Kind we already decided for this function; a cached
// kNone means "we looked at this function once and it is not ours".
const size_t kFnSlots = 2048;
struct FnSlot {
    std::atomic<void*> func{nullptr};
    uint8_t kind = 0;
    int16_t candidate = -1;
};
FnSlot g_fnSlots[kFnSlots];

inline size_t FnHash(void* p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 29; v *= 0xbf58476d1ce4e5b9ull; v ^= v >> 32;
    return (size_t)v & (kFnSlots - 1);
}

void DetourProcessEvent(void* self, void* func, void* params) {
    uint64_t tFilter0 = Perf::NowNs();
    FnProcessEvent orig = nullptr;
    if (self) {
        void* vt = *(void**)self;  // the engine just dispatched through this vtable: it is live
        orig = (FnProcessEvent)PeLookup(vt);
    }
    if (!orig) orig = g_processEvent;

    // Read what we need *before* the call: RPC parameter buffers do not survive it.
    Kind what = kNone;
    FnCandidate* hit = nullptr;
    bool cacheHit = false;
    try {
        if (g_fnNamesReady && func) {
            size_t si = FnHash(func);
            FnSlot* free_ = nullptr;
            for (size_t n = 0; n < 16; n++, si = (si + 1) & (kFnSlots - 1)) {
                void* cur = g_fnSlots[si].func.load(std::memory_order_acquire);
                if (cur == func) {
                    cacheHit = true;
                    what = (Kind)g_fnSlots[si].kind;
                    if (g_fnSlots[si].candidate >= 0) hit = &g_candidates[g_fnSlots[si].candidate];
                    break;
                }
                if (!cur) { free_ = &g_fnSlots[si]; break; }
            }
            if (!cacheHit) {
                FName fn;
                int16_t which = -1;
                if (ObjNameRaw(func, fn)) {
                    for (size_t i = 0; i < kCandidateCount; i++) {
                        if (g_candidates[i].fname.Comparison == 0) continue;
                        if (!(g_candidates[i].fname == fn)) continue;
                        hit = &g_candidates[i];
                        what = g_candidates[i].kind;
                        which = (int16_t)i;
                        break;
                    }
                    if (free_) {  // remember the decision - including "not ours"
                        free_->kind = (uint8_t)what;
                        free_->candidate = which;
                        free_->func.store(func, std::memory_order_release);
                    }
                }
            }
        }
        if (hit) {
            hit->seen++;
            // The live-name registry only needs the FIRST hit per kind; the atomic flag keeps the
            // mutex off the hot path for every hit after that.
            if (!g_liveNameSeen[what & 3].exchange(true)) NoteLiveFn(what, hit->name);
        }
        Perf::RecordFilter(Perf::NowNs() - tFilter0, hit != nullptr, cacheHit);
        uint64_t tHandler0 = what != kNone ? Perf::NowNs() : 0;
        if (what == kChat) {
            g_chat.fired++;
            HandleChat(self, func, params);
        } else if (what == kDeath) {
            g_death.fired++;
            g_kill.fired++;  // one event feeds both capabilities; the dispatch decides which emits
            DumpParamsOnce(func, "death event");
            HandleDeathDispatch(self, func, params, hit ? hit->name : "death");
        }
        if (tHandler0) Perf::RecordHandler(Perf::NowNs() - tHandler0);
    } catch (...) {
        PluginLog("events: ProcessEvent handler threw (kind=%d)", (int)what);
    }
    if (orig) orig(self, func, params);
}

Mutex g_vtLock;
std::set<void*> g_hookedVts;

bool HookObjectProcessEvent(const std::string& name, void* obj) {
    if (!obj || !MemReadable(obj, 8)) return false;
    void* vt = *(void**)obj;
    if (!vt) return false;
    {
        Guard g(g_vtLock);
        if (g_hookedVts.count(vt)) return false;
    }
    size_t slot = Resolve::ProcessEventSlot();
    if (slot == SIZE_MAX) return false;
    if (g_peCount.load() >= 256) return false;
    void* orig = nullptr;
    std::string err;
    if (!Hooks::HookObjectVTable(name, obj, slot, (void*)&DetourProcessEvent, &orig, err)) {
        PluginLog("events: ProcessEvent hook on %s failed: %s", name.c_str(), err.c_str());
        return false;
    }
    size_t idx = g_peCount.load();
    g_pe[idx].vtable = vt;
    g_pe[idx].orig = orig;
    g_peCount.store(idx + 1);
    PeRemember(vt, orig);  // L9: the O(1) table the detour actually reads
    Guard g(g_vtLock);
    g_hookedVts.insert(vt);
    PluginLog("events: ProcessEvent hooked on %s (vtable %p, orig %p)", name.c_str(), vt, orig);
    return true;
}

struct ClassTarget {
    const char* cls;
    const char* label;
    Kind kind;
    void* resolved = nullptr;
    size_t hooked = 0;
    size_t objects = 0;  // lane L2b: live objects seen at the last sweep - separates "no such object
                         // exists yet" from "the object exists and the hook failed"
};
// Blueprint subclasses (BP_VeinPlayerController_C, BP_Zombie_*_C) do not generate their own C++
// vtable, so hooking the *live objects* of these classes covers the whole bestiary and every
// Blueprint controller variant.
ClassTarget g_targets[] = {
    // chat: Server_Say arrives on the player controller, NetMulticast_SendChat on the game state.
    {"VeinPlayerController", "AVeinPlayerController::ProcessEvent", kChat},
    {"VeinBasePlayerController", "AVeinBasePlayerController::ProcessEvent", kChat},
    {"VeinGameStateBase", "AVeinGameStateBase::ProcessEvent", kChat},
    // player death: AVeinBaseCharacter::OnDeath, overridden down the chain.
    {"VeinPlayerCharacter", "AVeinPlayerCharacter::ProcessEvent", kDeath},
    {"VeinCharacter", "AVeinCharacter::ProcessEvent", kDeath},
    {"VeinBaseCharacter", "AVeinBaseCharacter::ProcessEvent", kDeath},
    // entity kills: the zombie/animal overrides plus the one universal event,
    // UHealthComponent::NetMulticast_OnDeath - VEIN has no separate AI health component.
    {"VeinZombieCharacter", "AVeinZombieCharacter::ProcessEvent", kKill},
    {"VeinAnimalCharacter", "AVeinAnimalCharacter::ProcessEvent", kKill},
    {"VeinBaseDreamCharacter", "AVeinBaseDreamCharacter::ProcessEvent", kKill},
    {"HealthComponent", "UHealthComponent::ProcessEvent", kKill},
};
const size_t kTargetCount = sizeof(g_targets) / sizeof(g_targets[0]);

size_t HookedFor(Kind k) {
    size_t n = 0;
    for (auto& t : g_targets)
        if (t.kind == k) n += t.hooked;
    return n;
}

// lane L2b: a character's death is announced by its *health component*
// (UHealthComponent::NetMulticast_OnDeath), and a zombie's component is a ULimbHealthComponent held
// in the UPROPERTY `Health` - a different class, and therefore a different vtable, from the player's.
// Sweeping only the character classes leaves that vtable unhooked, which is half of why L6b saw
// entity-killed produce nothing. So every live character target also contributes its health
// component's vtable.
size_t HookHealthComponentOf(void* actor, const char* label) {
    if (!ValidObject(actor)) return 0;
    int32_t off = PropOffOf(actor, "Health");
    void* c = nullptr;
    if (off < 0 || !ReadAt(actor, off, c) || !ValidObject(c)) return 0;
    if (g_clsHealthComponent && !Reflect::IsA(c, g_clsHealthComponent)) return 0;
    return HookObjectProcessEvent(label, c) ? 1 : 0;
}

// LANE L9: GetObjectsOfClass walks the whole GUObjectArray, so a full pass over all ten targets
// cost ~1.5 ms of game thread every 2 s. The pass is now RESUMABLE: it starts where the last one
// stopped and gives up its slot as soon as `budgetUs` is spent, so no single game-thread entry
// holds the pump for more than roughly one class's worth of work. Coverage is unchanged - the next
// cycle picks up the remaining targets - and nothing is missed, because a hook is keyed on the
// class VTABLE: every later instance of an already-swept class is already hooked.
size_t g_sweepCursor = 0;
std::atomic<uint64_t> g_sweepPasses{0}, g_sweepTargetsDone{0}, g_sweepYields{0};
// A budgeted sweep may yield before it has seen every target. `g_sweepPending` keeps housekeeping
// sweeping on consecutive cycles until one FULL pass is done, so a join can never wait for the 15 s
// safety cadence to get its controller/chat vtable hooked.
std::atomic<bool> g_sweepPending{true};

void SweepProcessEventTargets(uint64_t budgetUs = 0) {
    const size_t n = sizeof(g_targets) / sizeof(g_targets[0]);
    uint64_t t0 = Perf::NowNs();
    for (size_t k = 0; k < n; k++) {
        if (budgetUs && k && Perf::NowNs() - t0 >= budgetUs * 1000ull) {
            g_sweepYields++;
            g_sweepPending = true;  // finish the pass on the next cycle
            return;
        }
        auto& t = g_targets[g_sweepCursor];
        g_sweepCursor = (g_sweepCursor + 1) % n;
        if (g_sweepCursor == 0) g_sweepPasses++;
        g_sweepTargetsDone++;
        if (!t.resolved) {
            t.resolved = FindByName(t.cls);
            if (!t.resolved) continue;
        }
        std::vector<void*> objs;
        if (!Reflect::GetObjectsOfClass(t.resolved, objs, true)) continue;
        t.objects = objs.size();
        for (void* o : objs) {
            if (HookObjectProcessEvent(t.label, o)) t.hooked++;
            if (t.kind == kKill || t.kind == kDeath) t.hooked += HookHealthComponentOf(o, t.label);
        }
    }
    g_sweepPending = false;  // every target was visited in this call
}

// =================================================================================================
// death fallback: a Health / Dead edge per connected player

void PollHealthEdges() {
    std::vector<std::pair<void*, Ident>> conns;
    {
        Guard g(g_connLock);
        for (auto& c : g_conns)
            if (c.announced && !c.left) conns.push_back({c.controller, c.id});
    }
    for (auto& c : conns) {
        void* st = PlayerStateOf(c.first);
        if (!st) continue;
        void* pawn = nullptr;
        int32_t pawnOff = PropOffOf(st, "PawnPrivate");
        if (pawnOff < 0 || !ReadAt(st, pawnOff, pawn) || !ValidObject(pawn)) continue;

        Live now = Live::Unknown;
        // 1. a bool `Dead`/`bIsDead` on the pawn is the cheapest and most direct signal.
        for (const char* p : {"Dead", "bIsDead", "bDead", "IsDead"}) {
            int32_t off = PropOffOf(pawn, p);
            uint8_t v = 0;
            if (off >= 0 && ReadAt(pawn, off, v)) {
                now = v ? Live::Dead : Live::Alive;
                break;
            }
        }
        // 2. else the health component's Health/MaxHealth. DWARF: `UHealthComponent::Health` is a
        //    **double** at 0xf8 (hence ReadNumericProp, which reads by declared type - a hard-coded
        //    float would have read the low half of a double and called every living player dead),
        //    and there is no bDead/bIsDead member anywhere, so step 1 above normally finds nothing
        //    and this is the real path.
        if (now == Live::Unknown && g_clsHealthComponent) {
            std::vector<void*> comps;
            // AVeinCharacter and the AI classes hold their ULimbHealthComponent in a UPROPERTY
            // called `Health`, so the component is usually one reflected read away; the outer sweep
            // is the fallback for a pawn that does not.
            void* direct = nullptr;
            int32_t hcOff = PropOffOf(pawn, "Health");
            if (hcOff >= 0 && ReadAt(pawn, hcOff, direct) && ValidObject(direct) &&
                Reflect::IsA(direct, g_clsHealthComponent))
                comps.push_back(direct);
            if (comps.empty() && !Reflect::GetObjectsWithOuter(pawn, comps, true)) continue;
            for (void* comp : comps) {
                if (!ValidObject(comp) || !Reflect::IsA(comp, g_clsHealthComponent)) continue;
                double hp = 0.0, maxHp = 0.0;
                if (!ReadNumericProp(comp, "Health", hp) && !ReadNumericProp(comp, "CurrentHealth", hp)) break;
                if (!ReadNumericProp(comp, "MaxHealth", maxHp)) break;
                if (!(maxHp > 0.0)) break;  // health not replicated yet: seeding it would be a lie
                now = hp > 0.0 ? Live::Alive : Live::Dead;
                break;
            }
        }
        if (now == Live::Unknown) continue;

        Live prev = Live::Unknown;
        {
            Guard g(g_healthLock);
            auto it = g_liveness.find(c.second.gameId);
            if (it != g_liveness.end()) prev = it->second;
            g_liveness[c.second.gameId] = now;
        }
        // Only a genuine alive -> dead transition is a death; the first sample only seeds.
        if (now == Live::Dead && prev == Live::Alive) {
            double x = 0, y = 0, z = 0;
            bool havePos = ActorLocation(pawn, x, y, z);
            Ident attacker;
            std::string killerEntity;
            DeathParams none;
            AttributeFrom(none, pawn, c.second, attacker, killerEntity, nullptr);
            std::string cause = CauseOf(pawn);
            if (cause.empty() && !attacker.valid() && killerEntity.empty()) cause = "environment";
            EmitDeath(c.second, havePos, x, y, z, attacker, killerEntity, cause, "health-edge");
        }
    }
}

// =================================================================================================
// log tail

std::string g_logPath;
Mutex g_rawLogLock;
std::function<void(std::string)> g_rawLogSink;
bool g_customLogJoin = false, g_customLogChat = false;
int g_logFd = -1;
uint64_t g_logInode = 0;
off_t g_logOffset = 0;
std::string g_logPartial;
std::atomic<uint64_t> g_logDropped{0};
std::atomic<uint64_t> g_logChat{0};
uint64_t g_logStartMs = 0;
const uint64_t kBootRotationGraceMs = 180000;
const size_t kMaxLogPerCycle = 120;
const size_t kMaxReadPerCycle = 512 * 1024;

std::string DefaultLogPath() {
    const char* legacy = getenv("VEIN_LOG_FILE");
    if (legacy && *legacy) return legacy;
    std::string cfg = ConfigValue("TAKARO_LOG_PATH", "logPath", "");
    if (!cfg.empty()) return cfg;
    // <exe dir> = <root>/Vein/Binaries/Linux -> <root>/Vein/Saved/Logs/Vein.log
    std::string dir = ExeDir();
    size_t bin = dir.rfind("/Binaries/");
    if (bin != std::string::npos) return dir.substr(0, bin) + "/Saved/Logs/Vein.log";
    return dir + "/../../Saved/Logs/Vein.log";
}

void OpenLog(bool fromEof) {
    if (g_logFd >= 0) { close(g_logFd); g_logFd = -1; }
    g_logFd = open(g_logPath.c_str(), O_RDONLY);
    if (g_logFd < 0) return;
    struct stat stt;
    if (fstat(g_logFd, &stt) == 0) {
        g_logInode = (uint64_t)stt.st_ino;
        g_logOffset = fromEof ? stt.st_size : 0;
    }
    g_logPartial.clear();
    PluginLog("events: log tail attached to %s (inode %llu, offset %lld)", g_logPath.c_str(),
              (unsigned long long)g_logInode, (long long)g_logOffset);
}

// A chat line seen in the log is a second, independent chat source. It carries the SteamID64, so it
// needs no game-thread lookup at all - which is why it also works before any vtable is hooked.
// Log-only join fallback. `LogNet: Login request:` carries the SteamID64 *and* the display name, so
// a join is reportable from the log alone - which is what keeps player-connected working if the
// PostLogin vtable sweep ever binds nothing. It is deliberately slow: the hook gets a head start,
// and only a player still unannounced after kLogJoinGraceMs is reported this way.
struct LogJoin {
    std::string name;
    uint64_t seenMs = 0;
};
Mutex g_logJoinLock;
std::map<std::string, LogJoin> g_logJoins;
const uint64_t kLogJoinGraceMs = 25000;
std::atomic<uint64_t> g_logJoinsEmitted{0};

// Runs on the housekeeping thread. Touches no UObject, so it needs no game thread.
void FlushLogJoins() {
    std::vector<Ident> due;
    {
        Guard g(g_logJoinLock);
        uint64_t now = NowMs();
        for (auto it = g_logJoins.begin(); it != g_logJoins.end();) {
            if (now - it->second.seenMs < kLogJoinGraceMs) { ++it; continue; }
            if (!AlreadyAnnounced(it->first)) {
                Ident id;
                id.gameId = it->first;
                id.platformName = it->second.name;
                id.characterName = ::state::CharacterName(it->first);
                id.name = id.characterName.empty() ? id.platformName : id.characterName;
                if (id.name.empty()) id.name = id.gameId;
                due.push_back(id);
            }
            it = g_logJoins.erase(it);
        }
    }
    for (auto& id : due) {
        PluginLog("events: player-connected %s from the server log (no hook reported it within %llums)",
                  id.gameId.c_str(), (unsigned long long)kLogJoinGraceMs);
        g_logJoinsEmitted++;
        EmitJoin(id);
    }
}

void NoteLogLine(const std::string& line, bool customJoin, bool customChat) {
    EventsParse::JoinLine j = EventsParse::ParseJoinLine(line);
    if (j.ok && !customJoin) {
        Guard g(g_logJoinLock);
        LogJoin& e = g_logJoins[j.gameId];
        if (e.seenMs == 0) e.seenMs = NowMs();
        if (!j.name.empty()) e.name = j.name;
        return;
    }
    EventsParse::ChatLine c = EventsParse::ParseChatLine(line);
    if (c.ok && !customChat) {
        if (!c.characterName.empty()) ::state::NoteCharacterName(c.gameId, c.characterName);
        Ident id;
        id.gameId = c.gameId;
        id.platformName = c.platformName;
        id.characterName = c.characterName.empty() ? ::state::CharacterName(c.gameId) : c.characterName;
        id.name = id.characterName.empty() ? id.platformName : id.characterName;
        g_chat.fired++;
        g_logChat++;
        EmitChat(id, c.msg, "global", "log");
        return;
    }
    EventsParse::CharacterSelectLine s = EventsParse::ParseCharacterSelectLine(line);
    if (s.ok && !s.characterName.empty()) {
        // The line names the persona, not the id; match it to a tracked connection.
        Guard g(g_connLock);
        for (auto& k : g_conns)
            if (k.id.valid() && k.id.platformName == s.platformName)
                ::state::NoteCharacterName(k.id.gameId, s.characterName);
    }
}

void PollLog() {
    if (g_logPath.empty()) return;
    struct stat stt;
    if (stat(g_logPath.c_str(), &stt) != 0) return;
    if (g_logFd < 0 || (uint64_t)stt.st_ino != g_logInode || stt.st_size < g_logOffset) {
        // Rotated or truncated. Mid-run that means "read the new file from the start"; inside the
        // first three minutes it is the server opening its own log after our constructor ran, and
        // replaying the whole boot log would flood the ring buffer.
        bool boot = g_logStartMs && NowMs() - g_logStartMs < kBootRotationGraceMs && g_log.emitted.load() == 0;
        OpenLog(boot);
        if (g_logFd < 0) return;
    }
    std::vector<char> buf(65536);
    size_t emitted = 0, read_total = 0;
    while (read_total < kMaxReadPerCycle) {
        ssize_t n = pread(g_logFd, buf.data(), buf.size(), g_logOffset);
        if (n <= 0) break;
        g_logOffset += n;
        read_total += (size_t)n;
        g_logPartial.append(buf.data(), (size_t)n);
        size_t start = 0, nl;
        while ((nl = g_logPartial.find('\n', start)) != std::string::npos) {
            std::string line = g_logPartial.substr(start, nl - start);
            start = nl + 1;
            while (!line.empty() && line.back() == '\r') line.pop_back();
            std::function<void(std::string)> sink;
            bool customJoin, customChat;
            {
                Guard lock(g_rawLogLock);
                sink = g_rawLogSink; customJoin = g_customLogJoin; customChat = g_customLogChat;
            }
            try {
                NoteLogLine(line, customJoin, customChat);
            } catch (...) {
            }
            if (sink) {
                try { sink(std::move(line)); } catch (...) { ++g_logDropped; }
                continue;
            }
            if (EventsParse::IsNoise(line)) continue;
            if (emitted >= kMaxLogPerCycle) { g_logDropped++; continue; }
            PluginState::Get().EmitEvent("log", "{\"msg\":" + JsonStr(EventsParse::RedactLogLine(line)) + "}");
            emitted++;
        }
        g_logPartial.erase(0, start);
        if (g_logPartial.size() > (1 << 20)) g_logPartial.clear();
        if ((size_t)n < buf.size()) break;
    }
    if (emitted) g_log.emitted += emitted;
}

// =================================================================================================
// init

std::atomic<bool> g_bootDone{false};

void Phase(const char* p) {
    if (DebugEnabled()) PluginLog("events: housekeep phase %s", p);
}

void RefreshCapabilities() {
    size_t chatVts = HookedFor(kChat);
    size_t deathVts = HookedFor(kDeath);
    size_t killVts = HookedFor(kKill);

    if (chatVts || g_logChat.load()) {
        std::string how = "ProcessEvent on " + std::to_string(chatVts) + " chat vtable(s)";
        std::string live = LiveFn(kChat);
        if (!live.empty()) how += " (firing on " + live + ")";
        how += "; LogVeinChat log line as a second source";
        if (g_chat.emitted.load() == 0) how += "; not yet observed firing";
        Ok("chatEvents", g_chat, how);
    } else if (g_log.hooked.load()) {
        Degrade("chatEvents", g_chat,
                "no live chat vtable hooked yet (it appears when a player connects); the LogVeinChat "
                "log line is the only source until then");
    } else {
        Degrade("chatEvents", g_chat, "neither a chat vtable nor the log tail is available");
    }

    if (deathVts || killVts) {
        std::string how = "ProcessEvent on " + std::to_string(deathVts) +
                          " player-character vtable(s) + a Health/Dead edge per cycle";
        std::string live = LiveFn(kDeath);
        if (!live.empty()) how += " (firing on " + live + ")";
        if (g_death.emitted.load() == 0) how += "; not yet observed firing";
        Ok("deathEvents", g_death, how);
    } else {
        Degrade("deathEvents", g_death,
                "no live player-character vtable to hook yet; the Health/Dead edge fallback needs a "
                "connected player. Retried every 2 s");
    }

    if (killVts) {
        std::string how = "ProcessEvent on " + std::to_string(killVts) +
                          " AI-character / health-component vtable(s)";
        std::string live = LiveFn(kKill);
        if (!live.empty()) how += " (firing on " + live + ")";
        if (g_kill.emitted.load() == 0) how += "; not yet observed firing";
        Ok("killEvents", g_kill, how);
    } else {
        Degrade("killEvents", g_kill,
                "no live AI character or health component to hook yet (AI streams in around a player); "
                "retried every 2 s");
    }
}

void GameThreadInit() {
    g_clsUObject = Reflect::StaticClass("UObject::StaticClass");
    g_clsPlayerState = Reflect::StaticClass("APlayerState::StaticClass");
    if (!g_clsPlayerState) g_clsPlayerState = Reflect::FindObjectByPath("/Script/Engine", "PlayerState");
    g_clsPlayerController = Reflect::StaticClass("APlayerController::StaticClass");
    if (!g_clsPlayerController) g_clsPlayerController = Reflect::FindObjectByPath("/Script/Engine", "PlayerController");
    g_clsPawn = Reflect::FindObjectByPath("/Script/Engine", "Pawn");
    // The character StaticClass() bodies are inline-emitted and absent from the depot .sym for
    // AVeinBaseCharacter / AVeinCharacter / AVeinPlayerCharacter, so the class objects are looked up
    // by path instead - which is what "a missing StaticClass means look the class up" is for.
    g_clsVeinBaseCharacter = FindByName("VeinBaseCharacter");
    g_clsVeinPlayerCharacter = FindByName("VeinPlayerCharacter");
    g_clsAiCharacters[0] = Reflect::StaticClass("AVeinZombieCharacter::StaticClass");
    if (!g_clsAiCharacters[0]) g_clsAiCharacters[0] = FindByName("VeinZombieCharacter");
    g_clsAiCharacters[1] = Reflect::StaticClass("AVeinAnimalCharacter::StaticClass");
    if (!g_clsAiCharacters[1]) g_clsAiCharacters[1] = FindByName("VeinAnimalCharacter");
    g_clsAiCharacters[2] = FindByName("VeinBaseDreamCharacter");
    g_clsAiCharacters[3] = FindByName("VeinZombieCharacterProxy");
    g_clsHealthComponent = Reflect::StaticClass("UHealthComponent::StaticClass");
    if (!g_clsHealthComponent) g_clsHealthComponent = FindByName("HealthComponent");
    // UDamageType::StaticClass() is inline-emitted and absent from the .sym, so the class object is
    // looked up by path. POST /debug/kill-nearest refuses to run rather than pass a null TSubclassOf.
    g_clsDamageType = Reflect::FindObjectByPath("/Script/Engine", "DamageType");
    if (!g_clsDamageType) g_clsDamageType = Reflect::FindObjectByPath("/Script/Vein", "VeinDamageType");
    PluginLog("events: class pointers UObject=%p PlayerState=%p PlayerController=%p VeinBaseCharacter=%p "
              "VeinPlayerCharacter=%p Zombie=%p Animal=%p HealthComponent=%p DamageType=%p",
              g_clsUObject, g_clsPlayerState, g_clsPlayerController, g_clsVeinBaseCharacter,
              g_clsVeinPlayerCharacter, g_clsAiCharacters[0], g_clsAiCharacters[1], g_clsHealthComponent,
              g_clsDamageType);

    g_processEvent = SymFn<FnProcessEvent>("UObject::ProcessEvent");
    g_steamIdToString = SymFn<FnNetIdToString>("FUniqueNetIdSteam::ToString");
    uint64_t ztv = DynSymAddr("_ZTV18FUniqueNetIdSteam");
    if (!ztv) ztv = DynSymAddr("_ZTV19FUniqueNetIdSteam");
    g_steamIdVptr = ztv ? (void*)(uintptr_t)(ztv + 2 * sizeof(void*)) : nullptr;

    // FNAME_Find, never FNAME_Add: a name that is not in the pool simply stays 0 and its candidate
    // is skipped. Adding names would pollute the game's pool for no gain.
    for (size_t i = 0; i < kCandidateCount; i++) {
        g_candidates[i].fname = Reflect::MakeName(g_candidates[i].name);
        if (g_candidates[i].fname.Comparison == 0) continue;
        g_fnNamesReady = true;
        if (g_candidates[i].kind == kChat) g_chatCandidates++;
        else if (g_candidates[i].kind == kDeath) g_deathCandidates++;
        else g_killCandidates++;
    }
    PluginLog("events: UFunction candidates present in the name pool - chat %zu, death %zu, kill %zu",
              g_chatCandidates, g_deathCandidates, g_killCandidates);

    SweepProcessEventTargets();
    RefreshCapabilities();
    g_bootDone = true;
}

}  // namespace

// =================================================================================================
// POST /debug/kill-nearest  (lane L2; routed by http.cpp through Actions::KillNearest)
//
// Kills the AI nearest to a player through the game's own damage pipeline, so entity-killed can be
// proven without a human swinging anything. The damage goes through
// UGameplayStatics::ApplyDamage(DamagedActor, BaseDamage, EventInstigator, DamageCauser,
// DamageTypeClass) *by reflection* - the UFunction is found on the UGameplayStatics CDO and called
// with a parameter block whose offsets come from FindPropertyByName, so the C++ ABI is never
// guessed. DamageTypeClass is the looked-up UDamageType UClass, never a null TSubclassOf (a null
// class is what makes UE's damage path dereference garbage).

namespace {

Actions::Result ErrJson(int status, const std::string& msg) {
    Actions::Result r;
    r.status = status;
    r.body = "{\"error\":" + JsonStr(msg) + "}";
    return r;
}

// ---- lane L2b: the AI damage pipeline, measured from the depot DWARF ---------------------------
//
// L2's first cut called UGameplayStatics::ApplyDamage and reported 1e6 "damage applied" while the
// zombie walked on. The depot's DWARF says why, and the answer is not a subtlety:
//
//   UHealthComponent::OnTakeAnyDamage(AActor*, float, UDamageType const*, AController*, AActor*)
//     @ 0x930cb80 opens with
//        test %r8,%r8 ; je out                  <- null DamageCauser -> return
//        ... IsA(DamageCauser, APainCausingVolume::StaticClass()) ; jne out
//   i.e. the *generic* damage delegate is only honoured for pain-causing volumes. Engine-generic
//   ApplyDamage therefore reaches the component and is dropped on the floor, while
//   UGameplayStatics::ApplyDamage still returns BaseDamage (AActor::TakeDamage returns the damage
//   it broadcast, handled or not) - which is exactly the "1e6 applied, still alive" that L6b saw.
//
// The live path for a weapon hit is the *point* damage delegate:
//   UHealthComponent::OnTakePointDamage(AActor*, float, AController*, FVector, UPrimitiveComponent*,
//                                       FName, FVector, UDamageType const*, AActor*)  @ 0x930c880
//     -> UHealthComponent::OnTakeDamage(...)          @ 0x930d0e0 (virtual; ULimbHealthComponent
//                                                      overrides it @ 0x9314280)
//     -> ShouldModifyHealth(FName BoneName, UDamageType const*)   (whitelist / blacklist gate)
//     -> ModifyHealth(double)  @ 0x930c2d0  -> SetHealth(double) @ 0x930c200
//     -> CheckDeath(AActor*, AController*, UDamageType const*, float, FVector, FVector, FName,
//                   FPointDamageEvent, FRadialDamageEvent)        @ 0x930a830
//        -> UVeinDamageType::GetDeathCause(), AVeinPlayerController::SetDeathReason,
//           AVeinBaseCharacter::SetDeathReason, and finally NetMulticast_OnDeath - the one event
//           lane L2 hooks.
//
// Two further facts from the same disassembly, both of which would have produced a silent no-kill:
//   * SetHealth (0x930c200) clamps to [0, MaxHealth], writes Health (+0xf8) and broadcasts
//     OnHealthUpdated (+0xc8) - it does NOT call CheckDeath. So "SetHealth(0)" or
//     "ModifyHealth(-1e6)" zeroes the health bar and leaves the AI walking. Neither is used here.
//   * A zombie's health component is a ULimbHealthComponent held in the UPROPERTY `Health`
//     (DWARF: `TObjectPtr<ULimbHealthComponent> Health` on AVeinZombieCharacter), not a
//     UHealthComponent, and it overrides both OnTakeDamage and ShouldModifyHealth.
//
// So: drive UGameplayStatics::ApplyPointDamage with the player's *pawn* as DamageCauser (the old
// code passed the controller for both, and AActor::TakeDamage attribution wants the pawn), and if
// the health does not move, call the component's own OnTakePointDamage UFUNCTION by reflection.
// Either way the endpoint reads Health before and after and only reports success when the AI is
// actually dead - the L6b lesson that a mutating action must verify its own effect.

void* HealthCompOf(void* actor) {
    if (!ValidObject(actor)) return nullptr;
    int32_t off = PropOffOf(actor, "Health");
    void* c = nullptr;
    if (off >= 0 && ReadAt(actor, off, c) && ValidObject(c) &&
        (!g_clsHealthComponent || Reflect::IsA(c, g_clsHealthComponent)))
        return c;
    if (!g_clsHealthComponent) return nullptr;
    std::vector<void*> comps;
    if (Reflect::GetObjectsOfClass(g_clsHealthComponent, comps, true))
        for (void* o : comps)
            if (ValidObject(o) && Reflect::ObjOuter(o) == actor) return o;
    return nullptr;
}

// Health of an AI, by the component's *declared* property types. Returns false when unreadable.
bool ReadHealth(void* comp, double& hp, double& maxHp) {
    if (!ValidObject(comp)) return false;
    if (!ReadNumericProp(comp, "Health", hp)) return false;
    if (!ReadNumericProp(comp, "MaxHealth", maxHp)) maxHp = 0;
    return true;
}

// ---- parameter-block writers. Every offset comes from FindPropertyByName; nothing is guessed. ---
bool SetPtrParam(void* fn, std::vector<uint8_t>& p, const char* name, void* v, std::string& err) {
    int32_t off = PropOff(fn, name);
    if (off < 0 || off + 8 > (int32_t)p.size()) {
        err = std::string("parameter ") + name + " not found by reflection";
        return false;
    }
    memcpy(p.data() + off, &v, sizeof v);
    return true;
}
// Writes a scalar by the property's declared type (float vs double differ all over UE 5.6).
bool SetScalarParam(void* fn, std::vector<uint8_t>& p, const char* name, double v, std::string& err) {
    void* prop = Reflect::FindProperty(fn, name);
    int32_t off = prop ? Reflect::PropertyOffset(prop) : -1;
    if (off < 0 || off + 4 > (int32_t)p.size()) {
        err = std::string("parameter ") + name + " not found by reflection";
        return false;
    }
    std::string type = Reflect::PropertyTypeName(prop);
    if (type == "DoubleProperty") {
        if (off + 8 > (int32_t)p.size()) { err = std::string("parameter ") + name + " overruns the block"; return false; }
        memcpy(p.data() + off, &v, sizeof v);
    } else {
        float f = (float)v;
        memcpy(p.data() + off, &f, sizeof f);
    }
    return true;
}
// An FVector is three doubles in UE 5.x; the struct property's offset is the whole vector's.
bool SetVectorParam(void* fn, std::vector<uint8_t>& p, const char* name, double x, double y, double z,
                    std::string& err) {
    int32_t off = PropOff(fn, name);
    if (off < 0 || off + 24 > (int32_t)p.size()) {
        err = std::string("parameter ") + name + " not found by reflection";
        return false;
    }
    double v[3] = {x, y, z};
    memcpy(p.data() + off, v, sizeof v);
    return true;
}

bool ParamBlockSize(void* fn, uint32_t& size) {
    return ReadAt(fn, (int32_t)Reflect::Lay().structPropertiesSize, size) && size > 0 && size <= 8192;
}

// The damage type instance/class to attribute the kill to. VEIN's CheckDeath casts the damage type
// to UVeinDamageType to read a UDeathCause off it, so a UVeinDamageType is preferred; a plain
// UDamageType still kills, it just carries no death-cause text. Never null: a null TSubclassOf is
// what makes UE's damage path dereference garbage.
void* DamageTypeClass() {
    static void* cls = nullptr;
    if (cls) return cls;
    cls = Reflect::FindObjectByPath("/Script/Vein", "VeinDamageType");
    if (!cls) cls = g_clsDamageType;
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Engine", "DamageType");
    return cls;
}

// Mechanism A - the engine's own point-damage entry point, by reflection.
// UGameplayStatics::ApplyPointDamage(AActor* DamagedActor, float BaseDamage, FVector HitFromDirection,
//   FHitResult HitInfo, AController* EventInstigator, AActor* DamageCauser, TSubclassOf<UDamageType>)
bool ApplyPointDamageByReflection(void* victim, double damage, void* instigatorCtrl, void* causerPawn,
                                  double dirX, double dirY, double dirZ, std::string& err) {
    void* statics = Reflect::FindObjectByPath("/Script/Engine", "GameplayStatics");
    void* cdo = statics ? Reflect::ClassDefaultObject(statics) : nullptr;
    void* fn = cdo ? Reflect::FindFunction(cdo, "ApplyPointDamage") : nullptr;
    if (!fn || !g_processEvent) {
        err = "UGameplayStatics::ApplyPointDamage is not reachable by reflection";
        return false;
    }
    void* dmgCls = DamageTypeClass();
    if (!dmgCls) {
        err = "no UDamageType class could be looked up; refusing to pass a null TSubclassOf into the damage pipeline";
        return false;
    }
    uint32_t size = 0;
    if (!ParamBlockSize(fn, size)) {
        err = "ApplyPointDamage has an implausible parameter block size";
        return false;
    }
    std::vector<uint8_t> p(size, 0);  // a zeroed FHitResult is a valid "no component, bone None" hit
    if (!SetPtrParam(fn, p, "DamagedActor", victim, err)) return false;
    if (!SetPtrParam(fn, p, "EventInstigator", instigatorCtrl, err)) return false;
    if (!SetPtrParam(fn, p, "DamageCauser", causerPawn, err)) return false;
    if (!SetPtrParam(fn, p, "DamageTypeClass", dmgCls, err)) return false;
    if (!SetScalarParam(fn, p, "BaseDamage", damage, err)) return false;
    if (!SetVectorParam(fn, p, "HitFromDirection", dirX, dirY, dirZ, err)) return false;
    g_processEvent(cdo, fn, p.data());
    return true;
}

// Mechanism B - the health component's own delegate handler, by reflection. Same arguments the
// engine would have delivered; used when the engine route leaves the health untouched.
bool OnTakePointDamageByReflection(void* comp, void* victim, double damage, void* instigatorCtrl,
                                   void* causerPawn, double hx, double hy, double hz, double dirX,
                                   double dirY, double dirZ, std::string& err) {
    void* fn = Reflect::FindFunction(comp, "OnTakePointDamage");
    if (!fn || !g_processEvent) {
        err = "UHealthComponent::OnTakePointDamage is not reachable by reflection";
        return false;
    }
    void* dmgCls = DamageTypeClass();
    void* dmgObj = dmgCls ? Reflect::ClassDefaultObject(dmgCls) : nullptr;  // the param is an instance
    uint32_t size = 0;
    if (!ParamBlockSize(fn, size)) {
        err = "OnTakePointDamage has an implausible parameter block size";
        return false;
    }
    std::vector<uint8_t> p(size, 0);  // BoneName stays FName(None), HitComponent stays null
    if (!SetPtrParam(fn, p, "DamagedActor", victim, err)) return false;
    if (!SetPtrParam(fn, p, "InstigatedBy", instigatorCtrl, err)) return false;
    if (!SetPtrParam(fn, p, "DamageCauser", causerPawn, err)) return false;
    if (dmgObj && !SetPtrParam(fn, p, "DamageType", dmgObj, err)) return false;
    if (!SetScalarParam(fn, p, "Damage", damage, err)) return false;
    if (!SetVectorParam(fn, p, "HitLocation", hx, hy, hz, err)) return false;
    if (!SetVectorParam(fn, p, "ShotFromDirection", dirX, dirY, dirZ, err)) return false;
    g_processEvent(comp, fn, p.data());
    return true;
}

// Finds the nearest AI to a point. Shared by /debug/kill-nearest and the read-only /debug/nearby.
struct AiNear {
    void* actor = nullptr;
    std::string cls;
    double dist = 0;
    double hp = 0, maxHp = 0;
    bool haveHp = false;
    std::string healthCls;
    bool healthHooked = false;
};

std::vector<AiNear> NearbyAi(double px, double py, double pz, double radius, void* skip, size_t limit) {
    std::vector<AiNear> out;
    for (void* cls : g_clsAiCharacters) {
        if (!cls) continue;
        std::vector<void*> objs;
        if (!Reflect::GetObjectsOfClass(cls, objs, true)) continue;
        for (void* o : objs) {
            if (!ValidObject(o) || o == skip) continue;
            bool seen = false;
            for (auto& a : out)
                if (a.actor == o) { seen = true; break; }
            if (seen) continue;
            double x = 0, y = 0, z = 0;
            if (!ActorLocation(o, x, y, z)) continue;
            double d = sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
            if (d > radius) continue;
            AiNear a;
            a.actor = o;
            a.cls = SafeClassName(o);
            a.dist = d;
            void* hc = HealthCompOf(o);
            if (hc) {
                a.healthCls = SafeClassName(hc);
                a.haveHp = ReadHealth(hc, a.hp, a.maxHp);
                void* vt = nullptr;
                if (ReadAt(hc, 0, vt)) {
                    Guard g(g_vtLock);
                    a.healthHooked = g_hookedVts.count(vt) > 0;
                }
            }
            out.push_back(a);
        }
    }
    std::sort(out.begin(), out.end(), [](const AiNear& a, const AiNear& b) { return a.dist < b.dist; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::string AiJson(const AiNear& a) {
    std::string o = "{\"class\":" + JsonStr(a.cls) + ",\"distance\":" + JsonNum(a.dist) +
                    ",\"healthComponent\":" + JsonStr(a.healthCls) +
                    ",\"healthComponentHooked\":" + (a.healthHooked ? "true" : "false");
    if (a.haveHp) o += ",\"health\":" + JsonNum(a.hp) + ",\"maxHealth\":" + JsonNum(a.maxHp);
    return o + "}";
}

struct DeferredBody {
    std::string literal;
    std::function<std::string()> render;
    DeferredBody() = default;
    DeferredBody(const char* text) : literal(text) {}
    DeferredBody(std::function<std::string()> fn) : render(std::move(fn)) {}
    std::string operator()() const { return render ? render() : literal; }
};

DeferredBody NearbyOnGameThread(const std::string& wantId, double radius, int& status) {
    status = 200;
    if (!g_bootDone) return "{\"error\":\"the event sources have not finished booting\"}";
    void* ctrl = nullptr;
    Ident who;
    {
        Guard g(g_connLock);
        for (auto& c : g_conns) {
            if (!c.announced || c.left || !c.id.valid()) continue;
            if (!wantId.empty() && c.id.gameId != wantId) continue;
            ctrl = c.controller;
            who = c.id;
            break;
        }
    }
    if (!ctrl) {
        status = 404;
        return "{\"error\":\"no such player online\"}";
    }
    void* st = PlayerStateOf(ctrl);
    void* pawn = nullptr;
    int32_t pawnOff = st ? PropOffOf(st, "PawnPrivate") : -1;
    if (pawnOff < 0 || !ReadAt(st, pawnOff, pawn) || !ValidObject(pawn)) {
        status = 409;
        return "{\"error\":\"the player has no pawn in the world\"}";
    }
    double px = 0, py = 0, pz = 0;
    if (!ActorLocation(pawn, px, py, pz)) {
        status = 409;
        return "{\"error\":\"the player's pawn has no readable location\"}";
    }
    std::vector<AiNear> ai = NearbyAi(px, py, pz, radius, pawn, 25);
    for (auto& row : ai) row.actor = nullptr;
    return DeferredBody{[id = who.gameId, pawnClass = SafeClassName(pawn), radius, ai = std::move(ai)] {
    std::string o = "{\"player\":" + JsonStr(id) + ",\"pawn\":" + JsonStr(pawnClass) +
                    ",\"radius\":" + JsonNum(radius) + ",\"count\":" + std::to_string(ai.size()) + ",\"ai\":[";
    for (size_t i = 0; i < ai.size(); i++) o += std::string(i ? "," : "") + AiJson(ai[i]);
    return o + "]}";
    }};
}

DeferredBody KillNearestOnGameThread(const std::string& wantId, double radius, int& status) {
    status = 200;
    if (!g_bootDone) return "{\"error\":\"the event sources have not finished booting\"}";

    // 1. the reference player: the instigator of the kill, so attribution is real.
    void* ctrl = nullptr;
    Ident who;
    {
        Guard g(g_connLock);
        for (auto& c : g_conns) {
            if (!c.announced || c.left || !c.id.valid()) continue;
            if (!wantId.empty() && c.id.gameId != wantId) continue;
            ctrl = c.controller;
            who = c.id;
            break;
        }
    }
    if (!ctrl) {
        status = 404;
        return "{\"error\":\"no such player online\"}";
    }
    void* st = PlayerStateOf(ctrl);
    void* pawn = nullptr;
    int32_t pawnOff = st ? PropOffOf(st, "PawnPrivate") : -1;
    if (pawnOff < 0 || !ReadAt(st, pawnOff, pawn) || !ValidObject(pawn)) {
        status = 409;
        return "{\"error\":\"the player has no pawn in the world; the kill needs a pawn as DamageCauser\"}";
    }
    double px = 0, py = 0, pz = 0;
    if (!ActorLocation(pawn, px, py, pz)) {
        status = 409;
        return "{\"error\":\"the player's pawn has no readable location\"}";
    }

    // 2. the nearest AI, and its health component.
    std::vector<AiNear> near = NearbyAi(px, py, pz, radius, pawn, 1);
    if (near.empty()) {
        status = 404;
        return DeferredBody{[radius] { return "{\"error\":\"no AI character within the radius\",\"radius\":" + JsonNum(radius) + "}"; }};
    }
    AiNear target = near[0];
    void* victim = target.actor;
    void* comp = HealthCompOf(victim);
    if (!comp) {
        status = 409;
        return DeferredBody{[cls = target.cls] { return "{\"error\":\"the AI has no readable health component\",\"entityClass\":" + JsonStr(cls) + "}"; }};
    }
    // Hook this component's vtable now, so the NetMulticast_OnDeath it is about to send is seen even
    // if the periodic sweep has not reached this class yet.
    HookObjectProcessEvent("UHealthComponent::ProcessEvent (kill target)", comp);

    double hp0 = 0, maxHp0 = 0;
    bool haveHp = ReadHealth(comp, hp0, maxHp0);
    double damage = haveHp && maxHp0 > 0 ? maxHp0 * 10.0 + 1000.0 : 100000.0;

    // A unit direction from the player to the AI: the shot direction the engine would have had.
    double vx = 0, vy = 0, vz = 0;
    ActorLocation(victim, vx, vy, vz);
    double dx = vx - px, dy = vy - py, dz = vz - pz;
    double len = sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1.0) { dx = 1; dy = 0; dz = 0; len = 1; }
    dx /= len; dy /= len; dz /= len;

    // 3. mechanism A, then B, each verified against the health the component actually reports.
    std::string mech, err, notes;
    double hp1 = hp0;
    bool dead = false;
    auto stillAlive = [&]() {
        double h = 0, m = 0;
        if (!ReadHealth(comp, h, m)) return true;  // unreadable: assume not proven dead
        hp1 = h;
        return h > 0.0;
    };

    g_debugKillVictim.store(victim);
    if (ApplyPointDamageByReflection(victim, damage, ctrl, pawn, dx, dy, dz, err)) {
        mech = "UGameplayStatics::ApplyPointDamage";
        dead = !stillAlive();
    } else {
        notes += "ApplyPointDamage: " + err + "; ";
    }
    if (!dead) {
        std::string err2;
        if (OnTakePointDamageByReflection(comp, victim, damage, ctrl, pawn, vx, vy, vz, dx, dy, dz, err2)) {
            mech = mech.empty() ? "UHealthComponent::OnTakePointDamage" : mech + " + UHealthComponent::OnTakePointDamage";
            dead = !stillAlive();
        } else {
            notes += "OnTakePointDamage: " + err2 + "; ";
        }
    }
    if (!ValidObject(victim)) dead = true;  // the actor was destroyed outright

    PluginLog("events: /debug/kill-nearest %s %s for %s via %s (health %.1f -> %.1f)", dead ? "killed" : "FAILED to kill",
              target.cls.c_str(), who.gameId.c_str(), mech.empty() ? "no mechanism" : mech.c_str(), hp0, hp1);

    if (!dead) status = 500;
    target.actor = nullptr;
    return DeferredBody{[target, dead, mech, who, damage, haveHp, hp0, hp1, notes] {
    std::string body = std::string("{\"success\":") + (dead ? "true" : "false") + ",\"entityClass\":" +
                       JsonStr(target.cls) + ",\"healthComponent\":" + JsonStr(target.healthCls) +
                       ",\"distance\":" + JsonNum(target.dist) + ",\"mechanism\":" + JsonStr(mech) +
                       ",\"instigator\":" + JsonStr(who.gameId) + ",\"damage\":" + JsonNum(damage);
    if (haveHp) body += ",\"healthBefore\":" + JsonNum(hp0) + ",\"healthAfter\":" + JsonNum(hp1);
    if (!notes.empty()) body += ",\"notes\":" + JsonStr(notes);
    body += ",\"detail\":" +
            JsonStr(dead ? "the AI is dead; entity-killed is emitted by the NetMulticast_OnDeath hook, not by this "
                           "endpoint"
                         : "the damage did not kill the AI - this endpoint reports what the health component "
                           "actually says, never a bare success");
    return body + "}";
    }};
}

}  // namespace

// lane L2b: GET /debug/nearby?gameId=&radius= - read-only. Lists the AI around a player with its
// class, distance, health component and whether that component's ProcessEvent vtable is hooked, so
// the kill path can be inspected before anything is mutated.
Actions::Result Events::Nearby(const std::string& gameId, double radius) {
    if (!Reflect::Validated()) return ErrJson(503, "the reflection layout has not been validated yet");
    if (!(radius > 0) || radius > 1.0e7) return ErrJson(400, "radius must be a positive number of centimetres");
    auto out = std::make_shared<DeferredBody>();
    auto status = std::make_shared<int>(200);
    if (!GameThread::Run([gameId, radius, status, out] { *out = NearbyOnGameThread(gameId, radius, *status); }, 8000))
        return ErrJson(503, "the game thread did not answer in time");
    Actions::Result res;
    res.status = *status;
    res.body = (*out)();
    return res;
}

Actions::Result Events::KillNearest(const JsonValue& body) {
    if (!Reflect::Validated()) return ErrJson(503, "the reflection layout has not been validated yet");
    std::string gameId;
    const JsonValue* g = body.get("gameId");
    if (g && g->isStr()) gameId = g->str;
    double radius = 5000.0;
    const JsonValue* r = body.get("radius");
    if (r && r->isNum()) radius = r->num;
    if (!(radius > 0) || radius > 1.0e7) return ErrJson(400, "radius must be a positive number of centimetres");

    auto out = std::make_shared<DeferredBody>();
    auto status = std::make_shared<int>(200);
    if (!GameThread::Run([gameId, radius, status, out] { *out = KillNearestOnGameThread(gameId, radius, *status); }, 8000))
        return ErrJson(503, "the game thread did not answer in time");
    Actions::Result res;
    res.status = *status;
    res.body = (*out)();
    return res;
}

// =================================================================================================

void Events::SetRawLogSink(std::function<void(std::string)> sink, bool customJoin, bool customChat) {
    Guard lock(g_rawLogLock);
    g_rawLogSink = std::move(sink);
    g_customLogJoin = customJoin;
    g_customLogChat = customChat;
}

void Events::Init() {
    auto& st = PluginState::Get();
    for (const char* cap : {"logEvents", "joinLeaveEvents", "connectEvents", "chatEvents", "deathEvents",
                            "killEvents", "playerConnected", "playerDisconnected", "chatMessage",
                            "playerDeath", "entityKilled", "log"})
        st.SetCapability(cap, "degraded", "starting");

    // 1. lifecycle hooks. These are static vtables, so they are safe before the world exists.
    size_t slot = SIZE_MAX;
    std::string tables;
    g_origPostLoginVein = SymFn<FnPostLogin>("AVeinGameModeBase::PostLogin");
    size_t nPostVein = SwapEverywhere("AVeinGameModeBase::PostLogin", (void*)&DetourPostLoginVein, slot, tables);
    g_origPostLoginBase = SymFn<FnPostLogin>("AGameModeBase::PostLogin");
    size_t nPostBase = SwapEverywhere("AGameModeBase::PostLogin", (void*)&DetourPostLoginBase, slot, tables);
    size_t nPost = nPostVein + nPostBase;
    g_join.hooked = nPost > 0;
    PluginLog("events: PostLogin hooked AVeinGameModeBase x%zu, AGameModeBase x%zu", nPostVein, nPostBase);

    g_origLogoutVein = SymFn<FnLogout>("AVeinGameModeBase::Logout");
    size_t nOut = SwapEverywhere("AVeinGameModeBase::Logout", (void*)&DetourLogoutVein, slot, tables);
    g_origLogoutBase = SymFn<FnLogout>("AGameModeBase::Logout");
    nOut += SwapEverywhere("AGameModeBase::Logout", (void*)&DetourLogoutBase, slot, tables);
    g_origLogoutGm = SymFn<FnLogout>("AGameMode::Logout");
    nOut += SwapEverywhere("AGameMode::Logout", (void*)&DetourLogoutGm, slot, tables);
    g_origNetCleanup = SymFn<FnNetCleanup>("APlayerController::OnNetCleanup");
    size_t nNet = SwapEverywhere("APlayerController::OnNetCleanup", (void*)&DetourNetCleanup, slot, tables);
    g_origDestroyedPc = SymFn<FnDestroyed>("APlayerController::Destroyed");
    size_t nDest = SwapEverywhere("APlayerController::Destroyed", (void*)&DetourDestroyedPc, slot, tables);
    g_origDestroyedCtrl = SymFn<FnDestroyed>("AController::Destroyed");
    nDest += SwapEverywhere("AController::Destroyed", (void*)&DetourDestroyedCtrl, slot, tables);
    g_leave.hooked = (nOut + nNet + nDest) > 0;
    PluginLog("events: disconnect hooks Logout x%zu OnNetCleanup x%zu Destroyed x%zu", nOut, nNet, nDest);

    if (nPost && g_leave.hooked.load()) {
        std::string how = "PostLogin x" + std::to_string(nPost) + ", OnNetCleanup x" + std::to_string(nNet) +
                          ", Logout x" + std::to_string(nOut) + ", Destroyed x" + std::to_string(nDest) +
                          ", plus a player-state reaper";
        Ok("joinLeaveEvents", g_join, how);
        PluginState::Get().SetCapability("connectEvents", "ok", "");
        Note(g_leave, how);
    } else if (!nPost) {
        Degrade("joinLeaveEvents", g_join,
                "PostLogin is in no exported vtable; the live game mode's own vtable is retried every 2 s");
    } else {
        Degrade("joinLeaveEvents", g_join, "no disconnect hook could be installed; the reaper is the only source");
    }

    ::state::BansLoad();

    // 2. log tail, from EOF so a restart does not replay the whole file.
    g_logPath = DefaultLogPath();
    g_logStartMs = NowMs();
    OpenLog(true);
    if (g_logFd < 0) {
        Degrade("logEvents", g_log, "cannot open " + g_logPath);
    } else {
        g_log.hooked = true;
        Ok("logEvents", g_log, "tailing " + g_logPath + " from EOF, rotation-aware, redacted");
    }

    // 3. everything that needs live UObjects runs on the game thread, once it ticks.
    PluginLog("events: init done (UObject work deferred to the game thread)");
}

namespace {

// LANE L9: housekeeping cadence. Every one of these used to be "every 2 s, unconditionally".
const uint64_t kSweepIntervalMs = 15000;   // safety net; a join/leave sweeps immediately
const uint64_t kHealthIntervalMs = 5000;   // death fallback, only while somebody is connected
const uint64_t kReapIntervalMs = 5000;
const uint64_t kSweepBudgetUs = 1000;      // one game-thread entry never sweeps for longer

size_t PendingJoins() {
    Guard g(g_connLock);
    size_t n = 0;
    for (auto& c : g_conns)
        if (!c.announced && !c.left) n++;
    return n;
}

size_t AnnouncedConnections() {
    Guard g(g_connLock);
    size_t n = 0;
    for (auto& c : g_conns)
        if (c.announced && !c.left) n++;
    return n;
}

// The connector-facing capability names (the ones L1's stub registered, and the ones the sidecar
// reads) mirror the internal ones, detail included - a degrade has to carry its reason on both
// names or /health would show a bare "degraded" with no explanation.
void MirrorCapabilities() {
    auto& st = PluginState::Get();
    struct { const char* from; const char* to; SourceStat* stat; } kMirror[] = {
        {"joinLeaveEvents", "playerConnected", &g_join}, {"joinLeaveEvents", "playerDisconnected", &g_leave},
        {"chatEvents", "chatMessage", &g_chat},          {"deathEvents", "playerDeath", &g_death},
        {"killEvents", "entityKilled", &g_kill},         {"logEvents", "log", &g_log},
    };
    for (auto& m : kMirror) {
        std::string status = st.Capability(m.from);
        st.SetCapability(m.to, status, status == "ok" ? std::string() : NoteOf(*m.stat));
    }
}

}  // namespace

void Events::Housekeep() {
    std::function<void()> report;
    { Guard guard(g_debugReportLock); report.swap(g_debugReport); }
    if (report) report();
    try {
        PollLog();
        FlushLogJoins();
    } catch (...) {
    }
    if (!Reflect::Validated()) return;  // never touch UObjects before the layout is confirmed

    // LANE L9 - the game-thread policy (plugin/docs/gamethread-policy.md). Housekeeping used to
    // enter the game thread unconditionally every 2 s and run every phase. Now each phase decides
    // for itself, and when no phase wants to run we do not enter the game thread at all: an idle
    // server with nobody online costs zero game-thread entries from this path.
    const uint64_t now = NowMs();
    const bool dirty = g_worldDirty.exchange(false);
    if (dirty) g_sweepPending = true;
    static uint64_t lastSweep = 0, lastHealth = 0, lastReap = 0;
    const bool wantBoot = !g_bootDone;
    // A sweep only finds work when a class vtable is new, which happens on a join or on level
    // streaming - hence the dirty edge plus a 15 s safety cadence, instead of every 2 s.
    const bool wantSweep = wantBoot || dirty || g_sweepPending.load() || now - lastSweep >= kSweepIntervalMs;
    // The live game mode retry is a GetObjectsOfClass walk of its own. It was the single most
    // expensive thing left on the game thread once the sweep was rate-limited (1.2 ms every 2 s, on
    // a rig where it never binds because the exported-vtable sweep already did the job), so it gets
    // the same cadence as the sweep: immediately on a join, else every 15 s.
    static uint64_t lastGameMode = 0;
    const bool wantGameMode = !g_liveModeHooked.load() &&
                              (wantBoot || dirty || now - lastGameMode >= kSweepIntervalMs);
    if (wantGameMode) lastGameMode = now;
    const bool wantJoins = PendingJoins() > 0;
    const bool wantReap = AnnouncedConnections() > 0 && now - lastReap >= kReapIntervalMs;
    // The health-edge poll is the *fallback* death detector, and it only means anything while
    // somebody is connected.
    const bool wantHealth = AnnouncedConnections() > 0 && now - lastHealth >= kHealthIntervalMs;
    if (!(wantBoot || wantSweep || wantGameMode || wantJoins || wantReap || wantHealth)) {
        Phase("idle");
        if (g_bootDone) { try { RefreshCapabilities(); } catch (...) {} }
        MirrorCapabilities();
        return;
    }
    if (wantSweep) lastSweep = now;  // the pending flag, not this stamp, carries a partial pass on
    if (wantHealth) lastHealth = now;
    if (wantReap) lastReap = now;
    GameThread::Run(
        [wantBoot, wantSweep, wantGameMode, wantJoins, wantReap, wantHealth] {
            try {
                Perf::Scope total("housekeep");
                if (wantBoot) { Phase("boot"); Perf::Scope sc("housekeep.boot"); GameThreadInit(); }
                if (wantSweep) {
                    Phase("sweep");
                    Perf::Scope sc("housekeep.sweep");
                    SweepProcessEventTargets(kSweepBudgetUs);
                }
                if (wantGameMode) { Phase("gamemode"); Perf::Scope sc("housekeep.gamemode"); HookLiveGameMode(); }
                if (wantJoins) { Phase("joins"); Perf::Scope sc("housekeep.joins"); ResolvePendingJoins(); }
                if (wantReap) { Phase("reap"); Perf::Scope sc("housekeep.reap"); ReapGoneConnections(); }
                if (wantHealth) { Phase("health"); Perf::Scope sc("housekeep.health"); PollHealthEdges(); }
                Phase("idle");
            } catch (...) {
                PluginLog("events: housekeeping job threw");
            }
        },
        5000);
    if (g_bootDone) {
        try {
            RefreshCapabilities();
        } catch (...) {
        }
    }
    MirrorCapabilities();
}

bool Events::BanEnforcementLive() {
    // VEIN's login refusal path is lane L3's ban work; L2 only reports what it can prove, and the
    // PostLogin-driven kick below is a kick, not a refusal.
    return false;
}

std::string Events::DiagnosticsJson() {
    auto one = [](const char* name, SourceStat& s, bool hooked) {
        return "{\"source\":" + JsonStr(name) + ",\"hooked\":" + (hooked ? "true" : "false") +
               ",\"fired\":" + std::to_string(s.fired.load()) + ",\"emitted\":" + std::to_string(s.emitted.load()) +
               ",\"note\":" + JsonStr(NoteOf(s)) + "}";
    };
    size_t conns;
    {
        Guard g(g_connLock);
        conns = g_conns.size();
    }
    std::string o = "{\"implemented\":true,\"owner\":\"lane L2\",\"sources\":[";
    o += one("player-connected", g_join, g_join.hooked.load()) + ",";
    o += one("player-disconnected", g_leave, g_leave.hooked.load()) + ",";
    o += one("chat-message", g_chat, HookedFor(kChat) > 0 || g_log.hooked.load()) + ",";
    o += one("player-death", g_death, HookedFor(kDeath) > 0) + ",";
    o += one("entity-killed", g_kill, HookedFor(kKill) > 0) + ",";
    o += one("log", g_log, g_log.hooked.load());
    o += "],\"processEventVTables\":" + std::to_string(g_peCount.load());
    o += ",\"trackedConnections\":" + std::to_string(conns);
    o += ",\"nonPlayerDeathsIgnored\":" + std::to_string(g_deathsIgnored.load());
    o += ",\"aiKillsWithoutPlayer\":" + std::to_string(g_killsUnattributed.load());
    o += ",\"identity\":{\"steamIdVTable\":" + std::string(g_steamIdVptr ? "true" : "false") +
         ",\"steamIdToString\":" + std::string(g_steamIdToString ? "true" : "false") + "}";
    o += ",\"logPath\":" + JsonStr(g_logPath) + ",\"logDropped\":" + std::to_string(g_logDropped.load()) +
         ",\"logChatLines\":" + std::to_string(g_logChat.load()) +
         ",\"logJoinsEmitted\":" + std::to_string(g_logJoinsEmitted.load());
    o += ",\"functionCandidates\":[";
    {
        bool first = true;
        for (size_t i = 0; i < kCandidateCount; i++) {
            if (g_candidates[i].fname.Comparison == 0 && g_candidates[i].seen.load() == 0) continue;
            o += std::string(first ? "" : ",") + "{\"name\":" + JsonStr(g_candidates[i].name) + ",\"kind\":" +
                 JsonStr(g_candidates[i].kind == kChat ? "chat" : (g_candidates[i].kind == kDeath ? "death" : "kill")) +
                 ",\"inNamePool\":true,\"seen\":" + std::to_string(g_candidates[i].seen.load()) + "}";
            first = false;
        }
    }
    o += "],\"liveFunctions\":{\"chat\":" + JsonStr(LiveFn(kChat)) + ",\"death\":" + JsonStr(LiveFn(kDeath)) +
         ",\"kill\":" + JsonStr(LiveFn(kKill)) + "}";
    o += ",\"classesHooked\":[";
    for (size_t i = 0; i < kTargetCount; i++)
        o += std::string(i ? "," : "") + "{\"class\":" + JsonStr(g_targets[i].cls) +
             ",\"resolved\":" + (g_targets[i].resolved ? "true" : "false") +
             ",\"vtables\":" + std::to_string(g_targets[i].hooked) +
             ",\"liveObjects\":" + std::to_string(g_targets[i].objects) + "}";
    o += "],\"banEnforcement\":{\"live\":false,\"detail\":\"lane L3 owns VEIN's login refusal path; L2 "
         "kicks a banned player it sees join\"}}";
    return o;
}
