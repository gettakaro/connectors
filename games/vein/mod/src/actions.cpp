// Lane L3: the 15 action capabilities behind the plugin's HTTP surface (VEIN, UE 5.6.1).
//
// Every handler that touches a UObject runs its work inside GameThread::Run(); HTTP threads only
// build JSON out of what the job produced. Nothing here hard-codes a game struct offset: every
// UPROPERTY is looked up through UStruct::FindPropertyByName at runtime and cached per class, and
// every function address comes from resolve.cpp. A missing symbol or property degrades one
// capability with a reason; it never throws out of a handler and never takes the server down.
//
// VEIN's own admin panel (UAdminComponent) is a complete server-authoritative moderation API -
// give, kick, ban, unban, teleport, exec, server message, save - so almost every action is the
// game's own code path rather than a reimplementation. See docs/actions-design.md for the symbol
// evidence behind each one and for what is still UNVERIFIED (nothing here has run in the game
// process yet; M0 was blocked when this was written).
#include "actions.h"

#include "actions_util.h"

#include "events.h"
#include "gamethread.h"
#include "perf.h"
#include "reflect.h"
#include "resolve.h"
#include "state.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>

using UE::FName;
using UE::FString;
using UE::TArray;

namespace {

std::atomic<size_t> g_pendingBanJobs{0};
struct BanJobLifetime {
    BanJobLifetime() { ++g_pendingBanJobs; }
    ~BanJobLifetime() { --g_pendingBanJobs; }
};

// ---------------------------------------------------------------------------------------------
// resolved game functions (all SysV direct calls; every one may be null)

template <typename T>
T Fn(const char* sym) {
    return (T)(uintptr_t)Resolve::Addr(sym);
}

// A 24-byte POD vector, ABI-identical to UE::Math::TVector<double> and FVector_NetQuantize: both
// are passed BY VALUE on the stack (MEMORY class), which is what a by-value struct parameter of
// this size compiles to here too.
struct FVec3 {
    double x = 0, y = 0, z = 0;
};

// Non-trivial class parameters (FString, FText) are passed by invisible reference under the
// Itanium ABI and destroyed by the *caller*, so every one of these takes a pointer to a buffer we
// own and free ourselves after the call returns.
using FnGetFString = void (*)(FString* ret, const void* self);        // FString-returning getter
using FnTextFromString = void (*)(void* retFText, const FString* s);  // FText::FromString
using FnTextDisplayString = const FString* (*)(const void* ftext);
using FnKickBan = void (*)(void* session, void* pc, const void* ftext);
// LANE L3e: APlayerController::ClientReturnToMainMenuWithTextReason(FText const&) - the kick
// fallback; a (this, FText const&) call, one argument short of the session ones.
using FnPcText = void (*)(void* pc, const void* ftext);
using FnProcessEvent = void (*)(void* obj, void* func, void* params);
using FnEngineExec = bool (*)(void* engine, void* world, const char16_t* cmd, void* device);
using FnMalloc = void* (*)(size_t, uint32_t);
using FnFree = void (*)(void*);
using FnTeleportTo = bool (*)(void* actor, const double* loc, const double* rot, bool isTest, bool noCheck);

// LANE L3e - the ungated entry points, read out of the admin RPCs' own disassembly.
// AActor::SetActorLocation(FVector const&, bool bSweep, FHitResult*, ETeleportType)
using FnSetActorLocation = bool (*)(void* actor, const double* loc, bool sweep, void* hit, int teleportType);
// UAdminComponent::MovePlayer(APlayerState*, FVector) - what Server_MovePlayer tail-jumps to once
// the admin check passes. It re-reads IsAdmin only to log and proceeds either way, so it is the
// game's own full teleport (client RPC included) without the gate.
using FnMovePlayer = void (*)(void* self, void* playerState, FVec3 dest);
// FVirtualItemInstance::FromItem(TSubclassOf<UItem>) - 168 bytes, so it returns through the hidden
// sret pointer in RDI.
using FnFromItem = void (*)(void* retInstance, void* const* itemClassRef);
using FnSetStack = void (*)(void* instance, int stack);
using FnItemDtor = void (*)(void* instance);
// UBaseInventoryComponent::AddItem(FVirtualItemInstance&, bool bNoStackCombine, bool bNoWeightChecks,
// uint8 Slot). Server_GiveItem calls it as (inst, false, true, 0).
using FnAddItem = uint8_t (*)(void* inv, void* instance, bool noStackCombine, bool noWeightChecks, uint8_t slot);
// AVeinPlayerController::Client_Kicked() and AActor::Destroy(bool, bool) - the two halves of what
// Server_KickPlayer does after its gate.
using FnClientKicked = void (*)(void* pc);
using FnDestroy = bool (*)(void* actor, bool netForce, bool shouldModifyLevel);

// UAdminComponent server RPCs, called through their *_Implementation address: the server already
// has authority, so the replicated stub would only route the call straight back to it.
// LANE L3e, PROVEN FROM THE DISASSEMBLY 2026-09-17: `TSubclassOf<UItem>` is a non-trivial class
// type, so under the Itanium ABI it is passed **by invisible reference** - the callee gets a
// POINTER TO the UClass pointer, not the UClass pointer. `FVirtualItemInstance::FromItem` opens
// with `mov (%rsi),%r15` and then runs the inlined `FStructBaseChain::IsChildOf` on `%r15`
// (`+0x30` StructBaseChainArray, `+0x38` NumStructBasesInChainMinusOne), which only type-checks if
// `%r15` is the UClass - i.e. `%rsi` is `UClass**`.
//
// Lane L3 passed the UClass* itself, so the game dereferenced it, got the class's vtable pointer,
// failed the IsChildOf check and returned an empty instance. **That, and not only the admin gate,
// is why `giveItem` never put anything in an inventory** - it is also why the admin RPC did nothing
// for a genuinely-online admin (L6b cell 5, re-confirmed 2026-09-17 with Tester admin and online).
using FnAdminGiveItem = void (*)(void* self, void* playerState, void* const* itemClassRef, int count);
using FnAdminPlayerState = void (*)(void* self, void* playerState);                       // Kick / Heal
using FnAdminBan = void (*)(void* self, void* playerState, const FString* reason);
using FnAdminString = void (*)(void* self, const FString* s);                             // Unban/Exec/Message
using FnAdminMovePlayer = void (*)(void* self, void* playerState, FVec3 dest);
using FnAdminTeleportTo = void (*)(void* self, void* playerState, FName target);
using FnAdminVoid = void (*)(void* self);

// AVeinGameStateBase. BanID/UnbanID take FString *by value* -> invisible reference, caller destroys.
using FnBanId = void (*)(void* gs, FString* id, FString* reason);
using FnUnbanId = void (*)(void* gs, FString* id);
using FnVoidSelf = void (*)(void* self);

// The replicated multicast entry (NOT _Implementation): calling this on the server is what makes
// the message reach every client.
using FnSendChat = void (*)(void* gs, void* senderPlayerState, const FString* msg, int segment, void* chatCommandClass,
                            FVec3 worldLocation);
// AVeinGameStateBase::NetMulticast_BroadcastServerMessage(FString const&) - the same multicast
// mechanism, but with NO sender parameter, so nothing can be null-dereferenced (lane L3d).
using FnGsString = void (*)(void* gs, const FString* msg);
using FnClientNotify = void (*)(void* pc, const void* ftext, int notificationType, float duration);

// ---------------------------------------------------------------------------------------------
// small helpers (game thread only unless stated)

void* ReadPtrAt(void* base, int32_t off) {
    if (!base || off < 0) return nullptr;
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 8)) return nullptr;
    return *(void* const*)p;
}

struct PropKey {
    void* cls;
    std::string name;
    bool operator<(const PropKey& o) const { return cls != o.cls ? cls < o.cls : name < o.name; }
};
Mutex g_propLock;
std::map<PropKey, int32_t> g_propCache;

// Offset of a UPROPERTY on `cls` (walking up the class chain). -1 when absent.
int32_t OffOf(void* cls, const char* name) {
    if (!cls) return -1;
    PropKey k{cls, name};
    {
        Guard g(g_propLock);
        auto it = g_propCache.find(k);
        if (it != g_propCache.end()) return it->second;
    }
    int32_t off = -1;
    for (void* c = cls; c; c = Reflect::SuperStruct(c)) {
        void* p = Reflect::FindProperty(c, name);
        if (p) {
            int32_t o = Reflect::PropertyOffset(p);
            if (o >= 0 && o < 0x100000) off = o;
            break;
        }
    }
    Guard g(g_propLock);
    g_propCache[k] = off;
    return off;
}
int32_t Off(void* obj, const char* name) { return OffOf(Reflect::ObjClass(obj), name); }

// First of `names` that exists on the object. -1 when none do.
int32_t OffAny(void* obj, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        int32_t o = Off(obj, n);
        if (o >= 0) return o;
    }
    return -1;
}

std::string ReadFStringAt(void* base, int32_t off) {
    if (!base || off < 0) return "";
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 16)) return "";
    FString s;
    memcpy(&s, p, sizeof s);
    if (s.Num <= 0 || s.Num > 1 << 16 || !MemReadable(s.Data, (size_t)s.Num * 2)) return "";
    return Reflect::Utf16To8(s.Data, s.Num);
}

std::string TextToString(void* base, int32_t off) {
    if (!base || off < 0) return "";
    auto disp = Fn<FnTextDisplayString>("FTextInspector::GetDisplayString");
    const void* p = (const char*)base + off;
    if (!disp || !MemReadable(p, 16)) return "";
    const FString* s = disp(p);
    if (!s || !MemReadable(s, 16) || s->Num <= 0 || s->Num > 1 << 16) return "";
    if (!MemReadable(s->Data, (size_t)s->Num * 2)) return "";
    return Reflect::Utf16To8(s->Data, s->Num);
}

// An FString whose buffer comes from the game's allocator, so the game may copy or keep it. The
// caller frees it (Itanium ABI: the caller destroys by-value class arguments).
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

// An FText built from a string. 16 bytes (a TSharedRef); FText::FromString fills it.
struct GameFText {
    char bytes[16];
    bool ok = false;
    explicit GameFText(const std::string& s) {
        memset(bytes, 0, sizeof bytes);
        auto fromString = Fn<FnTextFromString>("FText::FromString");
        GameFString tmp(s);
        if (!fromString || !tmp.ok) return;
        fromString(bytes, &tmp.fs);
        ok = true;
    }
};

// The pure helpers live in actions_util.cpp so that the unit tests exercise the shipped code.
using ActionsUtil::HumaniseCode;
using ActionsUtil::IsGeneratedArtefact;
using ActionsUtil::LooksLikeSteamId64;
using ActionsUtil::Lower;
using ActionsUtil::NormalizeGameId;
using ActionsUtil::BroadcastCaps;
using ActionsUtil::BroadcastPath;
using ActionsUtil::BroadcastPathSymbol;
using ActionsUtil::ChooseBroadcastPath;
using ActionsUtil::RenderMessage;
using ActionsUtil::Rest;
using ActionsUtil::Words;

std::string ErrJson(const std::string& msg) { return "{\"error\":" + JsonStr(msg) + "}"; }
Actions::Result Fail(int status, const std::string& msg) { return {status, ErrJson(msg)}; }

struct JobOut {
    int status = 500;
    std::string body = "{\"error\":\"internal plugin error\"}";
    std::function<std::string()> render;
    JobOut() = default;
    JobOut(int code, std::string text) : status(code), body(std::move(text)) {}
    JobOut(int code, std::function<std::string()> serialize) : status(code), render(std::move(serialize)) {}
    static JobOut Error(int code, std::string message) {
        return {code, [message = std::move(message)] { return ErrJson(message); }};
    }
};

// Runs `fn` on the game thread and WAITS for it; 503 when the pump is unavailable and 504 when the
// job did not finish inside `timeoutMs`.
//
// LANE L3d: the ack must follow the effect, never precede it. `ran` is set by the job itself, as
// the last thing it does, so a job that was queued, started and then never completed (the game
// thread died mid-call, the process is restarting) cannot be reported as a success. Every mutating
// handler - /message, /teleport, /give, /kick, /ban, /unban - goes through here, so they all share
// this guarantee.
Actions::Result OnGameThread(const char* what, std::function<JobOut()> fn, uint32_t timeoutMs = 5000) {
    auto out = std::make_shared<JobOut>();
    // All callers pass a string literal. Perf retains this pointer as a metric key.
    bool ok = GameThread::Run(
        [out, fn = std::move(fn), what] {
            Perf::Scope sc(what);
            try {
                *out = fn();
            } catch (const std::exception& e) {
                *out = JobOut::Error(500, std::string("plugin error: ") + e.what());
            } catch (...) {
                *out = JobOut::Error(500, "plugin error");
            }
        },
        timeoutMs);
    if (!ok) {
        PluginLog("actions: %s did not run (game thread unavailable)", what);
        return Fail(503, "game thread unavailable");
    }
    return {out->status, out->render ? out->render() : out->body};
}

void SetCap(const char* name, const char* status, const std::string& detail = "") {
    PluginState::Get().SetCapability(name, status, detail);
}

// A Vein class, by StaticClass symbol when the compiler emitted one and by object path otherwise
// (StaticClass() is an inline template: a missing one means "look it up by path", never "absent").
void* VeinClass(const char* symName, const char* pathName) {
    void* c = symName ? Reflect::StaticClass(symName) : nullptr;
    if (!c && pathName) c = Reflect::FindObjectByPath("/Script/Vein", pathName);
    return c;
}

// ---------------------------------------------------------------------------------------------
// world / players (game thread)

void* FindWorld() {
    void* cls = Reflect::StaticClass("UWorld::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Engine", "World");
    if (!cls) return nullptr;
    std::vector<void*> worlds;
    Reflect::GetObjectsOfClass(cls, worlds, true);
    void* fallback = nullptr;
    for (void* w : worlds) {
        int32_t gsOff = OffOf(Reflect::ObjClass(w), "GameState");
        if (gsOff < 0) continue;
        if (!fallback) fallback = w;
        if (ReadPtrAt(w, gsOff)) return w;
    }
    return fallback;
}

void* GameStateOf(void* world) { return world ? ReadPtrAt(world, Off(world, "GameState")) : nullptr; }
void* GameModeOf(void* world) { return world ? ReadPtrAt(world, Off(world, "AuthorityGameMode")) : nullptr; }

struct PlayerInfo {
    void* playerState = nullptr;
    void* controller = nullptr;
    void* pawn = nullptr;      // LANE L3f: the CONTROLLER's current pawn - the only one that counts
    void* statePawn = nullptr; // APlayerState::PawnPrivate, kept only so /debug/inventories can show
                               // when the two disagree. Never used to answer a request.
    std::string gameId;  // SteamID64
    std::string name;
    std::string characterId;  // AVeinPlayerState::LoadedCharacterID, as :8080/status prints it
    int ping = 0;
    bool spawned = false;
};

// The SteamID64 behind an APlayerState.
//
// Three routes, cheapest and safest first:
//  1. a reflected FString property (OnlineID / PlayerUniqueID) - Vein replicates the id as text
//     (AVeinPlayerState::OnRep_OnlineID), so this needs no call at all;
//  2. the stock APlayerState::UniqueID (an FUniqueNetIdRepl). FUniqueNetIdSteam stores the id as a
//     raw uint64 right after its vtable pointer, so the struct's first words are scanned for the
//     SteamID64 bit pattern - no virtual call and no FName stringification;
//  3. AVeinPlayerState::GetPlayerUniqueID(), the game's own getter, last because it is the only
//     one that runs game code.
std::string PlayerStateGameId(void* ps) {
    // `PlayerID` is the SteamID64 as text (AVeinPlayerState::SetPlayerID(FString) @ 0x92b0e80,
    // found by lane L2); `OnlineID` is the replicated twin (AVeinPlayerState::OnRep_OnlineID).
    int32_t strOff = OffAny(ps, {"PlayerID", "OnlineID", "PlayerUniqueID", "UniqueIDString", "SteamID"});
    if (strOff >= 0) {
        std::string s = NormalizeGameId(ReadFStringAt(ps, strOff));
        if (s.size() == 17) return s;
    }
    int32_t idOff = OffAny(ps, {"UniqueId", "UniqueID"});
    if (idOff >= 0) {
        const char* base = (const char*)ps + idOff;
        for (int i = 0; i < 8; i++) {
            if (!MemReadable(base + i * 8, 8)) break;
            uint64_t word = 0;
            memcpy(&word, base + i * 8, 8);
            if (LooksLikeSteamId64(word)) return std::to_string(word);
            // ...or a pointer to an FUniqueNetIdSteam {vtable, uint64 id}.
            void* p = (void*)(uintptr_t)word;
            if (!p || !MemReadable(p, 16)) continue;
            uint64_t inner = 0;
            memcpy(&inner, (const char*)p + 8, 8);
            if (LooksLikeSteamId64(inner)) return std::to_string(inner);
        }
    }
    auto getter = Fn<FnGetFString>("AVeinPlayerState::GetPlayerUniqueID");
    if (getter) {
        FString out{};
        getter(&out, ps);
        std::string s = Reflect::ToStd(out, true);
        s = NormalizeGameId(s);
        if (s.size() == 17) return s;
    }
    return "";
}

// LANE L3e / finding F14: a pawn is not a character.
//
// While the client sits on the character-creation screen its controller possesses
// `BP_CharacterCreationPawn_C` - a real, non-null `APlayerState::PawnPrivate` that is nevertheless
// not a character: it has no `Inventory`, it is not standing anywhere meaningful, and a module that
// teleports or gives to it is talking to a menu. `spawned` used to be "Pawn != null" and therefore
// reported `true` for a player who had not picked a character yet (L6a F14), which is how a whole
// lane of give/teleport evidence ended up being gathered against a creation pawn.
//
// So `spawned` now means "the pawn is an AVeinPlayerCharacter". Every real character blueprint
// (BP_PlayerCharacter_C and friends) derives from it; the creation pawn does not. When the class
// cannot be resolved at all the check degrades to excluding the creation pawn by name, which is
// still strictly better than trusting a null test.
bool PawnIsPlayerCharacter(void* pawn) {
    if (!pawn || !MemReadable(pawn, 0x40)) return false;
    void* cls = VeinClass("AVeinPlayerCharacter::StaticClass", "VeinPlayerCharacter");
    if (cls) return Reflect::IsA(pawn, cls);
    std::string n = Reflect::ClassName(pawn);
    return n.find("CharacterCreation") == std::string::npos && n.find("Spectator") == std::string::npos;
}

// LANE L3f / finding F19: the current pawn is the CONTROLLER's pawn.
//
// `ReadPlayer` used to take the pawn from `APlayerState::PawnPrivate`. That field is replication
// state: it is set when a pawn is possessed and it is only cleared by the *pawn* on its way out, so
// across a death and a respawn it can still name the old character while the controller has long
// since possessed the new one. Everything downstream - the inventory, `giveItem`, the location, the
// teleport verification - then talks to a body the player is not in.
//
// `AController::Pawn` (DWARF: `AController` +0x300, a `TObjectPtr<APawn>`; resolved here by
// reflection, never by that offset) is what `AController::GetPawn()` returns and what possession
// updates first. It is the single source of truth for "which body is this player in right now".
void* ControllerPawn(void* controller) {
    if (!controller || !MemReadable(controller, 0x40)) return nullptr;
    void* pawn = ReadPtrAt(controller, Off(controller, "Pawn"));
    return (pawn && MemReadable(pawn, 0x40)) ? pawn : nullptr;
}

// AVeinPlayerState::LoadedCharacterID (an FGuid), rendered the way VEIN's own :8080/status and the
// `selected character <id>` log line render it. Reflection first; the DWARF offset is a guarded
// fallback for the case where the field is not a UPROPERTY on this build.
std::string PlayerStateCharacterId(void* ps) {
    if (!ps) return "";
    int32_t off = Off(ps, "LoadedCharacterID");
    if (off < 0) {
        // DWARF (VeinServer-Linux-Test.debug, v0.024h8): AVeinPlayerState is 1048 bytes and
        // LoadedCharacterID sits at 0x3e4. Only trusted when the object really is that class.
        void* cls = VeinClass("AVeinPlayerState::StaticClass", "VeinPlayerState");
        if (!cls || !Reflect::IsA(ps, cls)) return "";
        off = 0x3e4;
    }
    if (!MemReadable((const char*)ps + off, 16)) return "";
    uint32_t g[4];
    memcpy(g, (const char*)ps + off, sizeof g);
    return ActionsUtil::GuidDigits(g[0], g[1], g[2], g[3]);
}

bool ReadPlayer(void* ps, PlayerInfo& out) {
    if (!ps || !MemReadable(ps, 0x40)) return false;
    out.playerState = ps;
    out.gameId = PlayerStateGameId(ps);
    out.name = ReadFStringAt(ps, Off(ps, "PlayerNamePrivate"));
    if (out.name.empty()) out.name = state::CharacterName(out.gameId);
    else if (!out.gameId.empty()) state::NoteCharacterName(out.gameId, out.name);
    out.controller = ReadPtrAt(ps, Off(ps, "Owner"));
    out.statePawn = ReadPtrAt(ps, Off(ps, "PawnPrivate"));
    // LANE L3f: the controller's pawn, and only it. The player-state pawn is used exclusively when
    // there is no controller at all to ask, where it is the only thing there is; it is never
    // preferred over a live controller's answer.
    out.pawn = ControllerPawn(out.controller);
    if (!out.pawn && !out.controller) out.pawn = out.statePawn;
    out.spawned = PawnIsPlayerCharacter(out.pawn);
    out.characterId = PlayerStateCharacterId(ps);
    int32_t pingOff = Off(ps, "CompressedPing");
    if (pingOff >= 0 && MemReadable((const char*)ps + pingOff, 1))
        out.ping = (int)(*(const uint8_t*)((const char*)ps + pingOff)) * 4;
    return !out.gameId.empty() || !out.name.empty();
}

std::vector<PlayerInfo> ReadPlayers() {
    std::vector<PlayerInfo> out;
    void* gs = GameStateOf(FindWorld());
    if (!gs) return out;
    int32_t arrOff = Off(gs, "PlayerArray");
    if (arrOff < 0 || !MemReadable((const char*)gs + arrOff, 16)) return out;
    TArray<void*> arr;
    memcpy(&arr, (const char*)gs + arrOff, sizeof arr);
    if (!arr.Data || arr.Num <= 0 || arr.Num > 256 || !MemReadable(arr.Data, (size_t)arr.Num * 8)) return out;
    for (int32_t i = 0; i < arr.Num; i++) {
        PlayerInfo p;
        if (ReadPlayer(arr.Data[i], p)) out.push_back(p);
    }
    return out;
}

Mutex g_seenLock;
std::map<std::string, std::string> g_firstSeen;
std::string FirstSeen(const std::string& gameId) {
    Guard g(g_seenLock);
    auto it = g_firstSeen.find(gameId);
    if (it != g_firstSeen.end()) return it->second;
    std::string now = IsoNowUtc();
    g_firstSeen[gameId] = now;
    return now;
}

// -------------------------------------------------------------------------------------------
// LANE L9: the player snapshot (plugin/docs/gamethread-policy.md).
//
// /players, /players/{id} and /players/{id}/location used to enter the game thread on EVERY request
// and build their JSON there. Takaro polls positions every 30 s, so that freshness was never worth
// a game-thread entry per call. The game thread now only copies raw fields into POD rows; the JSON
// is built on the HTTP thread from a cache that is at most TAKARO_SNAPSHOT_TTL_MS old, and the
// refresh is LAZY - no request, no game-thread entry at all.
struct PlayerRow {
    std::string gameId, name, characterId, pawnClass;
    int ping = 0;
    bool spawned = false, hasPawn = false, hasLoc = false;
    double loc[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
};

Mutex g_snapLock;
std::vector<PlayerRow> g_snapRows;
uint64_t g_snapAtMs = 0;
std::atomic<uint64_t> g_snapRefreshes{0}, g_snapServedFromCache{0};

uint64_t SnapshotTtlMs() {
    static uint64_t cached = 0;
    if (!cached) {
        long v = strtol(ConfigValue("TAKARO_SNAPSHOT_TTL_MS", "snapshotTtlMs", "500").c_str(), nullptr, 10);
        if (v < 0) v = 0;
        if (v > 30000) v = 30000;
        cached = (uint64_t)(v ? v : 1);
    }
    return cached;
}

bool ActorLocation(void* actor, double loc[3], double rot[3]);

// Runs on the game thread. Pointer reads only - no JSON, no allocation beyond the strings the
// engine hands us.
void BuildPlayerSnapshotOnGameThread() {
    std::vector<PlayerRow> rows;
    for (auto& p : ReadPlayers()) {
        PlayerRow r;
        r.gameId = p.gameId;
        r.name = p.name;
        r.characterId = p.characterId;
        r.ping = p.ping;
        r.spawned = p.spawned;
        r.hasPawn = p.pawn != nullptr;
        if (p.pawn) {
            r.pawnClass = Reflect::ClassName(p.pawn);
            r.hasLoc = ActorLocation(p.pawn, r.loc, r.rot);
        }
        rows.push_back(r);
    }
    Guard g(g_snapLock);
    g_snapRows.swap(rows);
    g_snapAtMs = NowMs();
}

// Returns false only when the game thread is unreachable AND there is no usable cache.
bool PlayerRows(std::vector<PlayerRow>& out, uint64_t& ageMs) {
    uint64_t now = NowMs();
    {
        Guard g(g_snapLock);
        if (g_snapAtMs && now - g_snapAtMs <= SnapshotTtlMs()) {
            out = g_snapRows;
            ageMs = now - g_snapAtMs;
            g_snapServedFromCache++;
            return true;
        }
    }
    g_snapRefreshes++;
    bool ran = GameThread::Run([] { Perf::Scope sc("snapshot.players"); BuildPlayerSnapshotOnGameThread(); }, 5000);
    Guard g(g_snapLock);
    if (!ran && !g_snapAtMs) return false;
    out = g_snapRows;
    ageMs = NowMs() - g_snapAtMs;
    return true;
}

std::string PlayerRowJson(const PlayerRow& r);

std::string PlayerRowJson(const PlayerRow& r) {
    return "{\"gameId\":" + JsonStr(r.gameId) + ",\"name\":" + JsonStr(r.name) + ",\"steamId\":" +
           JsonStr(r.gameId) + ",\"platformId\":" + JsonStr("steam:" + r.gameId) + ",\"ping\":" +
           std::to_string(r.ping) + ",\"spawned\":" + (r.spawned ? "true" : "false") + ",\"pawn\":" +
           JsonStr(r.pawnClass) + ",\"characterId\":" +
           (r.characterId.empty() ? "null" : JsonStr(r.characterId)) + ",\"online\":true,\"connectedAt\":" +
           JsonStr(FirstSeen(r.gameId)) + "}";
}

bool FindPlayerById(const std::string& id, PlayerInfo& out) {
    std::string needle = NormalizeGameId(id);
    for (auto& p : ReadPlayers()) {
        if (p.gameId == needle || Lower(p.name) == Lower(id)) {
            out = p;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// positions

bool ActorLocation(void* actor, double loc[3], double rot[3]) {
    void* root = ReadPtrAt(actor, Off(actor, "RootComponent"));
    if (!root) return false;
    int32_t lo = Off(root, "RelativeLocation");
    int32_t ro = Off(root, "RelativeRotation");
    if (lo < 0 || !MemReadable((const char*)root + lo, 24)) return false;
    memcpy(loc, (const char*)root + lo, 24);
    if (ro >= 0 && MemReadable((const char*)root + ro, 24)) memcpy(rot, (const char*)root + ro, 24);
    return true;
}

// ---------------------------------------------------------------------------------------------
// the admin component: VEIN's own server-authoritative moderation API
//
// It lives on every AVeinPlayerController. Every RPC we call takes its *target* as an argument, so
// any live instance is a valid receiver; the class default object is the fallback for a server with
// nobody online, and is only used for calls that do not touch the receiver's own world state.

void* AdminComponentClass() { return VeinClass("UAdminComponent::StaticClass", "AdminComponent"); }

void* FindAdminComponent(std::string& why) {
    void* cls = AdminComponentClass();
    if (!cls) {
        why = "UAdminComponent class not found";
        return nullptr;
    }
    std::vector<void*> comps;
    if (Reflect::GetObjectsOfClass(cls, comps, true)) {
        for (void* c : comps) {
            if (!c || !MemReadable(c, 0x40)) continue;
            if (Reflect::ObjName(c).rfind("Default__", 0) == 0) continue;
            return c;
        }
    }
    void* cdo = Reflect::ClassDefaultObject(cls);
    if (cdo) {
        why = "no live UAdminComponent (nobody online); using the class default object";
        return cdo;
    }
    why = "no UAdminComponent instance and no class default object";
    return nullptr;
}

// The *owner-gated* flavour, for admin RPCs whose implementation dereferences the component's owner.
//
// PROVEN CRASH, lane L3d 2026-09-17 08:07:00Z: `UAdminComponent::Server_SendServerMessage_Implementation`
// (`AdminComponent.cpp:138`) calls `AVeinPlayerController::IsAdmin()` on its owner, so a component
// with no owner - the class default object, which `FindAdminComponent()` returns when nobody is
// online - is `SIGSEGV: invalid attempt to read memory at address 0x2c8`. That is a second, entirely
// separate null-deref from the NetMulticast_SendChat one, in the path that was meant to be the safe
// fallback.
//
// So: only a component that is actually owned by an *online player's* controller is returned, found
// by walking that controller's own subobjects rather than by trusting a global class scan.
void* FindOwnedAdminComponent(std::string& why) {
    void* cls = AdminComponentClass();
    if (!cls) {
        why = "UAdminComponent class not found";
        return nullptr;
    }
    for (const PlayerInfo& p : ReadPlayers()) {
        if (!p.controller || !MemReadable(p.controller, 8)) continue;
        std::vector<void*> objs;
        if (!Reflect::GetObjectsWithOuter(p.controller, objs, true)) continue;
        for (void* o : objs) {
            if (!o || !MemReadable(o, 0x40)) continue;
            if (!Reflect::IsA(o, cls)) continue;
            if (Reflect::ObjName(o).rfind("Default__", 0) == 0) continue;
            return o;
        }
    }
    why = "no UAdminComponent owned by an online player controller (its implementation dereferences "
          "the owner, so the class default object must never be used here)";
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// item catalogue
//
// VEIN items are UClasses (UAdminComponent::Server_GiveItem takes a TSubclassOf<UItem>), not data
// assets, so the catalogue is every loaded UClass deriving from UItem. `code` is the class short
// name and never changes: Takaro's stored items and `giveItem` are keyed on it.
//
// Lane L3c - the human-facing columns. `UItem : public UPrimaryDataAsset` carries its own display
// text (offsets from the depot DWARF, `gdb ptype /o struct UItem` on
// VeinServer-Linux-Test.debug; we still look every one up by name at runtime):
//
//     FText                  Name         @ 928    the item's display name
//     FText                  ShortName    @ 1040   a shorter variant (fallback for Name)
//     FText                  Description  @ 1072   the tooltip description
//     TObjectPtr<UItemType>  Type         @ 120    the category object; UItemType::Name is an
//                                                  FText @ 48 ("Tools", "Medical", ...)
//     float Weight @ 64, bool bStackable @ 72, int32 MaxStack @ 76   (read but not emitted; the
//                                                  Takaro item schema has nowhere to put them)
//
// Every FText is read off the *class default object* by reflection only - FindPropertyByName for
// the offset, then FTextInspector::GetDisplayString, a pure inspector - so nothing is *called* on
// a Blueprint CDO. That is deliberately unlike UItem::GetDefaultNameInvariant(), which is a real
// virtual call into game code against a possibly uninitialised CDO (the shape that crashed the
// Dragonwilds server); it stays available behind TAKARO_ITEM_NAMES=1 as a cross-check only.
//
// When the game has no display text the name falls back to ActionsUtil::HumaniseCode (e.g.
// `BloodPressureCuffItem` -> "Blood Pressure Cuff").

struct ItemDef {
    std::string code, name, description, category;
    float weight = 0;
    int32_t maxStack = 0;
    // LANE L3f / finding F19: UItem::bStackable. VEIN also has `bPseudoStackable`, which only makes
    // the UI GROUP identical entries - it does NOT make one entry hold several units. Only
    // `bStackable` items carry a meaningful `FVirtualItemInstance::Stack`.
    bool stackable = false;
    bool stackableKnown = false;
    void* cls = nullptr;
};
Mutex g_itemLock;
std::vector<ItemDef> g_items;
bool g_itemsBuilt = false;
bool g_itemNamesNative = false;
size_t g_itemsNamed = 0, g_itemsDescribed = 0, g_itemsCategorised = 0, g_itemsAbstractSkipped = 0;
std::string g_itemAbstractFilter = "off";

void* ItemClass() { return VeinClass("UItem::StaticClass", "Item"); }

bool DerivesFrom(void* cls, void* base) {
    if (!cls || !base) return false;
    for (void* s = cls; s; s = Reflect::SuperStruct(s))
        if (s == base) return true;
    return false;
}

// UClass::ClassFlags. DWARF (`print (int)&((UClass*)0)->ClassFlags`) says 0xD4 on this build; it is
// not in Reflect::Lay() because nothing else reads it, so BuildItems validates it against a fact it
// can check live - UItem is declared abstract (StaticClassFlags 0x10000001) - and skips the
// abstract filter entirely when the check fails.
constexpr uint32_t kClassFlagsOff = 0xD4;
constexpr uint32_t kClassAbstract = 0x1;

bool ClassIsAbstract(void* cls) {
    const void* p = (const char*)cls + kClassFlagsOff;
    if (!MemReadable(p, 4)) return false;
    uint32_t flags = 0;
    memcpy(&flags, p, 4);
    return (flags & kClassAbstract) != 0;
}

// The UItemType object a UItem CDO points at -> its display name, else its humanised object name.
std::string ItemCategory(void* cdo, int32_t typeOff) {
    if (!cdo || typeOff < 0) return "";
    const void* p = (const char*)cdo + typeOff;
    if (!MemReadable(p, 8)) return "";
    void* typeObj = nullptr;
    memcpy(&typeObj, p, 8);
    // A TObjectPtr is a raw UObject* here; an unresolved FObjectHandle would be tagged in the low
    // bits, and any UObject is at least 8-byte aligned, so a tagged or unreadable value is dropped.
    if (!typeObj || ((uintptr_t)typeObj & 0x7) || !MemReadable(typeObj, 0x40)) return "";
    void* typeCls = Reflect::ObjClass(typeObj);
    if (!typeCls) return "";
    std::string label = TextToString(typeObj, OffOf(typeCls, "Name"));
    if (label.empty()) {
        std::string objName = Reflect::ObjName(typeObj);
        if (!IsGeneratedArtefact(objName)) label = ActionsUtil::HumaniseCode(objName);
    }
    return label;
}

// Game thread. Builds (once) the UItem class catalogue.
bool BuildItems() {
    void* itemCls = ItemClass();
    void* classCls = Reflect::StaticClass("UClass::StaticClass");
    if (!classCls) classCls = Reflect::FindObjectByPath("/Script/CoreUObject", "Class");
    if (!itemCls || !classCls) return false;
    std::vector<void*> classes;
    if (!Reflect::GetObjectsOfClass(classCls, classes, true)) return false;

    // Property offsets are declared on UItem, so they are the same for every subclass: look them
    // up once on the base rather than 1400 times.
    const int32_t nameOff = OffOf(itemCls, "Name");
    const int32_t shortOff = OffOf(itemCls, "ShortName");
    const int32_t descOff = OffOf(itemCls, "Description");
    const int32_t typeOff = OffOf(itemCls, "Type");
    const int32_t weightOff = OffOf(itemCls, "Weight");
    const int32_t maxStackOff = OffOf(itemCls, "MaxStack");
    const int32_t stackableOff = OffOf(itemCls, "bStackable");

    // Only trust the abstract filter when the flag word really is where DWARF said it is.
    const bool abstractOk = ClassIsAbstract(itemCls);

    bool wantNames = ConfigValue("TAKARO_ITEM_NAMES", "itemNames", "0") == "1";
    auto defaultName = wantNames ? Fn<FnGetFString>("UItem::GetDefaultNameInvariant") : nullptr;

    std::vector<ItemDef> built;
    std::vector<ItemDef> abstractOnes;
    size_t named = 0, described = 0, categorised = 0;
    for (void* c : classes) {
        if (!c || !MemReadable(c, 0x40)) continue;
        if (c == itemCls || !DerivesFrom(c, itemCls)) continue;
        ItemDef d;
        d.code = Reflect::ObjName(c);
        if (IsGeneratedArtefact(d.code)) continue;
        d.cls = c;
        void* cdo = Reflect::ClassDefaultObject(c);
        if (cdo && !MemReadable(cdo, 0x40)) cdo = nullptr;
        if (cdo) {
            d.name = TextToString(cdo, nameOff);
            if (d.name.empty()) d.name = TextToString(cdo, shortOff);
            d.description = TextToString(cdo, descOff);
            d.category = ItemCategory(cdo, typeOff);
            if (weightOff >= 0 && MemReadable((const char*)cdo + weightOff, 4))
                memcpy(&d.weight, (const char*)cdo + weightOff, 4);
            if (maxStackOff >= 0 && MemReadable((const char*)cdo + maxStackOff, 4))
                memcpy(&d.maxStack, (const char*)cdo + maxStackOff, 4);
            if (stackableOff >= 0 && MemReadable((const char*)cdo + stackableOff, 1)) {
                d.stackable = *(const uint8_t*)((const char*)cdo + stackableOff) != 0;
                d.stackableKnown = true;
            }
            // Optional cross-check / last resort: the native getter. Off unless TAKARO_ITEM_NAMES=1.
            if (d.name.empty() && defaultName) {
                FString out{};
                defaultName(&out, cdo);
                d.name = Reflect::ToStd(out, true);
            }
        }
        if (!d.name.empty()) named++;
        if (!d.description.empty()) described++;
        if (!d.category.empty()) categorised++;
        if (d.name.empty()) d.name = ActionsUtil::HumaniseCode(d.code);
        if (abstractOk && ClassIsAbstract(c)) {
            abstractOnes.push_back(std::move(d));  // cannot be spawned, so it is not a real item
            continue;
        }
        built.push_back(std::move(d));
    }
    // An abstract filter that swallows the catalogue is a wrong filter: keep everything instead.
    bool abstractApplied = abstractOk && !abstractOnes.empty();
    if (abstractApplied && abstractOnes.size() * 10 > (built.size() + abstractOnes.size()) * 4) {
        for (auto& d : abstractOnes) built.push_back(std::move(d));
        abstractApplied = false;
    }
    if (built.empty()) return false;
    std::sort(built.begin(), built.end(),
              [](const ItemDef& a, const ItemDef& b) { return a.code < b.code; });
    Guard g(g_itemLock);
    g_items = std::move(built);
    g_itemsBuilt = true;
    g_itemNamesNative = defaultName != nullptr;
    g_itemsNamed = named;
    g_itemsDescribed = described;
    g_itemsCategorised = categorised;
    g_itemsAbstractSkipped = abstractApplied ? abstractOnes.size() : 0;
    g_itemAbstractFilter = abstractApplied  ? "on"
                           : abstractOk     ? "off (nothing matched)"
                                            : "off (UClass::ClassFlags not where DWARF said)";
    return true;
}

// Catalogue display name for an exact code. Takes g_itemLock itself; "" when unknown.
// LANE L3f: is one array entry of this item one unit, or a stack? Returns false for "unknown",
// which is the safe answer: `amount` then falls back to 1 per entry and can never over-report.
bool CatalogueStackable(const std::string& code, bool& known) {
    Guard g(g_itemLock);
    known = false;
    if (!g_itemsBuilt) return false;
    for (auto& d : g_items)
        if (d.code == code) {
            known = d.stackableKnown;
            return d.stackable;
        }
    return false;
}

std::string CatalogueName(const std::string& code) {
    Guard g(g_itemLock);
    if (!g_itemsBuilt) return "";
    for (auto& d : g_items)
        if (d.code == code) return d.name;
    return "";
}

// Exact code (ci), then exact name, then a unique case-insensitive substring. Caller holds
// g_itemLock. The matching rule itself lives in ActionsUtil so the tests cover it.
const ItemDef* LookupItem(const std::string& code, std::string& err) {
    if (!g_itemsBuilt) {
        err = "item catalogue not loaded yet";
        return nullptr;
    }
    std::vector<ActionsUtil::CatalogueEntry> entries;
    entries.reserve(g_items.size());
    for (auto& d : g_items) entries.push_back({d.code, d.name});
    int idx = ActionsUtil::LookupIndex(entries, code, err);
    return idx < 0 ? nullptr : &g_items[(size_t)idx];
}

// ---------------------------------------------------------------------------------------------
// inventory
//
// VEIN items are NOT UObjects hanging off the component: they are *virtual* items. Enumerating
// UObjects under the component would return nothing at all. The depot's DWARF settles the shape:
//
//   UBaseInventoryComponent::Items : FInventoryArray          (a fast-array serialiser, not a TArray)
//   FInventoryArray::Items         : TArray<FVirtualItemInstance>
//   FVirtualItemInstance           : 168 bytes, { Item: TSoftClassPtr<UItem>, Variant, ItemData,
//                                     AcquisitionTime, Slot: FName, Inventory, Instance, ID,
//                                     Stack: int32, Decay, DamagePerc, CustomLabel: FString, Flags }
//   UBaseInventoryComponent::Name  : FText
//
// So the array is one level down, inside the wrapper struct. The DWARF told us the field *names*
// and types; every offset below still comes from FindPropertyByName on the live script struct, and
// the element stride from the struct's own PropertiesSize.

struct InvItem {
    std::string code, name, inventory, path;
    int amount = 1;
    int slot = -1;
};

std::vector<InvItem> ReadInventory(const PlayerInfo& p, std::string& detail);  // LANE L3e fwd

struct VirtualItemLayout {
    void* st = nullptr;
    size_t stride = 0;
    int32_t item = -1, stack = -1, customLabel = -1, id = -1;
    // Offset of FInventoryArray::Items within the wrapper struct; 0 when the component's property
    // turns out to be a plain TArray after all.
    int32_t innerArray = 0;
    bool ok() const { return st && stride && item >= 0; }
};

VirtualItemLayout ReadVirtualItemLayout() {
    VirtualItemLayout v;
    v.st = Reflect::FindObjectByPath("/Script/Vein", "VirtualItemInstance");
    if (!v.st) return v;
    uint32_t size = 0;
    if (MemReadable((const char*)v.st + Reflect::Lay().structPropertiesSize, 4))
        size = *(const uint32_t*)((const char*)v.st + Reflect::Lay().structPropertiesSize);
    if (!size || size > 4096) return v;
    v.stride = (size + 7) & ~7u;
    auto off = [&](const char* n) -> int32_t {
        void* p = Reflect::FindProperty(v.st, n);
        return p ? Reflect::PropertyOffset(p) : -1;
    };
    v.item = off("Item");
    v.stack = off("Stack");
    v.customLabel = off("CustomLabel");
    v.id = off("ID");
    // UBaseInventoryComponent::Items is an FInventoryArray wrapper whose own `Items` member is the
    // real TArray<FVirtualItemInstance>.
    void* wrapper = Reflect::FindObjectByPath("/Script/Vein", "InventoryArray");
    if (wrapper) {
        void* inner = Reflect::FindProperty(wrapper, "Items");
        int32_t o = inner ? Reflect::PropertyOffset(inner) : -1;
        if (o >= 0 && o < 0x10000) v.innerArray = o;
    }
    return v;
}

// `Item` is a soft class pointer: FSoftObjectPath { FTopLevelAssetPath{PackageName, AssetName},
// FString SubPathString } followed by a weak pointer. The class name is the AssetName FName, i.e.
// the second word - read as a raw FName and only stringified once it looks like a live name.
std::string SoftClassName(const char* base) {
    if (!MemReadable(base, 16)) return "";
    FName assetName{};
    memcpy(&assetName, base + 8, sizeof assetName);
    if (!assetName.Comparison) return "";
    return Reflect::NameToString(assetName);
}

// LANE L3f / finding F19: THE player's inventory component - `AVeinCharacter::Inventory` on the
// controller's current pawn, and nothing else.
//
// This replaces the old "collect every UBaseInventoryComponent outered to the pawn or the
// controller" sweep. That sweep had two ways to answer with the wrong container: the controller
// carries a `UOfflineCharacterCache` (DWARF: `AVeinPlayerController` +0x840) with cached character
// state, and `UPersistentCorpseInventory` is itself a `UBaseInventoryComponent` subclass, so a
// dead body's loot passed the `IsA` test as readily as the live character's own bag. Combined with
// a stale `PawnPrivate` it produced the reported defect: Takaro showed "Corn 2" for a character
// that was carrying no corn.
//
// Three gates, all of which must pass:
//   1. the pawn is the controller's CURRENT pawn (PlayerInfo::pawn is now resolved that way);
//   2. the pawn is an AVeinPlayerCharacter, not the character-creation pawn (finding F14);
//   3. the component is the pawn's own `Inventory` property, is a UBaseInventoryComponent, and its
//      class name is not a corpse/cache/container class (ActionsUtil::IsPlayerInventoryClass).
void* PawnInventory(void* pawn, std::string& why) {
    if (!pawn) {
        why = "the player has no character in the world (no pawn possessed)";
        return nullptr;
    }
    if (!PawnIsPlayerCharacter(pawn)) {
        why = "the player's current pawn is " + Reflect::ClassName(pawn) + ", not a character";
        return nullptr;
    }
    int32_t off = Off(pawn, "Inventory");
    if (off < 0) {
        why = "the character class " + Reflect::ClassName(pawn) + " has no reflected Inventory property";
        return nullptr;
    }
    void* inv = ReadPtrAt(pawn, off);
    if (!inv || !MemReadable(inv, 0x40)) {
        why = "the character's Inventory component is null";
        return nullptr;
    }
    void* invCls = VeinClass("UBaseInventoryComponent::StaticClass", "BaseInventoryComponent");
    if (invCls && !Reflect::IsA(inv, invCls)) {
        why = "the character's Inventory is a " + Reflect::ClassName(inv) + ", not a UBaseInventoryComponent";
        return nullptr;
    }
    std::string cls = Reflect::ClassName(inv);
    if (!ActionsUtil::IsPlayerInventoryClass(cls)) {
        why = "the character's Inventory is a " + cls + ", which is not a player inventory";
        return nullptr;
    }
    return inv;
}

// LANE L3e / now diagnostic only: every UBaseInventoryComponent under the pawn or the controller.
// `/debug/inventories` uses it to show what the old resolution would have picked; nothing that
// answers a Takaro request may use it (see PawnInventory).
std::vector<void*> FindInventoryComponents(const PlayerInfo& p) {
    std::vector<void*> comps, tmp;
    void* invCls = VeinClass("UBaseInventoryComponent::StaticClass", "BaseInventoryComponent");
    if (!invCls) return comps;
    for (void* owner : {p.pawn, p.controller}) {
        if (!owner) continue;
        tmp.clear();
        if (Reflect::GetObjectsWithOuter(owner, tmp, true))
            for (void* c : tmp)
                if (c && Reflect::IsA(c, invCls)) comps.push_back(c);
    }
    return comps;
}

// LANE L3e: how many units of `code` the player holds right now, summed over every inventory
// component and every stack. This is the before/after measurement /give is verified with.
int CountItem(const PlayerInfo& p, const std::string& code) {
    std::string detail;
    int n = 0;
    for (const InvItem& it : ReadInventory(p, detail))
        if (Lower(it.code) == Lower(code)) n += it.amount;
    return n;
}

std::vector<InvItem> ReadInventory(const PlayerInfo& p, std::string& detail) {
    std::vector<InvItem> out;
    void* invCls = VeinClass("UBaseInventoryComponent::StaticClass", "BaseInventoryComponent");
    if (!invCls) {
        detail = "UBaseInventoryComponent class not found";
        return out;
    }
    VirtualItemLayout lay = ReadVirtualItemLayout();
    if (!lay.ok()) {
        detail = "FVirtualItemInstance is not reflectable (no Item field or an implausible size)";
        return out;
    }
    // LANE L3f: exactly one component, resolved from the controller's current pawn.
    std::string why;
    void* only = PawnInventory(p.pawn, why);
    if (!only) {
        detail = why;
        return out;
    }
    std::vector<void*> comps{only};
    for (void* c : comps) {
        std::string invName;
        int32_t nameOff = Off(c, "Name");
        if (nameOff >= 0) {
            invName = TextToString(c, nameOff);
            if (invName.empty()) invName = ReadFStringAt(c, nameOff);
        }
        if (invName.empty()) invName = Reflect::ObjName(c);

        int32_t itemsOff = Off(c, "Items");
        if (itemsOff < 0) continue;
        itemsOff += lay.innerArray;  // descend into FInventoryArray
        if (!MemReadable((const char*)c + itemsOff, 16)) continue;
        TArray<char> arr{};
        memcpy(&arr, (const char*)c + itemsOff, sizeof arr);
        if (arr.Num <= 0 || arr.Num > 8192) continue;
        if (!MemReadable(arr.Data, (size_t)arr.Num * lay.stride)) continue;
        for (int32_t i = 0; i < arr.Num; i++) {
            const char* e = arr.Data + (size_t)i * lay.stride;
            InvItem it;
            it.inventory = invName;
            it.slot = i;
            // Finding F11: the soft class path is folded onto the UClass short name, so this is
            // the *same* `code` GET /items lists and giveItem accepts. `path` keeps the raw value.
            it.path = SoftClassName(e + lay.item);
            it.code = ActionsUtil::ItemCodeFromSoftPath(it.path);
            if (it.code.empty()) continue;
            if (lay.customLabel >= 0) it.name = ReadFStringAt((void*)e, lay.customLabel);
            if (it.name.empty()) it.name = CatalogueName(it.code);
            if (it.name.empty()) it.name = ActionsUtil::HumaniseCode(it.code);
            // LANE L3f / finding F19: `Stack` is only a unit count for a STACKABLE item.
            //
            // This is the defect Tester reported as "Takaro says Corn 2, I have no corn". Corn's
            // class defaults are `bStackable=false, bPseudoStackable=true, MaxStack=50,
            // Weight=0.25`. `bPseudoStackable` only makes the UI group identical rows; each array
            // entry is still exactly ONE corn. `POST /give amount:3` nevertheless built a single
            // instance with `SetStack(3)` (see the give loop), the game gave the player one corn,
            // and the reader then multiplied that one corn back up by the `Stack` field. Measured
            // on the live rig 2026-09-17: the client's inventory showed one Corn row at 0,2 lbs
            // with a total carry weight of 23,1 lbs - which is 0.25 for exactly one corn (two would
            // have read 0,5 and 23,3) - while the plugin reported `amount: 2`.
            //
            // So: a stackable item's entry means `Stack` units; anything else - including an item
            // the catalogue has not loaded yet - means one unit.
            bool stackableKnown = false;
            if (CatalogueStackable(it.code, stackableKnown) && stackableKnown && lay.stack >= 0 &&
                MemReadable(e + lay.stack, 4)) {
                int32_t q = *(const int32_t*)(e + lay.stack);
                if (q > 0 && q < 1000000) it.amount = q;
            }
            out.push_back(std::move(it));
        }
    }
    // LANE L3e: an empty inventory is NOT a defect and must not degrade the capability - the sidecar
    // refuses an action whose capability is not ok, so "this player is carrying nothing" used to
    // switch playerInventory off for everybody.
    
    return out;
}

// ---------------------------------------------------------------------------------------------
// locations: the streamed-in ALocationMarker actors

struct NamedLocation {
    std::string code, name;
    double x = 0, y = 0, z = 0;
};

std::vector<NamedLocation> ReadLocations() {
    std::vector<NamedLocation> out;
    void* cls = VeinClass("ALocationMarker::StaticClass", "LocationMarker");
    if (!cls) return out;
    std::vector<void*> actors;
    if (!Reflect::GetObjectsOfClass(cls, actors, true)) return out;
    for (void* a : actors) {
        if (!a || !MemReadable(a, 0x40)) continue;
        std::string code = Reflect::ObjName(a);
        if (IsGeneratedArtefact(code)) continue;
        NamedLocation l;
        l.code = code;
        // The readable name is a reflected FText/FString property; the native GetLocationName() is
        // deliberately not called (see docs/actions-design.md).
        int32_t nameOff = OffAny(a, {"LocationName", "DisplayName", "Name", "MarkerName"});
        if (nameOff >= 0) {
            l.name = TextToString(a, nameOff);
            if (l.name.empty()) l.name = ReadFStringAt(a, nameOff);
        }
        if (l.name.empty()) l.name = code;
        bool dup = false;
        for (auto& e : out) dup = dup || Lower(e.code) == Lower(l.code);
        if (dup) continue;
        double loc[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
        if (ActorLocation(a, loc, rot)) {
            l.x = loc[0];
            l.y = loc[1];
            l.z = loc[2];
        }
        out.push_back(std::move(l));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// lane L2c: what the killer was holding
//
// The death event hands us an `AActor* DamageCauser`. For a melee swing that actor is the
// `AMeleeEquippedItem` the character has in hand, and DWARF says every `AEquippedItem` carries the
// item it represents as a plain member:
//
//   AEquippedItem::ItemInstance : FVirtualItemInstance @ 0x2d8   (the same struct the inventory
//                                                                 reader already parses)
// (AVeinCharacter also holds an FEquippedItemAttachment @ 0xa20 for what the character has in hand
// right now; it is deliberately NOT used - the current loadout is not evidence about a past hit.)
//
// So the weapon's *display* name is the same lookup GET /items answers with: the soft class path
// -> the item code -> the catalogue name. Offsets are all resolved by FindPropertyByName; the
// DWARF above only says which names to ask for. This runs on the game thread (the death hook
// already does) and returns "" rather than guessing.

// The UItem display name behind an FVirtualItemInstance at `base`.
std::string ItemNameFromInstance(const char* base) {
    if (!base) return "";
    VirtualItemLayout lay = ReadVirtualItemLayout();
    if (!lay.ok()) return "";
    if (!MemReadable(base + lay.item, 16)) return "";
    std::string code = ActionsUtil::ItemCodeFromSoftPath(SoftClassName(base + lay.item));
    if (code.empty()) return "";
    std::string name = CatalogueName(code);
    return name.empty() ? ActionsUtil::HumaniseCode(code) : name;
}

std::string EquippedItemNameImpl(void* actor) {
    if (!actor || !MemReadable(actor, 0x40)) return "";
    void* base = VeinClass("AEquippedItem::StaticClass", "EquippedItem");
    if (base && !Reflect::IsA(actor, base)) return "";
    int32_t off = Off(actor, "ItemInstance");
    if (off < 0) return "";
    return ItemNameFromInstance((const char*)actor + off);
}

// ---------------------------------------------------------------------------------------------
// bans: VEIN's own list
//
// AVeinGameStateBase carries a replicated TArray<FBan> with BanID(FString id, FString reason) /
// UnbanID(FString id) / ReloadBans(). Unlike the Dragonwilds equivalent it is keyed on the id
// *string*, so it accepts a player the server has never seen and works offline. The plugin list in
// takaro/bans.json is still written, because it is what carries the reason and the expiry the
// sidecar owns, and what lane L2's PreLogin hook refuses a rejoin with.

struct GameBan {
    std::string gameId, name, reason;
};

// Reads the game state's TArray<FBan> through reflection. Every offset comes from FBan's script
// struct at call time; the element stride is the struct's PropertiesSize, aligned to 8.
std::vector<GameBan> ReadGameBans(std::string& why) {
    std::vector<GameBan> out;
    void* gs = GameStateOf(FindWorld());
    if (!gs) {
        why = "no game state";
        return out;
    }
    // DWARF: AVeinGameStateBase::Bans is a TArray<FBan>; FBan is { FString ID; FString Reason; }.
    int32_t off = OffAny(gs, {"Bans", "BanList", "BannedIDs"});
    if (off < 0 || !MemReadable((const char*)gs + off, 16)) {
        why = "AVeinGameStateBase has no reflectable ban array";
        return out;
    }
    void* st = Reflect::FindObjectByPath("/Script/Vein", "Ban");
    if (!st) {
        why = "the FBan script struct was not found";
        return out;
    }
    uint32_t size = 0;
    if (MemReadable((const char*)st + Reflect::Lay().structPropertiesSize, 4))
        size = *(const uint32_t*)((const char*)st + Reflect::Lay().structPropertiesSize);
    if (!size || size > 1024) {
        why = "FBan has an implausible size";
        return out;
    }
    size_t stride = (size + 7) & ~7u;
    void* pId = nullptr;
    const char* idName = nullptr;
    for (const char* n : {"ID", "Id", "UniqueID", "PlayerID", "SteamID"}) {
        pId = Reflect::FindProperty(st, n);
        if (pId) {
            idName = n;
            break;
        }
    }
    (void)idName;
    if (!pId) {
        why = "FBan has no reflectable id field";
        return out;
    }
    int32_t idOff = Reflect::PropertyOffset(pId);
    void* pReason = nullptr;
    for (const char* n : {"Reason", "BanReason", "Message"})
        if ((pReason = Reflect::FindProperty(st, n))) break;
    void* pName = nullptr;
    for (const char* n : {"Name", "PlayerName", "DisplayName"})
        if ((pName = Reflect::FindProperty(st, n))) break;
    int32_t reasonOff = pReason ? Reflect::PropertyOffset(pReason) : -1;
    int32_t nameOff = pName ? Reflect::PropertyOffset(pName) : -1;

    TArray<char> arr{};
    memcpy(&arr, (const char*)gs + off, sizeof arr);
    if (arr.Num < 0 || arr.Num > 8192) {
        why = "the ban array has an implausible length";
        return out;
    }
    if (!arr.Num) return out;
    if (!MemReadable(arr.Data, (size_t)arr.Num * stride)) {
        why = "the ban array is not readable";
        return out;
    }
    for (int32_t i = 0; i < arr.Num; i++) {
        char* e = arr.Data + (size_t)i * stride;
        GameBan b;
        b.gameId = NormalizeGameId(ReadFStringAt(e, idOff));
        if (b.gameId.empty()) continue;
        if (reasonOff >= 0) b.reason = ReadFStringAt(e, reasonOff);
        if (nameOff >= 0) b.name = ReadFStringAt(e, nameOff);
        out.push_back(std::move(b));
    }
    return out;
}

// Adds or removes an id in the game's own ban list. Returns false with `err` set.
// LANE L3e: is `gameId` banned *according to a fresh read of the two lists we just wrote*? This is
// what makes /ban and /unban report an observed effect rather than the return value of a write.
bool BanListedNow(const std::string& gameId, bool& inPluginList, bool& inGameList) {
    inPluginList = false;
    for (const auto& b : state::BanList())
        if (b.gameId == gameId) inPluginList = true;
    inGameList = false;
    std::string why;
    for (const auto& b : ReadGameBans(why))
        if (b.gameId == gameId) inGameList = true;
    return inPluginList || inGameList;
}

bool WriteGameBan(const std::string& gameId, const std::string& reason, bool add, std::string& err) {
    void* gs = GameStateOf(FindWorld());
    if (!gs) {
        err = "no game state";
        return false;
    }
    if (add) {
        auto ban = Fn<FnBanId>("AVeinGameStateBase::BanID");
        if (!ban) {
            err = "AVeinGameStateBase::BanID unresolved";
            return false;
        }
        GameFString id(gameId), why(reason.empty() ? std::string("Banned") : reason);
        if (!id.ok || !why.ok) {
            err = "could not allocate the ban strings";
            return false;
        }
        ban(gs, &id.fs, &why.fs);
        err = "added to the game's ban list";
        return true;
    }
    auto unban = Fn<FnUnbanId>("AVeinGameStateBase::UnbanID");
    if (!unban) {
        err = "AVeinGameStateBase::UnbanID unresolved";
        return false;
    }
    GameFString id(gameId);
    if (!id.ok) {
        err = "could not allocate the ban string";
        return false;
    }
    unban(gs, &id.fs);
    err = "removed from the game's ban list";
    return true;
}

// ---------------------------------------------------------------------------------------------
// chat
//
// AVeinGameStateBase::NetMulticast_SendChat is the replicated multicast entry. It is deliberately
// NOT the _Implementation address: calling the implementation on the server would run the local
// half only and never reach a client.
//
// EChatSegment is read from the depot's DWARF, not guessed:
//   enum class EChatSegment : uint8 { All = 0, Local = 1, Global = 2, Radio = 3 }
// A server broadcast is Global. TAKARO_CHAT_SEGMENT overrides it.
enum : int { kChatAll = 0, kChatLocal = 1, kChatGlobal = 2, kChatRadio = 3 };

int ChatSegment() {
    std::string v = ConfigValue("TAKARO_CHAT_SEGMENT", "chatSegment", "");
    return v.empty() ? (int)kChatGlobal : atoi(v.c_str());
}

// The chat sender, for the one path that needs one.
//
// LANE L3d / THE CRASH: NetMulticast_SendChat's `Sender` is read through a TObjectPtr with no null
// check, so a null sender is a SIGSEGV of the whole dedicated server. This returns only a pointer
// that (a) is non-null, (b) is readable, and (c) passes a class-chain walk against
// AVeinPlayerState. Anything less and the caller must not use the chat path at all.
void* FindChatSender(std::string& senderName) {
    void* cls = VeinClass(nullptr, "VeinPlayerState");
    for (const PlayerInfo& p : ReadPlayers()) {
        void* ps = p.playerState;
        if (!ps || !MemReadable(ps, 8)) continue;
        if (cls && !Reflect::IsA(ps, cls)) continue;
        if (!cls && Reflect::ClassName(ps).find("PlayerState") == std::string::npos) continue;
        senderName = p.name;
        return ps;
    }
    senderName.clear();
    return nullptr;
}

// The server-wide message multicast: same replication mechanism as chat, but the signature carries
// no PlayerState, so there is nothing to dereference.
bool GameStateServerMessage(const std::string& body, std::string& err) {
    void* gs = GameStateOf(FindWorld());
    auto fn = Fn<FnGsString>("AVeinGameStateBase::NetMulticast_BroadcastServerMessage");
    if (!gs || !fn) {
        err = "AVeinGameStateBase::NetMulticast_BroadcastServerMessage unavailable";
        return false;
    }
    // The game state pointer is validated by a class-chain walk before a game function is called on
    // it: during the first ticks after a restart `World.GameState` can be a half-built object.
    void* gsCls = VeinClass(nullptr, "VeinGameStateBase");
    if (gsCls && !Reflect::IsA(gs, gsCls)) {
        err = "the world's GameState is not an AVeinGameStateBase yet";
        return false;
    }
    GameFString msg(body);
    if (!msg.ok) {
        err = "could not allocate the message body";
        return false;
    }
    state::MarkInjectedMessage(body);
    fn(gs, &msg.fs);
    return true;
}

// Chat, with a real sender. `body` is already rendered with the `[sender]` prefix, so the line
// shows up attributed to whichever player lent their PlayerState - see docs/actions-design.md.
bool ChatWithSender(void* senderPlayerState, const std::string& body, std::string& err) {
    if (!senderPlayerState) {  // belt and braces: this is the exact call that crashed the server
        err = "no sender available";
        return false;
    }
    void* gs = GameStateOf(FindWorld());
    auto send = Fn<FnSendChat>("AVeinGameStateBase::NetMulticast_SendChat");
    if (!gs || !send) {
        err = "AVeinGameStateBase::NetMulticast_SendChat unavailable";
        return false;
    }
    GameFString msg(body);
    if (!msg.ok) {
        err = "could not allocate the message body";
        return false;
    }
    state::MarkInjectedMessage(body);
    send(gs, senderPlayerState, &msg.fs, ChatSegment(), nullptr, FVec3{});
    return true;
}

// A single recipient: the multicast cannot be targeted, so a whisper is delivered as that player's
// own on-screen notification instead.
bool NotifyPlayer(const PlayerInfo& p, const std::string& body, std::string& err) {
    auto notify = Fn<FnClientNotify>("AVeinPlayerController::Client_SendNotification");
    if (!notify || !p.controller) {
        err = "AVeinPlayerController::Client_SendNotification unavailable";
        return false;
    }
    GameFText text(body);
    if (!text.ok) {
        err = "FText::FromString unavailable";
        return false;
    }
    // enum class ENotificationType : uint8 { Info, Warning, Danger, Stat, Multiplayer, Max }
    // A server-to-player message is Multiplayer.
    enum : int { kNotifyMultiplayer = 4 };
    state::MarkInjectedMessage(body);
    notify(p.controller, text.bytes, (int)kNotifyMultiplayer, 8.0f);
    return true;
}

// The server-wide banner the admin panel uses. Complements chat; not a replacement for it.
bool ServerMessage(const std::string& body, std::string& err) {
    std::string why;
    void* admin = FindOwnedAdminComponent(why);  // NOT FindAdminComponent: the CDO crashes here
    auto fn = Fn<FnAdminString>("UAdminComponent::Server_SendServerMessage");
    if (!admin || !fn) {
        err = why.empty() ? "UAdminComponent::Server_SendServerMessage unavailable" : why;
        return false;
    }
    GameFString msg(body);
    if (!msg.ok) {
        err = "could not allocate the message body";
        return false;
    }
    state::MarkInjectedMessage(body);
    fn(admin, &msg.fs);
    return true;
}

// The broadcast entry point. Picks a path with the pure chooser (so the choice is unit-tested),
// then falls forward through the remaining safe paths if the chosen one fails at call time.
// `outVia` names the symbol that actually delivered.
bool Broadcast(const std::string& text, const std::string& senderName, std::string& outBody, std::string& outVia,
               std::string& err) {
    std::string chatSenderName;
    void* chatSender = FindChatSender(chatSenderName);

    BroadcastCaps caps;
    caps.gameStateServerMsg = Resolve::Addr("AVeinGameStateBase::NetMulticast_BroadcastServerMessage") != 0 &&
                              GameStateOf(FindWorld()) != nullptr;
    // Only when a real, owned component exists: the implementation dereferences its owner, so the
    // class default object is a SIGSEGV, not a degraded path (crash of 2026-09-17 08:07:00Z).
    std::string adminWhy;
    caps.adminServerMsg =
        Resolve::Addr("UAdminComponent::Server_SendServerMessage") != 0 && FindOwnedAdminComponent(adminWhy) != nullptr;
    caps.sendChat = Resolve::Addr("AVeinGameStateBase::NetMulticast_SendChat") != 0;
    caps.haveSender = chatSender != nullptr;

    std::string chooseErr;
    BroadcastPath first = ChooseBroadcastPath(ConfigValue("TAKARO_BROADCAST_VIA", "broadcastVia", ""), caps, chooseErr);
    if (first == BroadcastPath::kNone) {
        err = chooseErr;
        return false;
    }

    // Try the chosen path first, then the other safe ones. kChatWithSender is only ever in this
    // list when a validated, non-null AVeinPlayerState was found above.
    std::vector<BroadcastPath> order{first};
    auto push = [&](BroadcastPath p, bool usable) {
        if (!usable || p == first) return;
        order.push_back(p);
    };
    push(BroadcastPath::kGameStateServerMsg, caps.gameStateServerMsg);
    push(BroadcastPath::kAdminServerMsg, caps.adminServerMsg);
    push(BroadcastPath::kChatWithSender, caps.sendChat && caps.haveSender);

    std::string lastErr;
    for (BroadcastPath p : order) {
        std::string e;
        bool ok = false;
        std::string body = RenderMessage(senderName, text);
        switch (p) {
            case BroadcastPath::kGameStateServerMsg:
                ok = GameStateServerMessage(body, e);
                break;
            case BroadcastPath::kAdminServerMsg:
                ok = ServerMessage(body, e);
                break;
            case BroadcastPath::kChatWithSender:
                if (!chatSender) {  // unreachable by construction; the guard the crash report asked for
                    e = "no sender available";
                    break;
                }
                ok = ChatWithSender(chatSender, body, e);
                break;
            case BroadcastPath::kNone:
            default:
                e = "no broadcast path";
                break;
        }
        if (ok) {
            outBody = body;
            outVia = BroadcastPathSymbol(p);
            if (p == BroadcastPath::kChatWithSender) outVia = std::string(outVia) + " (sender: " + chatSenderName + ")";
            return true;
        }
        lastErr = e;
        PluginLog("message: %s failed (%s)", BroadcastPathSymbol(p), e.c_str());
    }
    err = lastErr.empty() ? chooseErr : lastErr;
    return false;
}

// ================================================================================================
// LANE L3e - verification: never report success without the effect
//
// L6b proved that `kickPlayer`, `giveItem` and `teleportPlayer` answered `{"success":true}` while
// nothing at all happened in the world: every `UAdminComponent::Server_*_Implementation` opens with
// an owner-side `AVeinPlayerController::IsAdmin()` check, and the component this plugin handed it
// had no owning controller, so the RPC returned void having done nothing. A void return is not an
// answer, so from here on **every mutating endpoint observes its own effect** and reports what it
// observed:
//
//   200  the effect was observed          {"success":true, "verified":true,  "via":"<mechanism>"}
//   409  the call ran and nothing changed {"success":false,"verified":false, "via":…, "attempted":[…]}
//   503  the game thread never answered   {"success":false,"error":…}
//
// The mechanisms are tried in order, cheapest-and-ungated first, and each one is *verified before
// the next is tried*, so `via` always names the mechanism that actually produced the effect.

// Runs `probe` on the game thread repeatedly until it returns true or `timeoutMs` elapses. Each
// probe is its own short game-thread job: the game thread is never held for the whole window.
bool PollGameThread(std::function<bool()> probe, uint32_t timeoutMs, uint32_t stepMs = 250) {
    uint32_t waited = 0;
    for (;;) {
        auto hit = std::make_shared<bool>(false);
        if (!GameThread::Run([probe, hit] { *hit = probe(); }, 5000)) return false;
        if (*hit) return true;
        if (waited >= timeoutMs) return false;
        struct timespec ts {
            (time_t)(stepMs / 1000), (long)(stepMs % 1000) * 1000000L
        };
        nanosleep(&ts, nullptr);
        waited += stepMs;
    }
}

// The list of mechanisms an action tried, as a JSON array, for the 409 body. The body lives in
// actions_util.cpp so the unit tests exercise the shipped code.
std::string AttemptedJson(const std::vector<std::string>& v) { return ActionsUtil::JsonStrArray(v); }

// ---------------------------------------------------------------------------------------------
// LANE L3e - generic UFunction call by name, with parameters matched by property *type*
//
// Nothing here hard-codes a parameter offset or a struct layout: the UFunction's own
// ChildProperties list gives every parameter's name, type and offset, and the parameter block is
// UStruct::PropertiesSize bytes. This is how `/give` reaches the inventory component's own
// add-item function without going anywhere near the admin RPC.

// The UFunction's parameter properties, in declaration order.
void WalkFuncProps(void* func, std::vector<void*>& out, int max = 24) {
    if (!func) return;
    void* prop = ReadPtrAt(func, (int32_t)Reflect::Lay().structChildProperties);
    for (int i = 0; prop && i < max; i++) {
        out.push_back(prop);
        prop = ReadPtrAt(prop, (int32_t)Reflect::Lay().fieldNext);
    }
}

int32_t FuncParmsSize(void* func) {
    const void* p = (const char*)func + Reflect::Lay().structPropertiesSize;
    if (!MemReadable(p, 4)) return -1;
    uint32_t sz = *(const uint32_t*)p;
    return sz <= 4096 ? (int32_t)sz : -1;
}

// LANE L3e - giving an item the way the game gives one.
//
// There is NO add-by-class-and-count function anywhere in this binary (checked: every
// `Z_Construct_UFunction_UBaseInventoryComponent_*` and every defined
// `UBaseInventoryComponent::Add|Try|Give|Create*`). `UBaseInventoryComponent::AddItem` takes an
// already-built `FVirtualItemInstance&`, so `Server_GiveItem_Implementation` builds one:
//
//     FVirtualItemInstance inst = FVirtualItemInstance::FromItem(itemClass);
//     inst.SetStack(n);                                   // n split by the item CDO's MaxStack
//     character->Inventory->AddItem(inst, /*bNoStackCombine*/ false, /*bNoWeightChecks*/ true, 0);
//     inst.~FVirtualItemInstance();
//
// That is exactly what this does, minus the admin check that made the RPC a no-op. `FromItem`
// returns a 168-byte struct, so it returns through the hidden sret pointer in RDI; the instance is
// destroyed after every AddItem, because the game destroys it too and it owns an FString.
struct GiveChain {
    FnFromItem fromItem = nullptr;
    FnSetStack setStack = nullptr;
    FnAddItem addItem = nullptr;
    FnItemDtor dtor = nullptr;
    size_t instSize = 0;
    bool ok() const { return fromItem && addItem && instSize >= 32; }
};

GiveChain ResolveGiveChain(std::string& why) {
    GiveChain c;
    c.fromItem = Fn<FnFromItem>("FVirtualItemInstance::FromItem");
    c.setStack = Fn<FnSetStack>("FVirtualItemInstance::SetStack");
    c.addItem = Fn<FnAddItem>("UBaseInventoryComponent::AddItem");
    c.dtor = Fn<FnItemDtor>("FVirtualItemInstance::~FVirtualItemInstance");
    // The instance size comes from the live reflected script struct, never from a constant.
    VirtualItemLayout lay = ReadVirtualItemLayout();
    c.instSize = lay.stride;
    if (!c.fromItem) why = "FVirtualItemInstance::FromItem unresolved";
    else if (!c.addItem) why = "UBaseInventoryComponent::AddItem unresolved";
    else if (c.instSize < 32) why = "FVirtualItemInstance is not reflectable, so its size is unknown";
    return c;
}

// The item class's own stack limit, read by reflection off its class default object. 1 when the
// item is not stackable or the properties are not there - the loop below then adds one per call,
// which is slower but never wrong.
int MaxStackOf(void* itemClass) {
    void* cdo = Reflect::ClassDefaultObject(itemClass);
    if (!cdo) return 1;
    int32_t stackableOff = OffAny(cdo, {"bStackable", "Stackable"});
    if (stackableOff >= 0 && MemReadable((const char*)cdo + stackableOff, 1) &&
        *(const uint8_t*)((const char*)cdo + stackableOff) == 0)
        return 1;
    int32_t maxOff = OffAny(cdo, {"MaxStack", "MaxStackSize"});
    if (maxOff < 0 || !MemReadable((const char*)cdo + maxOff, 4)) return 1;
    int32_t v = *(const int32_t*)((const char*)cdo + maxOff);
    return v > 0 && v <= 100000 ? v : 1;
}

// One AddItem call with a freshly built instance of `count` units. Returns AddItem's own byte
// result; the caller does not trust it - the inventory is counted instead.
uint8_t AddOneStack(const GiveChain& c, void* inv, void* itemClass, int count) {
    // Generously oversized and zeroed: FromItem constructs into it, and anything the struct grew in
    // a later build still lands inside the buffer.
    std::vector<char> inst(c.instSize + 128, 0);
    // &itemClass, not itemClass: TSubclassOf is passed by invisible reference (see FnFromItem).
    c.fromItem(inst.data(), &itemClass);
    // ALWAYS, even for a single unit: a default-constructed FVirtualItemInstance has Stack 0, and
    // Server_GiveItem sets the stack on every instance it builds.
    if (c.setStack) c.setStack(inst.data(), count);
    uint8_t result = c.addItem(inv, inst.data(), false, true, 0);
    if (c.dtor) c.dtor(inst.data());
    return result;
}

// The inventory component the game itself gives into: AVeinCharacter::Inventory on the pawn
// (`mov 0x978(%r12),%r14` in Server_GiveItem). Looked up by property name, never by that offset;
// the component scan is the fallback.
void* GiveTargetInventory(const PlayerInfo& p) {
    // LANE L3f: the same single component the inventory is read from. The old fallback to the
    // first component of a pawn+controller sweep could give into a corpse or a cached character -
    // a give that "succeeded" into a container the player will never open.
    std::string why;
    return PawnInventory(p.pawn, why);
}

// ---------------------------------------------------------------------------------------------
// shutdown

std::atomic<bool> g_shutdownRequested{false};

void* ShutdownThread(void*) {
    // The HTTP response is already on the wire; give it a moment, then save and leave.
    struct timespec half {
        0, 500 * 1000 * 1000
    };
    nanosleep(&half, nullptr);
    auto saveRequested = std::make_shared<bool>(false);
    bool saveCompleted = GameThread::Run(
        [saveRequested] {
            std::string why;
            void* admin = FindAdminComponent(why);
            auto save = Fn<FnAdminVoid>("UAdminComponent::Server_RequestDedicatedServerSave");
            if (!admin || !save) {
                PluginLog("shutdown: no save path (%s)", why.empty() ? "symbol unresolved" : why.c_str());
                return;
            }
            save(admin);
            *saveRequested = true;
        },
        5000);
    PluginLog("shutdown: saveRequested=%d", (int)(saveCompleted && *saveRequested));
    if (saveCompleted && *saveRequested) {
        for (int i = 0; i < 20; i++) {
            struct timespec ts {
                0, 500 * 1000 * 1000
            };
            nanosleep(&ts, nullptr);
        }
    }
    PluginLog("shutdown: sending SIGTERM to pid %d", getpid());
    kill(getpid(), SIGTERM);
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// kick / ban of an online player

// Disconnects an online player. `ban` picks the ban flavour, which shows the client the game's own
// "you are banned" screen instead of the kick one.
//
// LANE L3e - order and verification, both changed.
//
// ORDER. The admin RPC used to be tried first. It is now **last**: L6b proved
// `UAdminComponent::Server_KickPlayer_Implementation` is an owner-side-`IsAdmin()`-gated no-op
// whenever the component's owner is not a live admin controller, and it returns void either way, so
// putting it first meant the working engine path was never reached. `AGameSession::KickPlayer`
// (`ClientWasKicked` + `Destroy()` on the controller) is stock engine code with no admin gate, so
// it goes first.
//
// VERIFICATION. This function only *makes the call*; whether the player actually left is decided by
// the caller polling the player table (see Actions::Kick). It reports which mechanism it used in
// `via` so the caller can attribute the effect, and it never claims success by itself.
struct SessionAttempt {
    bool called = false;     // a mechanism actually ran
    std::string via;         // which one
    std::string err;         // why nothing ran
    std::string gameId;
};

// One mechanism, by index. 0 = the engine session, 1 = a client-side return-to-menu RPC,
// 2 = VEIN's admin component (owner-gated; only useful with an admin online).
SessionAttempt KickMechanism(int which, const PlayerInfo& p, const std::string& reason, bool ban) {
    SessionAttempt a;
    a.gameId = p.gameId;
    switch (which) {
        case 0: {
            void* gm = GameModeOf(FindWorld());
            void* session = gm ? ReadPtrAt(gm, Off(gm, "GameSession")) : nullptr;
            auto fn = Fn<FnKickBan>(ban ? "AGameSession::BanPlayer" : "AGameSession::KickPlayer");
            if (!session || !fn || !p.controller) {
                a.err = "AGameSession path unavailable (no session, no controller or symbol unresolved)";
                return a;
            }
            GameFText text(reason.empty() ? std::string(ban ? "Banned" : "Kicked") : reason);
            if (!text.ok) {
                a.err = "FText::FromString unavailable";
                return a;
            }
            fn(session, p.controller, text.bytes);
            a.called = true;
            a.via = ban ? "AGameSession::BanPlayer" : "AGameSession::KickPlayer";
            return a;
        }
        case 1: {
            // What Server_KickPlayer_Implementation does *after* its admin check, verbatim from the
            // disassembly: Destroy() the pawn, tell the client with
            // AVeinPlayerController::Client_Kicked(), Destroy() the controller. No gate.
            auto kicked = Fn<FnClientKicked>("AVeinPlayerController::Client_Kicked");
            auto destroy = Fn<FnDestroy>("AActor::Destroy");
            if (p.controller && kicked && destroy) {
                if (p.pawn) destroy(p.pawn, false, true);
                kicked(p.controller);
                destroy(p.controller, false, true);
                a.called = true;
                a.via = "AVeinPlayerController::Client_Kicked + AActor::Destroy (the ungated body of "
                        "Server_KickPlayer)";
                return a;
            }
            // Engine fallback: the same "go back to the menu, here is why" the client understands.
            auto fn = Fn<FnPcText>("APlayerController::ClientReturnToMainMenuWithTextReason");
            if (!fn || !p.controller) {
                a.err = "neither AVeinPlayerController::Client_Kicked nor "
                        "APlayerController::ClientReturnToMainMenuWithTextReason is available";
                return a;
            }
            GameFText text(reason.empty() ? std::string("Kicked") : reason);
            if (!text.ok) {
                a.err = "FText::FromString unavailable";
                return a;
            }
            fn(p.controller, text.bytes);
            a.called = true;
            a.via = "APlayerController::ClientReturnToMainMenuWithTextReason";
            return a;
        }
        default: {
            // Owner-gated, so only ever an owned component - never the CDO (that one SIGSEGVs).
            std::string why;
            void* admin = FindOwnedAdminComponent(why);
            if (!admin) {
                a.err = why;
                return a;
            }
            if (ban) {
                auto fn = Fn<FnAdminBan>("UAdminComponent::Server_BanPlayer");
                if (!fn) {
                    a.err = "UAdminComponent::Server_BanPlayer unresolved";
                    return a;
                }
                GameFString r(reason.empty() ? std::string("Banned") : reason);
                if (!r.ok) {
                    a.err = "FMemory::Malloc unavailable";
                    return a;
                }
                fn(admin, p.playerState, &r.fs);
                a.via = "UAdminComponent::Server_BanPlayer (requires an admin online)";
            } else {
                auto fn = Fn<FnAdminPlayerState>("UAdminComponent::Server_KickPlayer");
                if (!fn) {
                    a.err = "UAdminComponent::Server_KickPlayer unresolved";
                    return a;
                }
                fn(admin, p.playerState);
                a.via = "UAdminComponent::Server_KickPlayer (requires an admin online)";
            }
            a.called = true;
            return a;
        }
    }
}

const int kKickMechanisms = 3;

// True when nobody with this id is in the player table any more - the only honest definition of
// "kicked".
bool PlayerGone(const std::string& id) {
    PlayerInfo p;
    return !FindPlayerById(id, p);
}

std::string BodyString(const JsonValue& body, const char* key) {
    const JsonValue* v = body.get(key);
    return v && v->isStr() ? v->str : "";
}

}  // namespace

// Lane L2c. Thin wrappers: the work is in the anonymous namespace above, next to the item
// catalogue and the FVirtualItemInstance layout it reuses.
std::string Actions::EquippedItemName(void* actor) { return EquippedItemNameImpl(actor); }

// ================================================================================================
// lifecycle

void Actions::Init() {
    state::BansLoad();

    // "ok" here means wired and every symbol it needs resolved.
    //
    // LANE L3e: the four read-only capabilities below used to boot "degraded" purely because they
    // had not been exercised yet. The sidecar refuses any action whose capability is not ok and
    // Takaro's testReachability then reports "capabilities not ok", so endpoints that work
    // perfectly were failing the rig check until something happened to call them. They boot "ok"
    // now; every handler still writes the real status, with the real reason, on every call.
    const char* kUnverified = " UNVERIFIED: wired but never executed in the game process (M2/M3).";

    SetCap("players", "ok",
           std::string("gameId is the SteamID64, read from a replicated id property, else from the "
                       "FUniqueNetIdSteam bit pattern in APlayerState::UniqueID, else from "
                       "AVeinPlayerState::GetPlayerUniqueID().") +
               kUnverified);
    SetCap("playerLocation", "ok", std::string("the pawn's root component transform.") + kUnverified);
    SetCap("playerInventory", "ok",
           std::string("the UItem objects outered to each UBaseInventoryComponent under the pawn and the "
                       "controller. Reflection only - no call into the game - but whether VEIN outers its items "
                       "to the component is unproven, so this may come back empty.") +
               kUnverified);
    SetCap("listItems", "degraded", "item catalogue not built yet (built on the first world tick)");
    SetCap("listEntities", "ok",
           std::string("the AI character classes loaded so far (AVeinZombieCharacter / AVeinAnimalCharacter "
                       "subclasses). VEIN streams its AI content, so this is not the full bestiary.") +
               kUnverified);
    SetCap("listLocations", "ok",
           std::string("the ALocationMarker actors currently streamed in; the world is partitioned, so this is "
                       "not every point of interest.") +
               kUnverified);

    // LANE L3e: the admin RPCs are the LAST mechanism of each of these, not the first, and none of
    // them is "ok" on the strength of a resolved symbol alone any more - the handler re-reports the
    // capability from what it actually observed on every call.
    bool give = Resolve::Addr("FVirtualItemInstance::FromItem") && Resolve::Addr("UBaseInventoryComponent::AddItem");
    SetCap("giveItem", give ? "ok" : (Resolve::Addr("UAdminComponent::Server_GiveItem") ? "degraded" : "unimplemented"),
           give ? "FVirtualItemInstance::FromItem + SetStack + UBaseInventoryComponent::AddItem on the pawn's own "
                  "Inventory component - what Server_GiveItem does after its admin check, without the check. "
                  "Falls back to UAdminComponent::Server_GiveItem on an owned admin component (requires an admin "
                  "online). Verified by the inventory stack count for `code` going up; never reported as a success "
                  "otherwise."
                : "FVirtualItemInstance::FromItem / UBaseInventoryComponent::AddItem unresolved; only the "
                  "owner-gated admin RPC is left, which needs an admin online");

    bool bcast = Resolve::Addr("AVeinGameStateBase::NetMulticast_BroadcastServerMessage") != 0 ||
                 Resolve::Addr("UAdminComponent::Server_SendServerMessage") != 0;
    bool notify = Resolve::Addr("AVeinPlayerController::Client_SendNotification") != 0;
    SetCap("sendMessage", bcast ? "ok" : (notify ? "degraded" : "unimplemented"),
           std::string(bcast ? "a broadcast goes through AVeinGameStateBase::NetMulticast_BroadcastServerMessage "
                               "(the sender-less multicast), falling back to "
                               "UAdminComponent::Server_SendServerMessage, prefixed with [sender]; "
                               "NetMulticast_SendChat is only used when a real AVeinPlayerState sender exists "
                               "(it null-derefs its sender otherwise); "
                             : "no sender-less broadcast path resolved; ") +
               (notify ? "a message with a recipientGameId is delivered as that player's own on-screen "
                         "notification, because the multicast cannot be targeted."
                       : "targeted messages are unavailable (Client_SendNotification unresolved).") +
               kUnverified);

    bool tp = Resolve::Addr("UAdminComponent::MovePlayer") || Resolve::Addr("AActor::TeleportTo") ||
              Resolve::Addr("AActor::SetActorLocation");
    SetCap("teleport", tp ? "ok" : "unimplemented",
           tp ? "UAdminComponent::MovePlayer(APlayerState*, FVector) - the ungated body Server_MovePlayer "
                "tail-jumps to, which also sends AVeinCharacter::Client_StartTeleport so the client is told - "
                "falling back to AActor::TeleportTo, then K2_TeleportTo/SetActorLocation. A named `target` is "
                "resolved against the streamed-in ALocationMarker actors. Verified by reading the pawn's location "
                "back: it must have moved AND be within 500 cm of the target."
              : "no teleport primitive resolved");

    bool kick = Resolve::Addr("AGameSession::KickPlayer") ||
                (Resolve::Addr("AVeinPlayerController::Client_Kicked") && Resolve::Addr("AActor::Destroy"));
    SetCap("kick", kick ? "ok" : (Resolve::Addr("UAdminComponent::Server_KickPlayer") ? "degraded" : "unimplemented"),
           kick ? "AGameSession::KickPlayer(PC, FText) (which carries the reason), falling back to "
                  "AVeinPlayerController::Client_Kicked + AActor::Destroy - the ungated body of "
                  "Server_KickPlayer - then to the admin RPC on an owned component (requires an admin online). "
                  "Verified by the player leaving GameState.PlayerArray within 5 s; never reported as a success "
                  "otherwise."
                : "no ungated kick path resolved");

    bool banList = Resolve::Addr("AVeinGameStateBase::BanID") != 0;
    SetCap("ban", banList ? "ok" : "degraded",
           std::string(banList
                           ? "VEIN has its own id-keyed ban list: AVeinGameStateBase::BanID(FString id, FString "
                             "reason) accepts a player the server has never seen, so an offline ban works. An "
                             "online player is additionally disconnected through UAdminComponent::Server_BanPlayer. "
                             "The plugin list (takaro/bans.json) is written too: it carries the reason and the "
                             "expiry, and lane L2's PreLogin hook refuses a rejoin with it."
                           : "AVeinGameStateBase::BanID unresolved; only the plugin ban list is written") +
               kUnverified);
    SetCap("unban", Resolve::Addr("AVeinGameStateBase::UnbanID") ? "ok" : "degraded",
           std::string(Resolve::Addr("AVeinGameStateBase::UnbanID")
                           ? "AVeinGameStateBase::UnbanID plus the plugin list."
                           : "AVeinGameStateBase::UnbanID unresolved; only the plugin list is cleared") +
               kUnverified);
    SetCap("listBans", "ok",
           std::string("the union of VEIN's own replicated TArray<FBan> on the game state (read by reflection; "
                       "FBan's field names are not known offline) and the plugin ban list, which owns the reason, "
                       "createdAt and expiresAt.") +
               kUnverified);

    SetCap("executeCommand", "ok",
           std::string("plugin command set (help, players, bans, items, entities, locations, say, whisper, give, "
                       "tp, kick, ban, unban, save, shutdown) plus `raw <cmd>` through UEngine::Exec with a "
                       "captured output device and `vein <cmd>` through UAdminComponent::Server_Exec. `cheat` is "
                       "501: the dedicated server creates no CheatManager.") +
               kUnverified);

    bool save = Resolve::Addr("UAdminComponent::Server_RequestDedicatedServerSave") != 0;
    SetCap("shutdown", save ? "ok" : "degraded",
           std::string(save ? "UAdminComponent::Server_RequestDedicatedServerSave, then SIGTERM to our own pid."
                            : "no save call resolved; SIGTERM without saving") +
               kUnverified);

    PluginLog("actions: lane L3 wired (%s)",
              give ? "admin component available" : "admin component give path unresolved");
}

void Actions::Housekeep() {
    // Also flush mutations that finished after a caller timed out.
    state::FlushBans();
    FlushPluginLogs();
    {
        Guard g(g_itemLock);
        if (g_itemsBuilt) return;
    }
    if (GameThread::TickCount() == 0) return;
    auto built = std::make_shared<bool>(false);
    bool completed = GameThread::Run([built] { Perf::Scope sc("catalogue.items"); *built = BuildItems(); }, 5000);
    if (completed && *built) {
        Guard g(g_itemLock);
        SetCap("listItems", "ok",
               "every loaded non-abstract UClass deriving from UItem (" + std::to_string(g_items.size()) +
                   " on this boot, " + std::to_string(g_itemsAbstractSkipped) +
                   " abstract classes skipped, filter " + g_itemAbstractFilter + "); " +
                   std::to_string(g_itemsNamed) + " have a UItem::Name display text, " +
                   std::to_string(g_itemsDescribed) + " a Description, " + std::to_string(g_itemsCategorised) +
                   " a UItemType category; the rest fall back to the humanised class name" +
                   (g_itemNamesNative ? " (UItem::GetDefaultNameInvariant enabled as a last resort)" : ""));
        PluginLog("actions: item catalogue built (%zu UItem classes; %zu named, %zu described, %zu categorised, "
              "%zu abstract skipped)",
              g_items.size(), g_itemsNamed, g_itemsDescribed, g_itemsCategorised, g_itemsAbstractSkipped);
    }
}

// ================================================================================================
// read-only handlers

// LANE L9: all three read endpoints are answered from the snapshot. The JSON is assembled on the
// HTTP thread; the game thread is entered only when the snapshot has aged out.
const PlayerRow* FindRow(const std::vector<PlayerRow>& rows, const std::string& id) {
    std::string needle = NormalizeGameId(id);
    for (auto& r : rows)
        if (r.gameId == needle || Lower(r.name) == Lower(id)) return &r;
    return nullptr;
}

Actions::Result Actions::Players() {
    std::vector<PlayerRow> rows;
    uint64_t age = 0;
    if (!PlayerRows(rows, age)) return Fail(503, "game thread unavailable");
    std::string o = "[";
    for (size_t i = 0; i < rows.size(); i++) o += (i ? "," : "") + PlayerRowJson(rows[i]);
    return {200, o + "]"};
}

Actions::Result Actions::Player(const std::string& gameId) {
    std::vector<PlayerRow> rows;
    uint64_t age = 0;
    if (!PlayerRows(rows, age)) return Fail(503, "game thread unavailable");
    const PlayerRow* r = FindRow(rows, gameId);
    if (!r) return Fail(404, "player not online");
    return {200, PlayerRowJson(*r)};
}

Actions::Result Actions::PlayerLocation(const std::string& gameId) {
    std::vector<PlayerRow> rows;
    uint64_t age = 0;
    if (!PlayerRows(rows, age)) return Fail(503, "game thread unavailable");
    const PlayerRow* r = FindRow(rows, gameId);
    if (!r) return Fail(404, "player not online");
    if (!r->hasPawn) return Fail(503, "player has no pawn yet");
    if (!r->hasLoc) return Fail(503, "pawn has no readable root component");
    return {200, "{\"x\":" + JsonNum(r->loc[0]) + ",\"y\":" + JsonNum(r->loc[1]) + ",\"z\":" +
                     JsonNum(r->loc[2]) + ",\"yaw\":" + JsonNum(r->rot[1]) + ",\"pitch\":" +
                     JsonNum(r->rot[0]) + ",\"ageMs\":" + std::to_string(age) + "}"};
}

Actions::Result Actions::PlayerInventory(const std::string& gameId) {
    return OnGameThread("GET /players/{id}/inventory", [gameId]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(gameId, p)) return JobOut::Error(404, "player not online");
        // LANE L3f: no current character means there is no inventory to report. Answering `[]`
        // here would be a lie that looks like "the player is carrying nothing"; answering with
        // some other container's items - which is what the old sweep did - is worse. 404 is the
        // honest answer, and it is the answer a player on the character screen or a player who is
        // dead and has not respawned gets.
        if (!p.spawned)
            return JobOut::Error(404, std::string("no character: ") +
                                 (p.pawn ? "the player's current pawn is " + Reflect::ClassName(p.pawn) +
                                           ", not a character"
                                         : "the player has no pawn possessed"));
        std::string detail;
        auto items = ReadInventory(p, detail);
        if (items.empty() && !detail.empty()) {
            SetCap("playerInventory", "degraded", detail);
            return {200, "[]"};
        }
        SetCap("playerInventory", "ok", "");
        return {200, [items = std::move(items)] {
        std::string o = "[";
        for (size_t i = 0; i < items.size(); i++) {
            if (i) o += ",";
            o += "{\"code\":" + JsonStr(items[i].code) + ",\"name\":" + JsonStr(items[i].name) +
                 ",\"amount\":" + std::to_string(items[i].amount) + ",\"inventory\":" + JsonStr(items[i].inventory) +
                 ",\"slot\":" + std::to_string(items[i].slot) + ",\"assetPath\":" +
                 JsonStr(items[i].path) + "}";
        }
        return o + "]";
        }};
    });
}

// LANE L3f / finding F19: the diagnosis endpoint. Read-only, debug-gated.
Actions::Result Actions::DebugInventories(const std::string& gameId) {
    struct ObjectView {
        bool valid = false;
        std::string address, cls, name;
        std::string Json() const {
            return valid ? "{\"ptr\":" + JsonStr(address) + ",\"class\":" + JsonStr(cls) +
                           ",\"name\":" + JsonStr(name) + "}" : "null";
        }
    };
    struct Entry { int index, stack; std::string code; };
    struct Entries {
        int total = -1;
        std::vector<Entry> rows;
        std::string Json() const {
            std::string out = "[";
            for (const auto& e : rows) {
                if (out.size() > 1) out += ",";
                out += "{\"i\":" + std::to_string(e.index) + ",\"code\":" + JsonStr(e.code) +
                       ",\"stack\":" + std::to_string(e.stack) + "}";
            }
            return out + "]";
        }
    };
    struct Sweep {
        ObjectView component, owner;
        Entries entries;
        bool ownerCurrent, ownerState, classAccepted, accepted;
    };
    return OnGameThread("GET /debug/inventories", [gameId]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(gameId, p)) return JobOut::Error(404, "player not online");
        VirtualItemLayout lay = ReadVirtualItemLayout();
        auto describe = [](void* object) {
            ObjectView out;
            if (!object || !MemReadable(object, 0x40)) return out;
            char address[32]; snprintf(address, sizeof address, "%p", object);
            out.valid = true; out.address = address;
            out.cls = Reflect::ClassName(object); out.name = Reflect::ObjName(object);
            return out;
        };
        auto entries = [&lay](void* c) {
            Entries out;
            if (!c || !lay.ok()) return out;
            int32_t off = Off(c, "Items");
            if (off < 0) return out;
            off += lay.innerArray;
            if (!MemReadable((const char*)c + off, 16)) return out;
            TArray<char> arr{}; memcpy(&arr, (const char*)c + off, sizeof arr);
            if (arr.Num < 0 || arr.Num > 8192) return out;
            if (arr.Num && !MemReadable(arr.Data, (size_t)arr.Num * lay.stride)) return out;
            out.total = arr.Num;
            for (int32_t i = 0; i < arr.Num && i < 64; ++i) {
                const char* e = arr.Data + (size_t)i * lay.stride;
                int32_t stack = 0;
                if (lay.stack >= 0 && MemReadable(e + lay.stack, 4)) memcpy(&stack, e + lay.stack, 4);
                out.rows.push_back({i, stack, ActionsUtil::ItemCodeFromSoftPath(SoftClassName(e + lay.item))});
            }
            return out;
        };
        std::string why;
        void* chosen = PawnInventory(p.pawn, why);
        auto controller = describe(p.controller), pawn = describe(p.pawn), statePawn = describe(p.statePawn);
        auto chosenView = describe(chosen);
        auto chosenEntries = entries(chosen);
        bool agree = p.pawn == p.statePawn, spawned = p.spawned, haveChosen = chosen != nullptr;
        std::vector<Sweep> sweep;
        for (void* c : FindInventoryComponents(p)) {
            void* owner = Reflect::ObjOuter(c);
            sweep.push_back({describe(c), describe(owner), entries(c), owner && owner == p.pawn,
                             owner && owner == p.statePawn,
                             ActionsUtil::IsPlayerInventoryClass(Reflect::ClassName(c)), c == chosen});
        }
        // Only owned strings, numbers and vectors survive this job; no UObject pointers.
        return {200, [id = p.gameId, name = p.name, character = p.characterId, controller, pawn, statePawn,
                      chosenView, chosenEntries = std::move(chosenEntries), agree, spawned, haveChosen,
                      why, sweep = std::move(sweep)] {
            std::string out = "{\"gameId\":" + JsonStr(id) + ",\"name\":" + JsonStr(name) +
                ",\"characterId\":" + (character.empty() ? "null" : JsonStr(character)) +
                ",\"controller\":" + controller.Json() + ",\"controllerPawn\":" + pawn.Json() +
                ",\"playerStatePawn\":" + statePawn.Json() + ",\"pawnsAgree\":" + (agree ? "true" : "false") +
                ",\"spawned\":" + (spawned ? "true" : "false") + ",\"chosen\":" + chosenView.Json() +
                ",\"chosenEntries\":" + std::to_string(chosenEntries.total) +
                ",\"chosenItems\":" + chosenEntries.Json() +
                ",\"chosenRejectedBecause\":" + (haveChosen ? "null" : JsonStr(why)) + ",\"legacySweep\":[";
            bool first = true;
            for (const auto& row : sweep) {
                if (!first) out += ",";
                first = false;
                out += "{\"component\":" + row.component.Json() + ",\"owner\":" + row.owner.Json() +
                    ",\"ownerIsCurrentPawn\":" + (row.ownerCurrent ? "true" : "false") +
                    ",\"ownerIsPlayerStatePawn\":" + (row.ownerState ? "true" : "false") +
                    ",\"entries\":" + std::to_string(row.entries.total) + ",\"items\":" + row.entries.Json() +
                    ",\"classAccepted\":" + (row.classAccepted ? "true" : "false") +
                    ",\"acceptedNow\":" + (row.accepted ? "true" : "false") + "}";
            }
            return out + "]}";
        }};
    });
}

Actions::Result Actions::Items(const std::string& search) {
    bool built;
    {
        Guard g(g_itemLock);
        built = g_itemsBuilt;
    }
    if (!built) Housekeep();
    Guard g(g_itemLock);
    if (!g_itemsBuilt) return Fail(503, "item catalogue not loaded yet");
    std::string needle = Lower(search);
    std::string o = "[";
    bool first = true;
    for (auto& d : g_items) {
        if (!ActionsUtil::MatchesSearch({d.code, d.name}, needle)) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"code\":" + JsonStr(d.code) + ",\"name\":" + JsonStr(d.name) +
             ",\"description\":" + JsonStr(d.description) + ",\"category\":" + JsonStr(d.category) + "}";
    }
    return {200, o + "]"};
}

// LANE L9: /entities is a CATALOGUE - the set of AI classes this build can spawn - not live state.
// Building it walks every UClass in the object array, which is far too heavy to redo per request.
// It is built once, cached, and only rebuilt when the cache has aged out (streaming can introduce a
// new Blueprint class), which keeps a Takaro poll off the game thread entirely.
Mutex g_entityCacheLock;
std::string g_entityCache;
uint64_t g_entityCacheAtMs = 0;
const uint64_t kEntityCacheMs = 300000;  // 5 min

Actions::Result Actions::Entities() {
    {
        Guard g(g_entityCacheLock);
        if (g_entityCacheAtMs && NowMs() - g_entityCacheAtMs < kEntityCacheMs) {
            Perf::RecordSweep("catalogue.entities.cacheHit", 0);
            return {200, g_entityCache};
        }
    }
    Actions::Result r = OnGameThread("GET /entities", []() -> JobOut {
        void* classCls = Reflect::StaticClass("UClass::StaticClass");
        if (!classCls) classCls = Reflect::FindObjectByPath("/Script/CoreUObject", "Class");
        if (!classCls) return JobOut::Error(503, "UClass class not found");
        struct Root {
            const char* sym;
            const char* path;
            const char* type;
        };
        // Takaro's entity type vocabulary is hostile|friendly|neutral. Zombies are always hostile.
        // Animals split on AVeinAnimalCharacter::bIsPrey (DWARF): prey is neutral, a predator such
        // as BP_Wolf_C is hostile. `type` below is the default when the flag cannot be read.
        const Root roots[] = {
            {"AVeinZombieCharacter::StaticClass", "VeinZombieCharacter", "hostile"},
            {"AVeinAnimalCharacter::StaticClass", "VeinAnimalCharacter", "neutral"},
        };
        std::map<std::string, std::pair<std::string, std::string>> found;  // code -> {type, description}
        std::vector<void*> classes;
        bool haveAny = false;
        if (Reflect::GetObjectsOfClass(classCls, classes, true)) {
            for (const Root& r : roots) {
                void* base = VeinClass(r.sym, r.path);
                if (!base) continue;
                haveAny = true;
                // Lane L3c: the display name. AVeinAnimalCharacter carries an FText UsableName
                // (DWARF: @ 0x958); zombies have no name text at all on this build, so they fall
                // back to the humanised class name. Read off the CDO by reflection, no call.
                const int32_t usableOff = OffOf(base, "UsableName");
                const int32_t preyOff = OffOf(base, "bIsPrey");
                const bool abstractOk = ClassIsAbstract(base);  // every AI base class is abstract
                for (void* c : classes) {
                    if (!c || c == base || !DerivesFrom(c, base)) continue;
                    std::string code = Reflect::ObjName(c);
                    if (IsGeneratedArtefact(code)) continue;
                    // F14: an abstract class can never be spawned, so it is not an entity.
                    if (abstractOk && ClassIsAbstract(c)) continue;
                    if (found.count(code)) continue;
                    std::string type = r.type;
                    void* cdo = Reflect::ClassDefaultObject(c);
                    if (cdo && MemReadable(cdo, 0x40)) {
                        std::string label = TextToString(cdo, usableOff);
                        if (!label.empty()) state::NoteEntityName(code, label);
                        if (preyOff >= 0 && MemReadable((const char*)cdo + preyOff, 1))
                            type = *((const uint8_t*)cdo + preyOff) ? "neutral" : "hostile";
                    }
                    found[code] = {type, std::string(r.path) + " subclass"};
                }
                // The base classes themselves are abstract; they are listed only when the abstract
                // flag could not be read, so that /entities is never empty.
                std::string baseName = Reflect::ObjName(base);
                if (!abstractOk && !IsGeneratedArtefact(baseName) && !found.count(baseName))
                    found[baseName] = {r.type, "base AI character class"};
            }
        }
        if (!haveAny) return JobOut::Error(503, "no VEIN AI character class is loaded");
        SetCap("listEntities", "ok",
               "the AI character classes loaded so far (" + std::to_string(found.size()) +
                   "); VEIN streams its AI content, so this is not the full bestiary");
        return {200, [found = std::move(found)] {
        std::string o = "[";
        bool first = true;
        for (auto& kv : found) {
            if (!first) o += ",";
            first = false;
            std::string name = state::EntityName(kv.first);
            if (name.empty()) name = HumaniseCode(kv.first);
            o += "{\"code\":" + JsonStr(kv.first) + ",\"name\":" + JsonStr(name) + ",\"type\":" +
                 JsonStr(kv.second.first) + ",\"description\":" + JsonStr(kv.second.second) + "}";
        }
        return o + "]";
        }};
    });
    if (r.status == 200) {
        Guard g(g_entityCacheLock);
        g_entityCache = r.body;
        g_entityCacheAtMs = NowMs();
    }
    return r;
}

Actions::Result Actions::Locations() {
    return OnGameThread("GET /locations", []() -> JobOut {
        auto locations = ReadLocations();
        return {200, [locations = std::move(locations)] {
        std::string o = "[";
        bool first = true;
        for (const auto& l : locations) {
            if (!first) o += ",";
            first = false;
            o += "{\"code\":" + JsonStr(l.code) + ",\"name\":" + JsonStr(l.name) + ",\"position\":{\"x\":" +
                 JsonNum(l.x) + ",\"y\":" + JsonNum(l.y) + ",\"z\":" + JsonNum(l.z) + "}}";
        }
        return o + "]";
        }};
    });
}

Actions::Result Actions::Bans() {
    return OnGameThread("GET /bans", []() -> JobOut {
        std::string why;
        auto gameRows = ReadGameBans(why);
        return {200, [gameRows = std::move(gameRows), why] {
        std::map<std::string, GameBan> game;
        std::vector<std::string> order;
        for (const auto& b : gameRows) {
            if (!game.count(b.gameId)) order.push_back(b.gameId);
            game[b.gameId] = b;
        }
        std::map<std::string, state::BanRecord> plugin;
        for (auto& b : state::BanList()) {
            std::string id = NormalizeGameId(b.gameId);
            plugin[id] = b;
            if (!game.count(id) && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
        }
        SetCap("listBans", why.empty() ? "ok" : "degraded",
               why.empty() ? "VEIN's own ban list plus the plugin list (which owns reason/createdAt/expiresAt)"
                           : ("the game's own ban list could not be read (" + why +
                              "); only the plugin ban list is reported"));
        std::string o = "[";
        bool first = true;
        for (auto& id : order) {
            auto pit = plugin.find(id);
            bool inPlugin = pit != plugin.end();
            auto git = game.find(id);
            bool inGame = git != game.end();
            std::string name = inGame ? git->second.name : std::string();
            if (name.empty() && inPlugin) name = pit->second.name;
            std::string reason = inPlugin && !pit->second.reason.empty() ? pit->second.reason
                                 : inGame                                ? git->second.reason
                                                                         : std::string();
            if (!first) o += ",";
            first = false;
            o += "{\"gameId\":" + JsonStr(id) + ",\"name\":" + JsonStr(name) + ",\"reason\":" + JsonStr(reason) +
                 ",\"expiresAt\":" +
                 ((inPlugin && !pit->second.expiresAt.empty()) ? JsonStr(pit->second.expiresAt)
                                                               : std::string("null")) +
                 ",\"createdAt\":" +
                 ((inPlugin && !pit->second.createdAt.empty()) ? JsonStr(pit->second.createdAt)
                                                               : std::string("null")) +
                 ",\"enforcedBy\":" + JsonStr(inGame ? "game" : "plugin") + "}";
        }
        return o + "]";
        }};
    });
}

// ================================================================================================
// mutating handlers

Actions::Result Actions::Message(const JsonValue& body) {
    const JsonValue* textV = body.get("text");
    if (!textV || !textV->isStr() || textV->str.empty()) return Fail(400, "'text' is required");
    std::string text = textV->str;
    std::string recipient = BodyString(body, "recipientGameId");
    const JsonValue* snd = body.get("senderName");
    std::string sender = snd && snd->isStr() ? snd->str : ConfigValue("TAKARO_SENDER_NAME", "senderName", "Server");

    return OnGameThread("POST /message", [text, recipient, sender]() -> JobOut {
        if (recipient.empty()) {
            std::string err, rendered, via;
            if (!Broadcast(text, sender, rendered, via, err)) {
                SetCap("sendMessage", "degraded", err);
                return JobOut::Error(503, err);
            }
            SetCap("sendMessage", "ok", std::string("broadcast via ") + via);
            size_t n = ReadPlayers().size();
            // LANE L3e: /message is the ONE mutating endpoint whose effect is not server-observable
            // - a chat multicast leaves no state behind to read back, and whether a client rendered
            // it can only be seen on the client. So it does not claim `verified:true`; the 200 means
            // the multicast was dispatched on the game thread and the job ran to completion (L3d's
            // ack-after-effect), and `verifiedBy` says exactly that.
            return {200, [n, via] { return std::string("{\"success\":true,\"verified\":false,\"verifiedBy\":\"dispatch-only: a chat "
                         "multicast leaves no server-side state to read back\",\"delivered\":" +
                             std::to_string(n) + ",\"via\":" + JsonStr(via) + "}"); }};
        }
        PlayerInfo p;
        if (!FindPlayerById(recipient, p)) return JobOut::Error(404, "player not online");
        std::string err;
        std::string rendered = RenderMessage(sender, text);
        if (!NotifyPlayer(p, rendered, err)) {
            SetCap("sendMessage", "degraded", err);
            return JobOut::Error(503, err);
        }
        return {200, "{\"success\":true,\"verified\":false,\"verifiedBy\":\"dispatch-only: a client "
                     "notification RPC leaves no server-side state to read back\",\"delivered\":1,"
                     "\"via\":\"AVeinPlayerController::Client_SendNotification\"}"};
    });
}

// POST /teleport - LANE L3e.
//
// `UAdminComponent::Server_MovePlayer` was the primary path and it is an owner-gated no-op (L6b),
// which the old code then papered over by reporting `success:true` next to the position it had just
// read back *unchanged*. Now:
//
//   1. `UAdminComponent::MovePlayer(APlayerState*, FVector)` - **not** the `Server_` RPC. The
//      disassembly shows `Server_MovePlayer_Implementation` is 0x5c bytes of admin check that
//      tail-jumps here, and `MovePlayer` itself re-reads `IsAdmin` only to write a log line and
//      proceeds either way. It is therefore the game's own *complete* teleport - iterate the player
//      characters, match on Controller, `AVeinCharacter::Client_StartTeleport` (so the client is
//      told, instead of being corrected back by movement replication), then `SetActorLocation` and
//      `WaitForLoadPostTeleport` - with no gate on it. It still needs a component whose owner is a
//      live controller, because the log line derefs that owner.
//   2. `AActor::TeleportTo(FVector, FRotator, bIsATest=false, bNoCheck)` on the live pawn - stock
//      engine, server-authoritative. Tried with bNoCheck=true, then false.
//   3. `K2_TeleportTo` through UObject::ProcessEvent, then `AActor::SetActorLocation` - the same
//      move through the reflection path and through the primitive the game itself ends on.
//
// Success is decided by reading the pawn's location back after the call: it must differ from the
// pre-call position AND be within `kTeleportTolerance` of the target. The pre-call position is
// never echoed as the result.
namespace {

// Centimetres. A teleport lands the capsule on the ground and nudges it out of geometry, so an
// exact match is not achievable; 5 m is far tighter than any accidental walking movement inside the
// verification window and far looser than the engine's own snap.
const double kTeleportTolerance = 500.0;

double Dist3(const double a[3], const double b[3]) { return ActionsUtil::Distance3(a, b); }

// Calls K2_TeleportTo(FVector DestLocation, FRotator DestRotation) through ProcessEvent.
bool K2Teleport(void* pawn, const double dest[3], const double rot[3], std::string& err) {
    auto pe = Fn<FnProcessEvent>("UObject::ProcessEvent");
    if (!pe) {
        err = "UObject::ProcessEvent unresolved";
        return false;
    }
    void* func = Reflect::FindFunction(pawn, "K2_TeleportTo");
    if (!func) {
        err = "K2_TeleportTo is not a UFunction on this pawn";
        return false;
    }
    int32_t size = FuncParmsSize(func);
    if (size < 48) {
        err = "K2_TeleportTo has an implausible parameter block";
        return false;
    }
    std::vector<char> parms((size_t)size + 16, 0);
    std::vector<void*> props;
    WalkFuncProps(func, props);
    bool gotLoc = false;
    for (void* prop : props) {
        int32_t off = Reflect::PropertyOffset(prop);
        if (off < 0 || off + 24 > size + 16) continue;
        if (Reflect::PropertyTypeName(prop) != "StructProperty") continue;
        memcpy(parms.data() + off, gotLoc ? rot : dest, 24);
        if (gotLoc) break;
        gotLoc = true;
    }
    if (!gotLoc) {
        err = "K2_TeleportTo has no FVector parameter";
        return false;
    }
    pe(pawn, func, parms.data());
    err.clear();
    return true;
}

}  // namespace

Actions::Result Actions::Teleport(const JsonValue& body) {
    const JsonValue* idV = body.get("gameId");
    if (!idV || !idV->isStr()) return Fail(400, "'gameId' is required");
    std::string id = idV->str;
    std::string target = BodyString(body, "target");
    const JsonValue* xv = body.get("x");
    const JsonValue* yv = body.get("y");
    const JsonValue* zv = body.get("z");
    // The sidecar always sends x/y/z (0,0,0 when it only has a named target), so a non-empty
    // `target` wins over the coordinates.
    bool haveXyz = xv && yv && zv && xv->isNum() && yv->isNum() && zv->isNum() && target.empty();
    if (!haveXyz && target.empty()) return Fail(400, "numeric 'x','y','z' or a 'target' name is required");
    double x = haveXyz ? xv->num : 0, y = haveXyz ? yv->num : 0, z = haveXyz ? zv->num : 0;
    const JsonValue* yawV = body.get("yaw");
    bool haveYaw = yawV && yawV->isNum();
    double yaw = haveYaw ? yawV->num : 0;

    // Step 1 (game thread): resolve the player, the destination and the starting position.
    double dest[3] = {x, y, z}, before[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
    int lookupStatus = 0;
    std::string lookupErr;
    struct TeleportLookup {
        double dest[3], before[3] = {}, rot[3] = {};
        int status = 0;
        std::string err;
        TeleportLookup(double x, double y, double z) : dest{x, y, z} {}
    };
    auto lookup = std::make_shared<TeleportLookup>(x, y, z);
    Actions::Result pre = OnGameThread("POST /teleport (lookup)", [id, target, haveXyz, haveYaw, yaw, lookup]() -> JobOut {
        auto* dest = lookup->dest;
        auto* before = lookup->before;
        auto* rot = lookup->rot;
        auto& lookupStatus = lookup->status;
        auto& lookupErr = lookup->err;
        PlayerInfo p;
        if (!FindPlayerById(id, p)) {
            lookupStatus = 404;
            lookupErr = "player not online";
            return {200, "{}"};
        }
        if (!p.spawned) {
            lookupStatus = 409;
            lookupErr = p.pawn ? "the player is connected but is still on the character screen (pawn is " +
                                     Reflect::ClassName(p.pawn) + "), so there is nothing in the world to move"
                               : "the player is connected but has no character in the world yet";
            return {200, "{}"};
        }
        ActorLocation(p.pawn, before, rot);
        if (haveYaw) rot[1] = yaw;
        if (!haveXyz) {
            bool found = false;
            for (auto& l : ReadLocations()) {
                if (Lower(l.code) != Lower(target) && Lower(l.name) != Lower(target)) continue;
                dest[0] = l.x;
                dest[1] = l.y;
                dest[2] = l.z;
                found = true;
                break;
            }
            if (!found) {
                lookupStatus = 404;
                lookupErr = "unknown teleport target '" + target + "'";
            }
        }
        return {200, "{}"};
    });
    if (pre.status != 200) return pre;
    memcpy(dest, lookup->dest, sizeof dest);
    memcpy(before, lookup->before, sizeof before);
    memcpy(rot, lookup->rot, sizeof rot);
    lookupStatus = lookup->status;
    lookupErr = lookup->err;
    if (lookupStatus) return Fail(lookupStatus, lookupErr);

    auto posJson = [](const char* key, const double v[3]) {
        return std::string("\"") + key + "\":{\"x\":" + JsonNum(v[0]) + ",\"y\":" + JsonNum(v[1]) +
               ",\"z\":" + JsonNum(v[2]) + "}";
    };

    // Step 2: each mechanism in turn, each one verified by reading the location back.
    std::vector<std::string> attempted;
    std::string lastErr;
    double after[3] = {0, 0, 0};
    for (int m = 0; m < 3; m++) {
        std::string via, err;
        bool called = false;
        double observed[3] = {0, 0, 0};
        struct TeleportAttempt { std::string via, err; bool called = false; double observed[3] = {}; };
        auto attempt = std::make_shared<TeleportAttempt>();
        std::array<double, 3> jobDest{dest[0], dest[1], dest[2]}, jobRot{rot[0], rot[1], rot[2]};
        Actions::Result r = OnGameThread("POST /teleport", [id, m, attempt, jobDest, jobRot]() -> JobOut {
            auto& via = attempt->via;
            auto& err = attempt->err;
            auto& called = attempt->called;
            auto* observed = attempt->observed;
            const double* dest = jobDest.data();
            const double* rot = jobRot.data();
            PlayerInfo p;
            if (!FindPlayerById(id, p) || !p.pawn) {
                err = "the player left while the teleport was running";
                return {200, "{}"};
            }
            if (m == 0) {
                std::string why;
                // NEVER the class default object: MovePlayer logs through its owner, and that is
                // the null deref that SIGSEGV'd the server on 2026-09-17.
                void* admin = FindOwnedAdminComponent(why);
                auto move = Fn<FnMovePlayer>("UAdminComponent::MovePlayer");
                if (!admin || !move) {
                    err = admin ? "UAdminComponent::MovePlayer unresolved" : why;
                    return {200, "{}"};
                }
                move(admin, p.playerState, FVec3{dest[0], dest[1], dest[2]});
                via = "UAdminComponent::MovePlayer (the ungated body of Server_MovePlayer)";
                called = true;
            } else if (m == 1) {
                auto tp = Fn<FnTeleportTo>("AActor::TeleportTo");
                if (!tp) {
                    err = "AActor::TeleportTo unresolved";
                    return {200, "{}"};
                }
                if (!tp(p.pawn, dest, rot, false, true)) tp(p.pawn, dest, rot, false, false);
                via = "AActor::TeleportTo";
                called = true;
            } else {
                if (K2Teleport(p.pawn, dest, rot, err)) {
                    via = "AActor::K2_TeleportTo (ProcessEvent)";
                    called = true;
                } else {
                    auto set = Fn<FnSetActorLocation>("AActor::SetActorLocation");
                    if (!set) return {200, "{}"};  // `err` already says why K2 was unusable
                    set(p.pawn, dest, false, nullptr, 2 /* ETeleportType::ResetPhysics */);
                    via = "AActor::SetActorLocation";
                    called = true;
                }
            }
            double arot[3] = {0, 0, 0};
            ActorLocation(p.pawn, observed, arot);
            return {200, "{}"};
        });
        if (r.status != 200) return r;
        via = attempt->via;
        err = attempt->err;
        called = attempt->called;
        memcpy(observed, attempt->observed, sizeof observed);
        if (!called) {
            lastErr = err;
            continue;
        }
        attempted.push_back(via);
        memcpy(after, observed, sizeof after);
        if (ActionsUtil::TeleportArrived(before, observed, dest, kTeleportTolerance)) {
            SetCap("teleportPlayer", "ok", "verified by reading the pawn's location back, via " + via);
            return {200, "{\"success\":true,\"verified\":true,\"via\":" + JsonStr(via) + "," + posJson("position", after) +
                             "," + posJson("from", before) + "," + posJson("target", dest) +
                             ",\"offsetCm\":" + JsonNum(Dist3(dest, observed)) + "}"};
        }
    }

    std::string why = attempted.empty()
                          ? ("no teleport mechanism was available: " + lastErr)
                          : ("every teleport mechanism ran and the pawn did not arrive (it is " +
                             JsonNum(Dist3(dest, after)) + " cm from the target)");
    PluginLog("teleport: %s", why.c_str());  // see the note in Give: never degrade on one call
    return {attempted.empty() ? 501 : 409,
            "{\"success\":false,\"verified\":false,\"attempted\":" + AttemptedJson(attempted) + "," +
                posJson("position", after) + "," + posJson("from", before) + "," + posJson("target", dest) +
                ",\"skipped\":" + JsonStr(lastErr) + ",\"error\":" + JsonStr(why) + "}"};
}

// POST /give - LANE L3e.
//
// The old path was `UAdminComponent::Server_GiveItem_Implementation(APlayerState*,
// TSubclassOf<UItem>, int)`, which opens with the owner-side `IsAdmin()` check and therefore did
// nothing at all while answering `success:true` (L6b cell 5). What it does *after* that check is
// build an `FVirtualItemInstance` and hand it to the character's own inventory component, so that
// is what we do - see ResolveGiveChain above. Two mechanisms, each verified before the next runs:
//
//   1. FVirtualItemInstance::FromItem + SetStack + UBaseInventoryComponent::AddItem on the pawn's
//      `Inventory` component. No admin gate anywhere in this path.
//   2. `UAdminComponent::Server_GiveItem` on an *owned* admin component - last, and honestly
//      labelled "requires an admin online".
//
// The answer is decided by counting the player's stacks of that code before and after: `verified`
// is true only when the count went up, and `received` reports how many units actually arrived,
// which can be fewer than `amount` when the inventory ran out of room.
Actions::Result Actions::Give(const JsonValue& body) {
    const JsonValue* idV = body.get("gameId");
    const JsonValue* codeV = body.get("code");
    if (!idV || !idV->isStr()) return Fail(400, "'gameId' is required");
    if (!codeV || !codeV->isStr() || codeV->str.empty()) return Fail(400, "'code' is required");
    const JsonValue* amtV = body.get("amount");
    int amount = amtV && amtV->isNum() ? (int)amtV->num : 1;
    if (amount <= 0) return Fail(400, "'amount' must be positive");
    if (amount > 1000) return Fail(400, "'amount' must be 1000 or less");
    std::string id = idV->str, code = codeV->str;

    Housekeep();  // make sure the catalogue exists before the lookup
    void* itemClass = nullptr;
    std::string resolvedCode, resolvedName;
    {
        Guard g(g_itemLock);
        std::string lookupErr;
        const ItemDef* def = LookupItem(code, lookupErr);
        if (!def) return Fail(lookupErr.find("not loaded") != std::string::npos ? 503 : 404, lookupErr);
        itemClass = def->cls;
        resolvedCode = def->code;
        resolvedName = def->name;
    }

    // Step 1: the count before, on the game thread.
    int before = 0;
    int lookupStatus = 0;
    std::string lookupErr;
    struct GiveLookup { int before = 0, status = 0; std::string err; };
    auto lookup = std::make_shared<GiveLookup>();
    Actions::Result pre = OnGameThread("POST /give (count before)", [id, resolvedCode, lookup]() -> JobOut {
        auto& before = lookup->before;
        auto& lookupStatus = lookup->status;
        auto& lookupErr = lookup->err;
        PlayerInfo p;
        if (!FindPlayerById(id, p)) {
            lookupStatus = 404;
            lookupErr = "player not online";
            return {200, "{}"};
        }
        if (!p.spawned) {
            lookupStatus = 409;
            lookupErr = p.pawn ? "the player is connected but is still on the character screen (pawn is " +
                                     Reflect::ClassName(p.pawn) + "), so there is no inventory to give into"
                               : "the player is connected but has no character in the world yet";
            return {200, "{}"};
        }
        before = CountItem(p, resolvedCode);
        return {200, "{}"};
    });
    if (pre.status != 200) return pre;
    before = lookup->before;
    lookupStatus = lookup->status;
    lookupErr = lookup->err;
    if (lookupStatus) return Fail(lookupStatus, lookupErr);

    std::vector<std::string> attempted;
    std::string lastErr = "no give mechanism resolved";
    int after = before, lastAddResult = -1;

    for (int m = 0; m < 2; m++) {
        std::string via, err;
        bool called = false;
        int observed = before, addResult = -1;
        struct GiveAttempt { std::string via, err; bool called = false; int observed = 0, addResult = -1; };
        auto attempt = std::make_shared<GiveAttempt>();
        attempt->observed = before;
        Actions::Result r = OnGameThread("POST /give", [id, m, amount, itemClass, resolvedCode, attempt]() -> JobOut {
            auto& via = attempt->via;
            auto& err = attempt->err;
            auto& called = attempt->called;
            auto& observed = attempt->observed;
            auto& addResult = attempt->addResult;
            void* itemClassLocal = itemClass;
            PlayerInfo p;
            if (!FindPlayerById(id, p)) {
                err = "the player left while the give was running";
                return {200, "{}"};
            }
            if (m == 0) {
                GiveChain chain = ResolveGiveChain(err);
                if (!chain.ok()) return {200, "{}"};
                void* inv = GiveTargetInventory(p);
                if (!inv) {
                    err = "the character has no inventory component";
                    return {200, "{}"};
                }
                // Split by the item's own stack limit, exactly as Server_GiveItem does. The cap on
                // the iteration count is a safety net, not a policy: `amount` is already <= 1000.
                // LANE L3f / finding F19: the split used to be
                //     n = stack > 1 && remaining > stack ? stack : remaining;
                // which for a NON-stackable item (MaxStackOf returns 1) collapsed to
                // `n = remaining` and built ONE instance with `SetStack(amount)`. The game ignores
                // `Stack` on a non-stackable item, so `giveItem BP_Corn_C amount:3` put exactly one
                // corn in the player's hands while the plugin read the instance back as three.
                // `ActionsUtil::StackSplit` is the same decision, made where it can be tested.
                uint8_t last = 0;
                for (int n : ActionsUtil::StackSplit(amount, MaxStackOf(itemClassLocal)))
                    last = AddOneStack(chain, inv, itemClassLocal, n);
                called = true;
                addResult = (int)last;
                via = "FVirtualItemInstance::FromItem + UBaseInventoryComponent::AddItem";
            } else {
                std::string why;
                void* admin = FindOwnedAdminComponent(why);
                auto give = Fn<FnAdminGiveItem>("UAdminComponent::Server_GiveItem");
                if (!admin || !give) {
                    err = admin ? "UAdminComponent::Server_GiveItem unresolved" : why;
                    return {200, "{}"};
                }
                give(admin, p.playerState, &itemClassLocal, amount);  // &: passed by invisible reference
                called = true;
                via = "UAdminComponent::Server_GiveItem (requires an admin online)";
            }
            if (called) observed = CountItem(p, resolvedCode);
            return {200, "{}"};
        });
        if (r.status != 200) return r;
        via = attempt->via;
        err = attempt->err;
        called = attempt->called;
        observed = attempt->observed;
        addResult = attempt->addResult;
        if (!called) {
            if (!err.empty()) lastErr = err;
            continue;
        }
        attempted.push_back(via);
        after = observed;
        lastAddResult = addResult;
        if (ActionsUtil::GiveArrived(before, after)) {
            SetCap("giveItem", "ok", "verified by the inventory stack count, via " + via);
            return {200, "{\"success\":true,\"verified\":true,\"code\":" + JsonStr(resolvedCode) + ",\"name\":" +
                             JsonStr(resolvedName) + ",\"amount\":" + std::to_string(amount) + ",\"received\":" +
                             std::to_string(after - before) + ",\"before\":" + std::to_string(before) +
                             ",\"after\":" + std::to_string(after) + ",\"via\":" + JsonStr(via) +
                             ",\"addResult\":" + std::to_string(addResult) + ",\"attempted\":" +
                             AttemptedJson(attempted) + "}"};
        }
    }

    std::string why = attempted.empty()
                          ? ("no give mechanism was available: " + lastErr)
                          : ("every give mechanism ran and the inventory count for '" + resolvedCode +
                             "' did not change (still " + std::to_string(after) + ")");
    // NOT SetCap(degraded): capability status describes whether a MECHANISM exists, not how the
    // last call went. The sidecar refuses an action whose capability is not ok, so degrading here
    // would let one 409 - a player still on the character screen, a full inventory - disable
    // giveItem for every player until the next restart.
    PluginLog("give: %s", why.c_str());
    return {attempted.empty() ? 501 : 409,
            "{\"success\":false,\"verified\":false,\"code\":" + JsonStr(resolvedCode) + ",\"before\":" +
                std::to_string(before) + ",\"after\":" + std::to_string(after) + ",\"addResult\":" +
                std::to_string(lastAddResult) + ",\"attempted\":" + AttemptedJson(attempted) + ",\"skipped\":" + JsonStr(lastErr) + ",\"error\":" + JsonStr(why) +
                "}"};
}

// Called from lane L2's join resolver, ALREADY ON THE GAME THREAD - so it must not poll (that
// would enqueue a job behind itself and deadlock). It fires every mechanism in turn and reports
// only that a call was made; the PreLogin refusal is what actually enforces the ban.
bool Actions::KickBanned(const std::string& gameId) {
    PlayerInfo p;
    if (!FindPlayerById(gameId, p)) return false;
    for (int m = 0; m < kKickMechanisms; m++) {
        SessionAttempt a = KickMechanism(m, p, "Banned", true);
        if (!a.called) continue;
        PluginLog("kick-banned: %s via %s", gameId.c_str(), a.via.c_str());
        if (PlayerGone(gameId)) return true;
    }
    return PlayerGone(gameId);
}

// POST /kick - LANE L3e.
//
// Each mechanism is fired on the game thread, then the player table is polled (also on the game
// thread, one short job per sample) for up to five seconds. `via` names the mechanism the player
// disappeared after; if none of them removes the player, the answer is 409 with `verified:false`
// and the list of everything that was tried - never `{"success":true}`.
Actions::Result Actions::Kick(const JsonValue& body) {
    std::string id = BodyString(body, "gameId");
    if (id.empty()) return Fail(400, "'gameId' is required");
    std::string reason = BodyString(body, "reason");

    // Resolve the player once, up front, so "not online" is a clean 404 rather than a failed kick.
    std::string gameId;
    bool online = false;
    struct KickLookup { std::string gameId; bool online = false; };
    auto lookup = std::make_shared<KickLookup>();
    Actions::Result pre = OnGameThread("POST /kick (lookup)", [id, lookup]() -> JobOut {
        PlayerInfo p;
        lookup->online = FindPlayerById(id, p);
        if (lookup->online) lookup->gameId = p.gameId;
        return {200, "{}"};
    });
    if (pre.status != 200) return pre;
    gameId = lookup->gameId;
    online = lookup->online;
    if (!online) return Fail(404, "player not online");

    std::vector<std::string> attempted;
    std::string lastErr;
    for (int m = 0; m < kKickMechanisms; m++) {
        SessionAttempt a;
        auto attempt = std::make_shared<SessionAttempt>();
        Actions::Result r = OnGameThread("POST /kick", [id, reason, m, attempt]() -> JobOut {
            PlayerInfo p;
            if (!FindPlayerById(id, p)) {
                attempt->called = false;
                attempt->err = "gone";
                return {200, "{}"};
            }
            *attempt = KickMechanism(m, p, reason, false);
            return {200, "{}"};
        });
        if (r.status != 200) return r;  // 503/504: the game thread, not the kick
        a = *attempt;
        if (a.err == "gone") break;     // already left between mechanisms
        if (!a.called) {
            lastErr = a.err;
            continue;
        }
        attempted.push_back(a.via);
        // 5 s for the first mechanism: that is the client's own disconnect round trip -
        // ClientWasKicked has to reach the client and the connection has to close before the player
        // state leaves AGameStateBase::PlayerArray. The later mechanisms get 1.5 s each so that the
        // worst case (all three ran, nothing worked) still fits inside the sidecar's 10 s plugin
        // timeout and comes back as an honest 409 rather than as a client-side timeout.
        // 4 s for the first mechanism (the client's own disconnect round trip: ClientWasKicked has
        // to reach the client and the connection close before the player state leaves
        // AGameStateBase::PlayerArray - the working path is well under 1 s), 1 s for the rest, so
        // the worst case "all three ran and nothing worked" is ~6 s and still comes back as an
        // honest 409 inside **Takaro's own** 10 s action timeout rather than as a timeout.
        if (PollGameThread([id] { return PlayerGone(id); }, m == 0 ? 4000 : 1000)) {
            SetCap("kick", "ok", "verified by the player leaving the player table, via " + a.via);
            return {200, "{\"success\":true,\"verified\":true,\"gameId\":" + JsonStr(gameId) + ",\"online\":false," +
                             "\"via\":" + JsonStr(a.via) + ",\"attempted\":" + AttemptedJson(attempted) + "}"};
        }
    }
    std::string why = attempted.empty() ? ("no kick mechanism was available: " + lastErr)
                                        : "every kick mechanism ran and the player is still connected";
    PluginLog("kick: %s", why.c_str());  // see the note in Give: never degrade on one call
    return {attempted.empty() ? 501 : 409,
            "{\"success\":false,\"verified\":false,\"gameId\":" + JsonStr(gameId) + ",\"online\":true,\"attempted\":" +
                AttemptedJson(attempted) + ",\"skipped\":" + JsonStr(lastErr) + ",\"error\":" + JsonStr(why) +
                "}"};
}

Actions::Result Actions::Ban(const JsonValue& body) {
    std::string id = BodyString(body, "gameId");
    if (id.empty()) return Fail(400, "'gameId' is required");
    std::string reason = BodyString(body, "reason");
    const JsonValue* exp = body.get("expiresAt");
    std::string expiresAt = exp && exp->isStr() ? exp->str : "";

    Result result = OnGameThread("POST /ban", [id, reason, expiresAt,
                                              lifetime = std::make_shared<BanJobLifetime>()]() -> JobOut {
        std::string normalized = NormalizeGameId(id);
        PlayerInfo found;
        bool online = FindPlayerById(id, found);
        if (online && !found.gameId.empty()) normalized = found.gameId;

        // 1. The plugin list, always and first: it accepts an id the server has never seen, it
        //    carries the reason and the expiry, and it is what lane L2's PreLogin hook refuses a
        //    rejoin with while the server keeps running.
        state::BanRecord rec;
        rec.gameId = normalized;
        rec.name = online ? found.name : state::CharacterName(normalized);
        rec.reason = reason;
        rec.expiresAt = expiresAt;
        rec.createdAt = IsoNowUtc();
        bool pluginList = state::BanAdd(rec);

        // 2. VEIN's own id-keyed list, which persists and works offline.
        std::string err;
        bool persisted = WriteGameBan(normalized, reason, true, err);

        // 3. Disconnect an online player through the admin panel's ban path.
        // LANE L3e: the ban itself is the list write above (that is what PreLogin refuses a rejoin
        // with, and it is proven); disconnecting the session already in progress is a separate,
        // best-effort leg, and it is reported as *observed*, not as attempted. This runs on the game
        // thread, so it cannot poll - it checks the player table straight after each mechanism.
        std::string disconnect;
        if (online) {
            for (int m = 0; m < kKickMechanisms; m++) {
                SessionAttempt a = KickMechanism(m, found, reason, true);
                if (a.called && PlayerGone(id)) break;
            }
            disconnect = PlayerGone(id) ? "disconnected" : "still connected";
        }
        if (!persisted && !pluginList) {
            SetCap("ban", "degraded", "ban failed: " + err);
            return JobOut::Error(503, "ban failed: " + err);
        }
        std::string detail = err + (disconnect.empty() ? "" : ("; the online player was " + disconnect));
        // LANE L3e: read both lists back. `verified` means "the id is listed now", not "the write
        // call returned"; a ban that is not listed cannot refuse a rejoin, so it is not a ban.
        bool inPlugin = false, inGame = false;
        bool verified = BanListedNow(normalized, inPlugin, inGame);
        if (!verified) {
            SetCap("ban", "degraded", "the ban was written but neither list shows it: " + err);
            return {409, [normalized, err] { return "{\"success\":false,\"verified\":false,\"gameId\":" + JsonStr(normalized) +
                             ",\"error\":" + JsonStr("the ban was written but neither the plugin list nor the "
                                                     "game's own list shows it: " + err) + "}"; }};
        }
        SetCap("ban", "ok", "verified by reading the ban lists back");
        return {200, [normalized, online, inGame, inPlugin, disconnect, detail] {
            return "{\"success\":true,\"verified\":true,\"gameId\":" + JsonStr(normalized) + ",\"online\":" +
                         (online ? "true" : "false") + ",\"persisted\":" + (inGame ? "true" : "false") +
                         ",\"pluginList\":" + (inPlugin ? "true" : "false") + ",\"via\":" +
                         JsonStr(std::string(inGame ? "AVeinGameStateBase ban list (Game.ini)" : "") +
                                 (inGame && inPlugin ? " + " : "") + (inPlugin ? "the plugin ban list" : "")) +
                         ",\"enforcedBy\":" + JsonStr(inGame ? "game" : "plugin") + ",\"disconnected\":" +
                         JsonStr(disconnect) + ",\"detail\":" + JsonStr(detail) + "}";
        }};
    });
    if (!state::FlushBans()) return Fail(503, state::BanPersistenceError());
    return result;
}

size_t Actions::PendingBanJobs() { return g_pendingBanJobs.load(); }

static Actions::Result UnbanWithRevision(const JsonValue& body, bool checkRevision, uint64_t expectedRevision) {
    std::string id = BodyString(body, "gameId");
    if (id.empty()) return Fail(400, "'gameId' is required");
    Actions::Result result = OnGameThread("POST /unban", [id, checkRevision, expectedRevision,
                                                         lifetime = std::make_shared<BanJobLifetime>()]() -> JobOut {
        if (checkRevision && state::BanRevision() != expectedRevision)
            return JobOut::Error(409, "ban changed before timed expiry; preserving current ban");
        std::string normalized = NormalizeGameId(id);
        bool pluginList = state::BanRemove(normalized);
        std::string err;
        bool persisted = WriteGameBan(normalized, "", false, err);
        if (!persisted && !pluginList) {
            SetCap("unban", "degraded", err);
            return JobOut::Error(503, "unban failed: " + err);
        }
        // LANE L3e: verified means the id is gone from BOTH lists on a fresh read - if either one
        // still holds it, PreLogin still refuses the rejoin and the unban did not happen.
        bool inPlugin = false, inGame = false;
        bool stillListed = BanListedNow(normalized, inPlugin, inGame);
        if (stillListed) {
            std::string why = std::string("the unban was written but the id is still in ") +
                              (inPlugin ? "the plugin ban list" : "") + (inPlugin && inGame ? " and " : "") +
                              (inGame ? "the game's own ban list" : "") + "; a rejoin would still be refused";
            SetCap("unban", "degraded", why);
            return {409, [normalized, inPlugin, inGame, why] {
                return "{\"success\":false,\"verified\":false,\"gameId\":" + JsonStr(normalized) +
                             ",\"pluginList\":" + (inPlugin ? "true" : "false") + ",\"persisted\":" +
                             (inGame ? "true" : "false") + ",\"error\":" + JsonStr(why) + "}";
            }};
        }
        SetCap("unban", "ok", "verified by reading both ban lists back empty for this id");
        return {200, [normalized, pluginList, persisted, err] {
            return "{\"success\":true,\"verified\":true,\"gameId\":" + JsonStr(normalized) +
                         ",\"removedFromPluginList\":" + (pluginList ? "true" : "false") +
                         ",\"removedFromGameList\":" + (persisted ? "true" : "false") +
                         ",\"via\":\"both ban lists re-read and empty for this id\",\"detail\":" +
                         JsonStr(err) + "}";
        }};
    });
    if (!state::FlushBans()) return Fail(503, state::BanPersistenceError());
    return result;
}

Actions::Result Actions::Unban(const JsonValue& body) { return UnbanWithRevision(body, false, 0); }
Actions::Result Actions::UnbanIfRevision(const JsonValue& body, uint64_t expectedRevision) {
    return UnbanWithRevision(body, true, expectedRevision);
}

Actions::Result Actions::Shutdown() {
    if (g_shutdownRequested.exchange(true)) return {200, "{\"success\":true,\"detail\":\"already shutting down\"}"};
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, ShutdownThread, nullptr);
    pthread_attr_destroy(&attr);
    return {200, "{\"success\":true,\"detail\":\"saving, then SIGTERM\"}"};
}

// ================================================================================================
// POST /debug/kill-nearest  (debug builds only; the HTTP layer gates it on TAKARO_PLUGIN_DEBUG)
//
// http.cpp routes this to Actions::KillNearest, but the implementation belongs to lane L2: it needs
// the AI class set, the damage pipeline and the entity-killed hook, all of which live in events.cpp.
// It deliberately does not emit the event itself - the kill has to come out of the normal
// entity-killed path or it proves nothing.

Actions::Result Actions::KillNearest(const JsonValue& body) { return Events::KillNearest(body); }

// ================================================================================================
// POST /command

namespace {

const char* kHelp =
    "players | say <msg> | whisper <gameId> <msg> | give <gameId> <code> [n] | tp <gameId> <x> <y> <z> | "
    "kick <gameId> [reason] | ban <gameId> [reason] | unban <gameId> | bans | items [query] | entities | "
    "locations | save | shutdown | raw <console command> | vein <admin exec command> | cheat <gameId> <cmd> | help";

JobOut CommandOutput(bool success, const std::string& out) {
    return {200, [success, out] {
        return "{\"success\":" + std::string(success ? "true" : "false") + ",\"output\":" + JsonStr(out) + "}";
    }};
}

// UEngine::Exec with our own FOutputDevice, built by copying FOutputDeviceFile's vtable and
// replacing every slot that holds one of its Serialize implementations with ours.
std::string g_execOutput;
Mutex g_execLock;

void CaptureSerialize(void* /*self*/, const char16_t* msg, int /*verbosity*/, const void* /*category*/) {
    if (!msg) return;
    int len = 0;
    while (len < 4096 && MemReadable(msg + len, 2) && msg[len]) len++;
    g_execOutput += Reflect::Utf16To8(msg, len);
    g_execOutput += "\n";
}
void CaptureSerializeTime(void* self, const char16_t* msg, int verbosity, const void* category, double /*t*/) {
    CaptureSerialize(self, msg, verbosity, category);
}
// Every non-Serialize slot of our synthetic device: do nothing, report "no". The object owns no
// file handle, so calling FOutputDeviceFile's own Flush/TearDown on it would fault.
bool NoopSlot(void*) { return false; }

bool RunExec(const std::string& cmd, std::string& out, std::string& err) {
    auto exec = Fn<FnEngineExec>("UEngine::Exec");
    if (!exec) {
        err = "UEngine::Exec unresolved";
        return false;
    }
    void* engineCls = Reflect::FindObjectByPath("/Script/Engine", "Engine");
    std::vector<void*> engines;
    if (engineCls) Reflect::GetObjectsOfClass(engineCls, engines, true);
    void* engine = engines.empty() ? nullptr : engines[0];
    if (!engine) {
        err = "no live UEngine object";
        return false;
    }
    void* world = FindWorld();
    uint64_t ztv = DynSymAddr("_ZTV17FOutputDeviceFile");
    if (!ztv) {
        err = "FOutputDeviceFile vtable not exported";
        return false;
    }
    const size_t kSlots = 24;
    static void* vt[kSlots];
    auto* src = (void* const*)(uintptr_t)(ztv + 16);
    if (!MemReadable(src, kSlots * 8)) {
        err = "FOutputDeviceFile vtable not readable";
        return false;
    }
    uint64_t ser4 = Resolve::Addr("FOutputDeviceFile::Serialize");
    size_t replaced = 0;
    for (size_t i = 0; i < kSlots; i++) vt[i] = (void*)&NoopSlot;
    for (size_t i = 0; i < kSlots; i++) {
        if (!ser4 || (uint64_t)(uintptr_t)src[i] != ser4) continue;
        vt[i] = (void*)&CaptureSerializeTime;
        // The 3-argument overload sits next to it; which side depends on the declaration order the
        // compiler emitted, and the neighbouring dtor slot is never called on our stack object, so
        // both neighbours get the (memory-guarded) 3-argument capture.
        if (i + 1 < kSlots) vt[i + 1] = (void*)&CaptureSerialize;
        if (i > 0) vt[i - 1] = (void*)&CaptureSerialize;
        replaced++;
        break;
    }
    if (!replaced) {
        err = "could not locate FOutputDeviceFile::Serialize in its vtable";
        return false;
    }
    struct Device {
        void* vptr;
        uint8_t pad[64];
    } dev;
    memset(&dev, 0, sizeof dev);
    dev.vptr = vt;
    auto w = Reflect::Utf8To16(cmd);
    Guard g(g_execLock);
    g_execOutput.clear();
    bool handled = exec(engine, world, w.data(), &dev);
    out = g_execOutput;
    if (!handled && out.empty()) out = "(command not recognised by the engine)";
    return true;
}

JsonValue JStr(const std::string& s) {
    JsonValue v;
    v.type = JsonValue::String;
    v.str = s;
    return v;
}
JsonValue JNum(const std::string& raw) {
    JsonValue v;
    v.type = JsonValue::Number;
    v.num = atof(raw.c_str());
    v.str = raw;
    return v;
}

}  // namespace

Actions::Result Actions::Command(const JsonValue& body) {
    std::string command = BodyString(body, "command");
    if (command.empty()) return Fail(400, "'command' is required");
    auto w = Words(command);
    if (w.empty()) return Fail(400, "'command' is required");
    std::string verb = Lower(w[0]);

    auto wrap = [&](const Result& r) -> Result {
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    };

    if (verb == "help") return {200, "{\"success\":true,\"output\":" + JsonStr(kHelp) + "}"};
    if (verb == "players") return wrap(Players());
    if (verb == "bans") return wrap(Bans());
    if (verb == "items") return wrap(Items(w.size() > 1 ? Rest(command, 1) : ""));
    if (verb == "entities") return wrap(Entities());
    if (verb == "locations") return wrap(Locations());

    if (verb == "say") {
        if (w.size() < 2) return Fail(400, "usage: say <message>");
        JsonValue b;
        b.type = JsonValue::Object;
        b.obj.push_back({"text", JStr(Rest(command, 1))});
        return wrap(Message(b));
    }
    if (verb == "whisper") {
        if (w.size() < 3) return Fail(400, "usage: whisper <gameId> <message>");
        JsonValue b;
        b.type = JsonValue::Object;
        b.obj.push_back({"text", JStr(Rest(command, 2))});
        b.obj.push_back({"recipientGameId", JStr(w[1])});
        return wrap(Message(b));
    }
    if (verb == "give") {
        if (w.size() < 3) return Fail(400, "usage: give <gameId> <code> [amount]");
        JsonValue b;
        b.type = JsonValue::Object;
        b.obj.push_back({"gameId", JStr(w[1])});
        b.obj.push_back({"code", JStr(w[2])});
        b.obj.push_back({"amount", JNum(w.size() > 3 ? w[3] : "1")});
        return wrap(Give(b));
    }
    if (verb == "tp" || verb == "teleport") {
        if (w.size() < 5) return Fail(400, "usage: tp <gameId> <x> <y> <z>");
        JsonValue b;
        b.type = JsonValue::Object;
        b.obj.push_back({"gameId", JStr(w[1])});
        const char* keys[] = {"x", "y", "z"};
        for (int i = 0; i < 3; i++) b.obj.push_back({keys[i], JNum(w[2 + i])});
        return wrap(Teleport(b));
    }
    if (verb == "kick" || verb == "ban" || verb == "unban") {
        if (w.size() < 2) return Fail(400, "usage: " + verb + " <gameId> [reason]");
        JsonValue b;
        b.type = JsonValue::Object;
        b.obj.push_back({"gameId", JStr(w[1])});
        std::string reason = w.size() > 2 ? Rest(command, 2) : "";
        if (!reason.empty()) b.obj.push_back({"reason", JStr(reason)});
        return wrap(verb == "kick" ? Kick(b) : verb == "ban" ? Ban(b) : Unban(b));
    }
    if (verb == "save") {
        return OnGameThread("command save", []() -> JobOut {
            std::string why;
            void* admin = FindAdminComponent(why);
            auto save = Fn<FnAdminVoid>("UAdminComponent::Server_RequestDedicatedServerSave");
            if (!save) return JobOut::Error(501, "UAdminComponent::Server_RequestDedicatedServerSave unresolved");
            if (!admin) return JobOut::Error(503, why);
            save(admin);
            return CommandOutput(true, "save requested");
        });
    }
    if (verb == "shutdown") return Shutdown();
    if (verb == "cheat") {
        return {501,
                "{\"error\":\"unimplemented\",\"capability\":\"executeCommand\",\"detail\":\"cheat commands need a "
                "CheatManager, which the dedicated server does not create\"}"};
    }
    if (verb == "vein") {
        // The admin panel's own console: UAdminComponent::Server_Exec. It produces no output we can
        // capture, so the answer only says that it was dispatched.
        if (w.size() < 2) return Fail(400, "usage: vein <admin exec command>");
        std::string cmd = Rest(command, 1);
        return OnGameThread("command vein", [cmd]() -> JobOut {
            std::string why;
            void* admin = FindAdminComponent(why);
            auto exec = Fn<FnAdminString>("UAdminComponent::Server_Exec");
            if (!exec) return JobOut::Error(501, "UAdminComponent::Server_Exec unresolved");
            if (!admin) return JobOut::Error(503, why);
            GameFString s(cmd);
            if (!s.ok) return JobOut::Error(503, "could not allocate the command string");
            exec(admin, &s.fs);
            return CommandOutput(true, "dispatched to UAdminComponent::Server_Exec (it returns no output)");
        });
    }
    if (verb == "raw") {
        if (w.size() < 2) return Fail(400, "usage: raw <console command>");
        std::string cmd = Rest(command, 1);
        return OnGameThread(
            "command raw",
            [cmd]() -> JobOut {
                std::string out, err;
                if (!RunExec(cmd, out, err)) return JobOut::Error(501, err);
                return CommandOutput(true, out);
            },
            10000);
    }
    return Fail(400, std::string("unknown command '") + w[0] + "'. " + kHelp);
}
