#include "conan/events.h"

#include "common.h"
#include "conan/events_payload.h"
#include "conan/hook_dispatch.h"
#include "conan/identity.h"
#include "conan/logtail.h"
#include "conan/text.h"
#include "gamethread.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "ue/mem.h"
#include "ue/reflect.h"
#include "ue/ue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace conan {
namespace {

using events::PlayerId;
using events::Position;
using HookDispatch::Call;

template <typename T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void*)a, sizeof v);
    return v;
}

constexpr uint8_t kStateDead = 1;  // ECharacterState::Dead (S4)
constexpr uint64_t kDamageFreshMs = 250, kPendingDeathMs = 60, kDedupeMs = 5000;
constexpr size_t kDamageSlots = 64, kDedupeSlots = 32, kMaxPendingDeaths = 32;
constexpr int kWorkerTickMs = 50;

// ---------------------------------------------------------------------------------------- names
enum N {
    N_UserIDFromURLOptions, N_PlayerState, N_PlayerNamePrivate, N_SavedNetworkAddress, N_Controller,
    N_m_CharacterName, N_SourceSpawnTable, N_OwnerItem, N_TemplateId, N_ConanPlayerController,
    N_ConanCharacter, N_InventoryItemBase, N_GameItem, N_Conv_TextToString, N_GetItemName,
    N_K2_GetActorLocation, N_InText, N_ReturnValue, N_Pawn, N_UniqueID, N_Count
};
const char* const kNames[N_Count] = {
    "UserIDFromURLOptions", "PlayerState", "PlayerNamePrivate", "SavedNetworkAddress", "Controller",
    "m_CharacterName", "SourceSpawnTable", "OwnerItem", "TemplateId", "ConanPlayerController",
    "ConanCharacter", "InventoryItemBase", "GameItem", "Conv_TextToString", "GetItemName",
    "K2_GetActorLocation", "InText", "ReturnValue", "Pawn", "UniqueID",
};
uint32_t g_n[N_Count];
std::atomic<bool> g_namesReady{false};

// Lazily resolved property offset (game thread). Only asked on an object of the right type.
struct LazyOff {
    int32_t v = -2;
    int32_t Get(uintptr_t obj, N name) {
        if (v == -2 && obj) v = UER::PropertyOffset(UER::ClassOf(obj), g_n[name]);
        return v;
    }
};

// ------------------------------------------------------------------------- captured (to worker)
enum class Kind { Connected, Disconnected, Chat, Death, Killed, DeferredLogin };
struct Captured {
    Kind kind;
    PlayerId a, b;  // a: the event's player; b: attacker (Death)
    bool hasB = false;
    std::string s1, s2;  // Chat: channel, msg. Death: msg. Killed: entity, weapon.
    std::string s3;      // Killed: the victim's class when s1 is empty
    Position pos;
    uintptr_t controller = 0;  // DeferredLogin only (re-read on the game thread)
};

struct Stats {
    std::atomic<uint64_t> connected{0}, disconnected{0}, chat{0}, deaths{0}, kills{0}, logs{0};
    std::atomic<uint64_t> deferredLogins{0}, unknownLogout{0}, unknownChat{0}, deathNoIdentity{0}, killsNotByPlayer{0},
        dedupedDeaths{0}, pendingDeaths{0}, notReady{0}, dropped{0}, textConversions{0}, connectWaitedForPawn{0},
        connectTimedOut{0}, leftBeforeSpawn{0};
    std::atomic<uint64_t> deathHandlerMaxNs{0};
};

// ------------------------------------------------------------------- game-thread-only state
struct Damage {
    uintptr_t victim = 0;
    uintptr_t instigator = 0, causer = 0, damageType = 0;
    uintptr_t killerChar = 0;  // OnOwnerKilled InKiller
    uint64_t tMs = 0, killerMs = 0;
};
struct PendingDeath {
    uintptr_t victim = 0;
    bool isPlayer = false;
    PlayerId victimId;
    std::string victimName;  // entity display name (non-player)
    std::string victimClass;  // class name when victimName is empty (resolved on the worker)
    Position pos;
    uint64_t tMs = 0;
};

struct GT {
    std::unordered_map<uintptr_t, PlayerId> byController;  // PostLogin cache
    std::unordered_map<std::string, PlayerId> bySteam;     // last known identity per Steam64
    std::unordered_map<uintptr_t, uintptr_t> pawnToController;
    Damage damage[kDamageSlots];
    size_t damageNext = 0;
    std::pair<uintptr_t, uint64_t> dedupe[kDedupeSlots] = {};
    size_t dedupeNext = 0;
    std::unordered_map<uint32_t, std::string> entityBySpawnRow;  // SourceSpawnTable FName -> name
    std::unordered_map<int32_t, std::string> weaponByTemplate;
    LazyOff offUserId, offPlayerState, offPlayerName, offIp, offPawnController, offCharName, offSpawnRow,
        offOwnerItem, offTemplateId, offControllerPawn, offUniqueId;
    bool loggedIdSource = false;
    UER::TextConv conv;
    bool convTried = false;
};
GT& G() {
    static GT* g = new GT;
    return *g;
}

// ------------------------------------------------------------------------ shared (any thread)
struct Shared {
    EventsOptions o;
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Captured> queue;
    std::vector<PendingDeath> pending;  // deaths waiting up to 60 ms for their damage record
    std::atomic<int> pendingCount{0};   // lets the damage hook skip the lock in the common case
    // player-connected waits until the controller possesses its character: Takaro asks for the
    // location right after the event and drops the event when that fails (client still loading).
    struct PendingLogin {
        uintptr_t pc = 0;
        PlayerId id;
        uint64_t tLogin = 0, emitAt = 0;  // emitAt 0: no character yet
    };
    std::vector<PendingLogin> pendingLogins;
    std::atomic<int> pendingLoginCount{0};
    bool stopping = false;
    std::thread worker;
    std::unique_ptr<LogTail> logtail;
    Stats st;
    bool started = false;
};
Shared& Sh() {
    static Shared* s = new Shared;
    return *s;
}

void Push(Captured c) {
    std::lock_guard<std::mutex> g(Sh().mu);
    if (Sh().queue.size() >= 4096) {
        Sh().st.dropped++;
        return;
    }
    Sh().queue.push_back(std::move(c));
}

// ------------------------------------------------------------------------ game-thread readers

bool IsPlayerController(uintptr_t pc) { return UER::Alive(pc) && UER::IsA(pc, g_n[N_ConanPlayerController]); }

// Steam64, PlayerNamePrivate, SavedNetworkAddress of a ConanPlayerController. The Steam64 comes
// from PlayerState.UniqueID (conan/identity.h); UserIDFromURLOptions only when it is a Steam64
// itself (on FLS accounts it holds the Funcom id).
bool ReadIdentity(uintptr_t pc, PlayerId& out) {
    GT& g = G();
    if (!IsPlayerController(pc)) return false;
    int32_t pso = g.offPlayerState.Get(pc, N_PlayerState);
    uintptr_t ps = pso >= 0 ? rd<uintptr_t>(pc + (uintptr_t)pso) : 0;
    std::string how;
    UniqueIdProbe uid;
    if (UER::Live(ps)) {
        int32_t n = g.offPlayerName.Get(ps, N_PlayerNamePrivate), ip = g.offIp.Get(ps, N_SavedNetworkAddress);
        if (n >= 0) out.name = UER::ReadFString(ps + (uintptr_t)n, 256);
        if (ip >= 0) out.ip = UER::ReadFString(ps + (uintptr_t)ip, 128);
        int32_t uo = g.offUniqueId.Get(ps, N_UniqueID);
        if (uo >= 0) {
            uid = Steam64FromUniqueId(UE::SelfMem(), ps + (uintptr_t)uo);
            how = "PlayerState.UniqueID " + uid.how;
        }
    }
    std::string url;
    int32_t u = g.offUserId.Get(pc, N_UserIDFromURLOptions);
    if (u >= 0) url = UER::ReadFString(pc + (uintptr_t)u, 64);
    out.steam64 = GameIdFrom(uid, url);  // conan/identity.h: the one gameId rule
    if (uid.steam64.empty() && !out.steam64.empty()) how += (how.empty() ? "" : "; ") + std::string("UserIDFromURLOptions");
    if (!g.loggedIdSource && !out.steam64.empty()) {
        g.loggedIdSource = true;
        NativeLog("events: Steam64 from %s (UserIDFromURLOptions is %s)", how.c_str(),
                  url == out.steam64 ? "the same Steam64" : ("'" + url + "'").c_str());
    } else if (out.steam64.empty()) {
        NativeLog("events: no Steam64 for a player controller (%s; UserIDFromURLOptions '%s')", how.c_str(), url.c_str());
    }
    return out.Valid();
}

// Identity of a controller: the login cache first, then a live read.
bool IdentityOf(uintptr_t pc, PlayerId& out) {
    GT& g = G();
    auto it = g.byController.find(pc);
    if (it != g.byController.end()) {
        out = it->second;
        return true;
    }
    if (!ReadIdentity(pc, out)) return false;
    if (out.name.empty()) {
        auto s = g.bySteam.find(out.steam64);
        if (s != g.bySteam.end()) out.name = s->second.name;
    }
    return true;
}

uintptr_t ControllerOfPawn(uintptr_t pawn) {
    GT& g = G();
    if (!UER::Live(pawn)) return 0;
    int32_t o = g.offPawnController.Get(pawn, N_Controller);
    uintptr_t pc = o >= 0 ? rd<uintptr_t>(pawn + (uintptr_t)o) : 0;
    if (UER::Alive(pc)) return pc;
    auto it = g.pawnToController.find(pawn);
    return it != g.pawnToController.end() && UER::Alive(it->second) ? it->second : 0;
}

const UER::TextConv& Conv() {
    GT& g = G();
    if (!g.conv.Ok()) {
        uintptr_t cdo = HookDispatch::FoundObject("Default__KismetTextLibrary");
        if (cdo) {
            uintptr_t f = UER::FindFunction(UER::ClassOf(cdo), g_n[N_Conv_TextToString]);
            if (f) {
                g.conv.cdo = cdo;
                g.conv.func = f;
                g.conv.inOff = UER::PropertyOffset(f, g_n[N_InText]);
                g.conv.retOff = UER::PropertyOffset(f, g_n[N_ReturnValue]);
                g.conv.parmsSize = UER::FunctionParmsSize(f);
            }
        }
    } else if (!UER::Alive(g.conv.cdo)) {
        g.conv = UER::TextConv();
    }
    return g.conv;
}

std::string TextAt(uintptr_t ftext) {
    Sh().st.textConversions++;
    return UER::TextToString(Conv(), ftext);
}

// FName -> SpawnDataTable row -> Name FText (S3 §4 layout: RowMap @0x30, elements {FName, row*}
// stride 24, row Name FText @8). Game thread, only on a cache miss.
std::string SpawnTableName(uint32_t rowName) {
    uintptr_t table = HookDispatch::FoundObject("SpawnDataTable");
    if (!table || rowName == UER::kNoName) return "";
    uintptr_t data = rd<uintptr_t>(table + 0x30);
    int32_t num = rd<int32_t>(table + 0x38);
    if (!data || num <= 0 || num > 200000) return "";
    for (int32_t i = 0; i < num; i++) {
        uintptr_t e = data + (uintptr_t)i * 24;
        if (rd<uint32_t>(e) != rowName || rd<uint32_t>(e + 4) != 0) continue;
        uintptr_t row = rd<uintptr_t>(e + 8);
        return row ? TextAt(row + 8) : "";
    }
    return "";
}

// Display name of a live non-player character (the same string listEntities reports).
std::string EntityName(uintptr_t ch) {
    GT& g = G();
    if (!UER::Live(ch) || !UER::IsA(ch, g_n[N_ConanCharacter])) return "";
    int32_t ro = g.offSpawnRow.Get(ch, N_SourceSpawnTable);
    uint32_t row = ro >= 0 ? rd<uint32_t>(ch + (uintptr_t)ro) : UER::kNoName;
    if (ro >= 0 && rd<uint32_t>(ch + (uintptr_t)ro + 4) != 0) row = UER::kNoName;  // numbered FName: no cache
    if (row != UER::kNoName && row != 0) {
        auto it = g.entityBySpawnRow.find(row);
        if (it != g.entityBySpawnRow.end()) return it->second;
    }
    std::string name;
    int32_t no = g.offCharName.Get(ch, N_m_CharacterName);
    if (no >= 0) name = TextAt(ch + (uintptr_t)no);
    if (name.empty() && row != UER::kNoName && row != 0) name = SpawnTableName(row);
    if (events::IsInternalName(name)) name.clear();
    if (!name.empty() && row != UER::kNoName && row != 0 && g.entityBySpawnRow.size() < 20000)
        g.entityBySpawnRow[row] = name;
    return name;
}

// Item display name of the weapon actor that dealt the damage.
std::string WeaponOf(uintptr_t causer) {
    GT& g = G();
    if (!UER::Live(causer)) return "";
    if (!UER::IsA(causer, g_n[N_InventoryItemBase])) return "";
    int32_t oo = g.offOwnerItem.Get(causer, N_OwnerItem);
    uintptr_t item = oo >= 0 ? rd<uintptr_t>(causer + (uintptr_t)oo) : 0;
    if (!UER::Live(item) || !UER::IsA(item, g_n[N_GameItem])) return "";
    int32_t to = g.offTemplateId.Get(item, N_TemplateId);
    if (to < 0) return "";
    const int32_t templateId = rd<int32_t>(item + (uintptr_t)to);
    auto it = g.weaponByTemplate.find(templateId);
    if (it != g.weaponByTemplate.end()) return it->second;
    std::string name;
    uintptr_t f = UER::FindFunction(UER::ClassOf(item), g_n[N_GetItemName]);
    int32_t ret = f ? UER::PropertyOffset(f, g_n[N_ReturnValue]) : -1;
    if (f && ret >= 0 && UER::FunctionParmsSize(f) <= 128) {
        alignas(16) uint8_t parms[256];
        memset(parms, 0, sizeof parms);
        UE::CallProcessEvent((void*)item, (void*)f, parms);
        name = TextAt((uintptr_t)(parms + ret));
    }
    std::string w = events::WeaponName(name, templateId);
    if (w.empty()) w = "item #" + std::to_string(templateId);  // never "unknown" when a TemplateId exists
    if (g.weaponByTemplate.size() < 20000) g.weaponByTemplate[templateId] = w;
    return w;
}

Position LocationOf(uintptr_t actor) {
    Position p;
    if (!UER::Live(actor)) return p;
    uintptr_t f = UER::FindFunction(UER::ClassOf(actor), g_n[N_K2_GetActorLocation]);
    int32_t ret = f ? UER::PropertyOffset(f, g_n[N_ReturnValue]) : -1;
    if (!f || ret < 0 || UER::FunctionParmsSize(f) > 128) return p;
    alignas(16) uint8_t parms[256];
    memset(parms, 0, sizeof parms);
    UE::CallProcessEvent((void*)actor, (void*)f, parms);
    double v[3];
    memcpy(v, parms + ret, sizeof v);
    p.has = true;
    p.x = v[0];
    p.y = v[1];
    p.z = v[2];
    return p;
}

Damage* DamageFor(uintptr_t victim, bool create) {
    GT& g = G();
    for (auto& d : g.damage)
        if (d.victim == victim) return &d;
    if (!create) return nullptr;
    Damage& d = g.damage[g.damageNext++ % kDamageSlots];
    d = Damage();
    d.victim = victim;
    return &d;
}

// Killer attribution from a damage record (game thread, actors alive). Fills the attacker identity
// when a player did it, else a readable killer name (NPC) and/or the cause.
struct Killer {
    bool isPlayer = false;
    PlayerId player;
    std::string name, cause, weapon;
};
Killer Attribute(const Damage* d, uintptr_t victim, uintptr_t victimController, uint64_t now) {
    Killer k;
    if (!d) return k;
    const bool damageFresh = d->tMs && now - d->tMs <= kDamageFreshMs;
    const bool killerFresh = d->killerMs && now - d->killerMs <= kDamageFreshMs;
    if (damageFresh && d->damageType && UER::Alive(d->damageType))
        k.cause = events::CauseFromDamageType(UER::ObjectName(d->damageType));
    if (killerFresh && d->killerChar && d->killerChar != victim) {
        uintptr_t pc = ControllerOfPawn(d->killerChar);
        if (pc && pc != victimController && IsPlayerController(pc) && IdentityOf(pc, k.player)) k.isPlayer = true;
        else k.name = EntityName(d->killerChar);
    }
    if (!k.isPlayer && damageFresh && d->instigator && d->instigator != victimController &&
        IsPlayerController(d->instigator) && IdentityOf(d->instigator, k.player)) {
        k.isPlayer = true;
        k.name.clear();
    }
    if (!k.isPlayer && k.name.empty() && damageFresh && d->instigator && UER::Alive(d->instigator) &&
        d->instigator != victimController) {
        GT& g = G();
        int32_t po = g.offControllerPawn.Get(d->instigator, N_Pawn);
        uintptr_t pawn = po >= 0 ? rd<uintptr_t>(d->instigator + (uintptr_t)po) : 0;
        if (pawn && pawn != victim) k.name = EntityName(pawn);
    }
    if (k.isPlayer && damageFresh && d->causer) k.weapon = WeaponOf(d->causer);
    return k;
}

void FinishDeath(const PendingDeath& pd, const Killer& k) {
    Captured c;
    if (pd.isPlayer) {
        c.kind = Kind::Death;
        c.a = pd.victimId;
        if (k.isPlayer) {
            c.b = k.player;
            c.hasB = true;
        }
        c.pos = pd.pos;
        c.s1 = events::DeathMessage(pd.victimId.name, k.isPlayer ? k.player.name : k.name, k.cause);
        Push(std::move(c));
        return;
    }
    if (!k.isPlayer) {
        Sh().st.killsNotByPlayer++;
        return;
    }
    c.kind = Kind::Killed;
    c.a = k.player;
    c.s1 = pd.victimName;
    c.s3 = pd.victimClass;
    c.s2 = k.weapon;
    Push(std::move(c));
}

// ------------------------------------------------------------------------------------- handlers

constexpr uint64_t kConnectAfterPossessMs = 500, kConnectTimeoutMs = 120000;

// Game thread. Queues player-connected; it is sent once the controller has a pawn.
void QueueConnect(uintptr_t pc, const PlayerId& id) {
    GT& g = G();
    g.byController[pc] = id;
    g.bySteam[id.steam64] = id;
    int32_t po = g.offControllerPawn.Get(pc, N_Pawn);
    uintptr_t pawn = po >= 0 ? rd<uintptr_t>(pc + (uintptr_t)po) : 0;
    Shared::PendingLogin pl;
    pl.pc = pc;
    pl.id = id;
    pl.tLogin = NowMs();
    pl.emitAt = UER::Live(pawn) ? pl.tLogin + kConnectAfterPossessMs : 0;
    std::lock_guard<std::mutex> lk(Sh().mu);
    Sh().pendingLogins.push_back(pl);
    Sh().pendingLoginCount = (int)Sh().pendingLogins.size();
}

bool OnPostLogin(const Call& c, void*) {
    if (!g_namesReady.load(std::memory_order_acquire)) {
        Sh().st.notReady++;
        return true;
    }
    const uintptr_t pc = c.Ptr(0);
    PlayerId id;
    if (!ReadIdentity(pc, id)) {
        if (!IsPlayerController(pc)) return true;
        Sh().st.deferredLogins++;
        Captured d;
        d.kind = Kind::DeferredLogin;
        d.controller = pc;
        Push(std::move(d));
        return true;
    }
    QueueConnect(pc, id);
    return true;
}

bool OnLogout(const Call& c, void*) {
    if (!g_namesReady.load(std::memory_order_acquire)) {
        Sh().st.notReady++;
        return true;
    }
    const uintptr_t pc = c.Ptr(0);
    GT& g = G();
    PlayerId id;
    auto it = g.byController.find(pc);
    if (it != g.byController.end()) {
        id = it->second;
        g.byController.erase(it);
    } else if (UER::Alive(pc) && UER::IsA(pc, g_n[N_ConanPlayerController])) {
        // Loaded after this login (or the login was missed): the Steam64 is still readable.
        int32_t u = g.offUserId.Get(pc, N_UserIDFromURLOptions);
        if (u >= 0) id.steam64 = GameIdFrom(UniqueIdProbe(), UER::ReadFString(pc + (uintptr_t)u, 64));
        auto s = g.bySteam.find(id.steam64);
        if (s != g.bySteam.end()) id = s->second;
    }
    for (auto p = g.pawnToController.begin(); p != g.pawnToController.end();)
        p = p->second == pc ? g.pawnToController.erase(p) : std::next(p);
    if (Sh().pendingLoginCount.load(std::memory_order_relaxed) > 0) {
        bool wasPending = false;
        {
            std::lock_guard<std::mutex> lk(Sh().mu);
            auto& pl = Sh().pendingLogins;
            for (size_t i = 0; i < pl.size(); i++)
                if (pl[i].pc == pc) {
                    pl.erase(pl.begin() + (long)i);
                    wasPending = true;
                    break;
                }
            Sh().pendingLoginCount = (int)pl.size();
        }
        if (wasPending) {  // left while loading: neither event (Takaro could not have kept the connect)
            Sh().st.leftBeforeSpawn++;
            return true;
        }
    }
    if (!id.Valid()) {
        Sh().st.unknownLogout++;
        return true;
    }
    Captured e;
    e.kind = Kind::Disconnected;
    e.a = id;
    Push(std::move(e));
    return true;
}

bool OnChat(const Call& c, void*) {
    if (!g_namesReady.load(std::memory_order_acquire)) {
        Sh().st.notReady++;
        return true;
    }
    uint8_t* data = c.At(0);
    if (!data) return true;
    if (rd<uint8_t>((uintptr_t)data + ChatRpc::kGenerated)) return true;  // system lines, not a player
    PlayerId id;
    if (!IdentityOf(c.obj, id)) {
        Sh().st.unknownChat++;
        return true;
    }
    Captured e;
    e.kind = Kind::Chat;
    e.a = id;
    e.s1 = UER::ReadFString((uintptr_t)data + ChatRpc::kChannel, 64);
    e.s2 = UER::ReadFString((uintptr_t)data + ChatRpc::kMessage, 4096);
    Push(std::move(e));
    return true;
}

bool OnPossessed(const Call& c, void*) {
    const uintptr_t pc = c.Ptr(0);
    GT& g = G();
    if (g.pawnToController.size() > 1024) {
        for (auto p = g.pawnToController.begin(); p != g.pawnToController.end();)
            p = UER::Alive(p->first) ? std::next(p) : g.pawnToController.erase(p);
    }
    if (pc) g.pawnToController[c.obj] = pc;
    if (pc && Sh().pendingLoginCount.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(Sh().mu);
        for (auto& pl : Sh().pendingLogins)
            if (pl.pc == pc && !pl.emitAt) {
                pl.emitAt = NowMs() + kConnectAfterPossessMs;
                Sh().st.connectWaitedForPawn++;
            }
    }
    return true;
}

bool OnOwnerKilled(const Call& c, void*) {
    const uintptr_t owner = c.Ptr(0), killer = c.Ptr(1);
    if (!owner) return true;
    Damage* d = DamageFor(owner, true);
    d->killerChar = killer;
    d->killerMs = NowMs();
    return true;
}

// ctx: nullptr = ReceiveAnyDamage (params DamageType, InstigatedBy, DamageCauser), else Point.
bool OnDamage(const Call& c, void*) {
    if (!g_namesReady.load(std::memory_order_acquire)) return true;
    const uintptr_t victim = c.obj;
    Damage* d = DamageFor(victim, true);
    d->damageType = c.Ptr(0);
    d->instigator = c.Ptr(1);
    d->causer = c.Ptr(2);
    d->tMs = NowMs();
    // A death that arrived before its damage record (S4: ReceiveAnyDamage can follow EventOnDeath).
    PendingDeath pd;
    bool found = false;
    if (Sh().pendingCount.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> g(Sh().mu);
        auto& pend = Sh().pending;
        for (size_t i = 0; i < pend.size(); i++) {
            if (pend[i].victim != victim) continue;
            pd = pend[i];
            pend.erase(pend.begin() + (long)i);
            Sh().pendingCount = (int)pend.size();
            found = true;
            break;
        }
    }
    if (found) {
        uintptr_t vpc = pd.isPlayer ? ControllerOfPawn(victim) : 0;
        FinishDeath(pd, Attribute(d, victim, vpc, NowMs()));
    }
    return true;
}

bool OnStateChange(const Call& c, void*) {
    if (!g_namesReady.load(std::memory_order_acquire)) return true;
    const uint8_t oldState = c.Get<uint8_t>(1, 0xff), newState = c.Get<uint8_t>(2, 0xff);
    if (newState != kStateDead || oldState == kStateDead) return true;
    const uint64_t t0 = NowNs();
    const uintptr_t ch = c.Has(0) && c.Ptr(0) ? c.Ptr(0) : c.obj;
    const uint64_t now = NowMs();
    GT& g = G();
    for (auto& e : g.dedupe)
        if (e.first == ch && now - e.second < kDedupeMs) {
            Sh().st.dedupedDeaths++;
            return true;
        }
    g.dedupe[g.dedupeNext++ % kDedupeSlots] = {ch, now};
    if (!UER::Alive(ch) || !UER::IsA(ch, g_n[N_ConanCharacter])) return true;

    PendingDeath pd;
    pd.victim = ch;
    pd.tMs = now;
    const uintptr_t pc = ControllerOfPawn(ch);
    pd.isPlayer = pc && IsPlayerController(pc);
    if (pd.isPlayer) {
        if (!IdentityOf(pc, pd.victimId)) {
            Sh().st.deathNoIdentity++;
            return true;
        }
        pd.pos = LocationOf(ch);
    } else {
        pd.victimName = EntityName(ch);
        if (pd.victimName.empty()) pd.victimClass = UER::ObjectName(UER::ClassOf(ch));
    }
    Damage* d = DamageFor(ch, false);
    const bool fresh = d && ((d->tMs && now - d->tMs <= kDamageFreshMs) || (d->killerMs && now - d->killerMs <= kDamageFreshMs));
    if (fresh) {
        FinishDeath(pd, Attribute(d, ch, pd.isPlayer ? pc : 0, now));
    } else {
        Sh().st.pendingDeaths++;
        std::lock_guard<std::mutex> lk(Sh().mu);
        if (Sh().pending.size() < kMaxPendingDeaths) Sh().pending.push_back(pd);
        Sh().pendingCount = (int)Sh().pending.size();
    }
    uint64_t ns = NowNs() - t0, prev = Sh().st.deathHandlerMaxNs.load();
    while (ns > prev && !Sh().st.deathHandlerMaxNs.compare_exchange_weak(prev, ns)) {
    }
    return true;
}

// ---------------------------------------------------------------------------------- worker

void Emit(const char* type, const JsonValue& data, std::atomic<uint64_t>& counter) {
    JsonValue clean = events::Sanitize(type, data);
    if (clean.type != JsonValue::Object) return;
    takaro::GameEvent ev;
    ev.type = type;
    ev.data = std::move(clean);
    counter++;
    if (Sh().o.emit) Sh().o.emit(std::move(ev));
}

void Deliver(const Captured& c) {
    Stats& st = Sh().st;
    switch (c.kind) {
        case Kind::Connected:
            Emit("player-connected", events::ConnectedPayload(c.a), st.connected);
            NativeLog("events: player-connected %s (%s)", c.a.steam64.c_str(), c.a.name.c_str());
            break;
        case Kind::Disconnected:
            Emit("player-disconnected", events::ConnectedPayload(c.a), st.disconnected);
            NativeLog("events: player-disconnected %s (%s)", c.a.steam64.c_str(), c.a.name.c_str());
            break;
        case Kind::Chat:
            Emit("chat-message", events::ChatPayload(c.a, c.s1, c.s2), st.chat);
            NativeLog("events: chat-message %s channel=%s (%zu chars)", c.a.steam64.c_str(), c.s1.c_str(), c.s2.size());
            break;
        case Kind::Death:
            Emit("player-death", events::DeathPayload(c.a, c.hasB ? &c.b : nullptr, c.pos, c.s1), st.deaths);
            NativeLog("events: player-death %s attacker=%s msg=\"%s\"", c.a.steam64.c_str(),
                      c.hasB ? c.b.steam64.c_str() : "-", c.s1.c_str());
            break;
        case Kind::Killed:
        {
            std::string entity = c.s1;
            if (entity.empty() && !c.s3.empty() && Sh().o.entityNameForClass) entity = Sh().o.entityNameForClass(c.s3);
            if (entity.empty()) entity = events::EntityNameFromClass(c.s3);
            if (entity.empty()) entity = "Unknown creature";
            Emit("entity-killed", events::KilledPayload(c.a, entity, c.s2), st.kills);
            NativeLog("events: entity-killed by %s entity=\"%s\"%s%s weapon=\"%s\"", c.a.steam64.c_str(), entity.c_str(),
                      c.s1.empty() ? " from class " : "", c.s1.empty() ? c.s3.c_str() : "", c.s2.c_str());
        }
            break;
        case Kind::DeferredLogin:
            break;
    }
}

struct Deferred {
    uintptr_t pc;
    uint64_t at;
    int tries;
};

void WriteHealth() {
    if (Sh().o.healthFile.empty()) return;
    std::string err;
    takaro::AtomicWriteFile(Sh().o.healthFile, EventsHealthJson() + "\n", err);
}

void WorkerLoop() {
    std::vector<Deferred> deferred;
    uint64_t nextHealth = 0;
    for (;;) {
        std::vector<Captured> batch;
        std::vector<PendingDeath> expired;
        {
            std::unique_lock<std::mutex> l(Sh().mu);
            Sh().cv.wait_for(l, std::chrono::milliseconds(kWorkerTickMs), [] { return Sh().stopping; });
            if (Sh().stopping) return;
            batch.swap(Sh().queue);
            const uint64_t now = NowMs();
            auto& pend = Sh().pending;
            for (size_t i = 0; i < pend.size();) {
                if (now - pend[i].tMs > kPendingDeathMs) {
                    expired.push_back(pend[i]);
                    pend.erase(pend.begin() + (long)i);
                    Sh().pendingCount = (int)pend.size();
                } else {
                    i++;
                }
            }
        }
        std::vector<Shared::PendingLogin> dueLogins;
        {
            std::lock_guard<std::mutex> lk(Sh().mu);
            const uint64_t now = NowMs();
            auto& pl = Sh().pendingLogins;
            for (size_t i = 0; i < pl.size();) {
                bool timeout = !pl[i].emitAt && now - pl[i].tLogin > kConnectTimeoutMs;
                if ((pl[i].emitAt && now >= pl[i].emitAt) || timeout) {
                    if (timeout) Sh().st.connectTimedOut++;
                    dueLogins.push_back(pl[i]);
                    pl.erase(pl.begin() + (long)i);
                } else {
                    i++;
                }
            }
            Sh().pendingLoginCount = (int)pl.size();
        }
        for (auto& l : dueLogins) {
            Captured e;
            e.kind = Kind::Connected;
            e.a = l.id;
            Deliver(e);
        }
        if (!g_namesReady.load() && UE::HaveGlobals()) {
            std::vector<std::string> list(kNames, kNames + N_Count);
            std::vector<uint32_t> idx;
            if (UER::LookupNames(list, idx) == (size_t)N_Count) {
                for (int i = 0; i < N_Count; i++) g_n[i] = idx[i];
                g_namesReady.store(true, std::memory_order_release);
                NativeLog("events: engine names resolved");
            }
        }
        for (auto& pd : expired) FinishDeath(pd, Killer());  // no damage arrived: cause unknown
        for (auto& c : batch) {
            if (c.kind == Kind::DeferredLogin) deferred.push_back({c.controller, NowMs() + 200, 0});
            else Deliver(c);
        }
        // An empty Steam64 at PostLogin: re-read it on the game thread a few times.
        for (size_t i = 0; i < deferred.size();) {
            Deferred& d = deferred[i];
            if (NowMs() < d.at) {
                i++;
                continue;
            }
            PlayerId id;
            bool alive = false, ok = false;
            GameThread::Run(
                [&] {
                    alive = IsPlayerController(d.pc);
                    ok = alive && ReadIdentity(d.pc, id);
                    if (ok) QueueConnect(d.pc, id);
                },
                1000);
            if (ok || !alive || ++d.tries >= 10) {
                if (!ok) NativeLog("events: gave up reading the identity of a new player controller");
                deferred.erase(deferred.begin() + (long)i);
            } else {
                d.at = NowMs() + 500;
                i++;
            }
        }
        if (NowMs() >= nextHealth) {
            nextHealth = NowMs() + 10000;
            WriteHealth();
        }
    }
}

void SubscribeAll() {
    using HookDispatch::Phase;
    auto sub = [](const char* base, const char* fn, Phase ph, std::vector<std::string> params, HookDispatch::Handler h) {
        HookDispatch::Subscription s;
        s.owner = "events";
        s.baseClass = base;
        s.function = fn;
        s.phase = ph;
        s.params = std::move(params);
        s.fn = h;
        if (HookDispatch::Subscribe(s) < 0) NativeLog("events: could not subscribe %s.%s", base, fn);
    };
    sub("GameModeBase", "K2_PostLogin", Phase::After, {"NewPlayer"}, OnPostLogin);
    sub("GameModeBase", "K2_OnLogout", Phase::Before, {"ExitingController"}, OnLogout);
    sub("ConanPlayerController", "ServerSendChatMessage", Phase::Before, {"chatData"}, OnChat);
    sub("ConanCharacter", "ClientPossessedBy", Phase::Before, {"NewController"}, OnPossessed);
    sub("BountyHuntFactionComponent", "OnOwnerKilled", Phase::Before, {"InOwner", "InKiller"}, OnOwnerKilled);
    sub("ConanCharacter", "ReceiveAnyDamage", Phase::Before, {"DamageType", "InstigatedBy", "DamageCauser"}, OnDamage);
    sub("ConanCharacter", "ReceivePointDamage", Phase::Before, {"DamageType", "InstigatedBy", "DamageCauser"}, OnDamage);
    sub("ConanCharacter", "EventOnDeath", Phase::Before, {"Character", "OldState", "NewState"}, OnStateChange);
    HookDispatch::RequestObject("Default__KismetTextLibrary");
    HookDispatch::RequestObject("SpawnDataTable");
}
}  // namespace

void StartEvents(const EventsOptions& o) {
    Shared& s = Sh();
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (s.started) return;
        s.started = true;
        s.o = o;
    }
    if (o.hooks) SubscribeAll();
    s.worker = std::thread(WorkerLoop);
    LogTailOptions lo;
    lo.path = ServerLogPath(o.savedDir);
    lo.secrets = o.secrets;
    lo.perSecond = std::atof(EnvOr("TAKARO_CONAN_LOG_RATE", "30").c_str());
    if (lo.perSecond <= 0) lo.perSecond = 30;
    lo.burst = lo.perSecond * 10;
    lo.emit = [](const std::string& line) {
        Emit("log", events::LogPayload(line), Sh().st.logs);
    };
    if (EnvOr("TAKARO_CONAN_LOG_EVENTS", "1") != "0" && !lo.path.empty()) {
        s.logtail.reset(new LogTail(lo));
        s.logtail->Start();
        NativeLog("events: tailing %s for log events (%.0f lines/s)", lo.path.c_str(), lo.perSecond);
    }
}

void StopEvents() {
    Shared& s = Sh();
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (!s.started) return;
        s.stopping = true;
    }
    s.cv.notify_all();
    if (s.worker.joinable()) s.worker.join();
    if (s.logtail) s.logtail->Stop();
}

std::string EventsHealthJson() {
    Stats& st = Sh().st;
    auto n = [](const std::atomic<uint64_t>& a) { return (double)a.load(std::memory_order_relaxed); };
    return takaro::ObjBuilder()
        .B("namesReady", g_namesReady.load())
        .Raw("emitted", takaro::ObjBuilder()
                            .N("player-connected", n(st.connected))
                            .N("player-disconnected", n(st.disconnected))
                            .N("chat-message", n(st.chat))
                            .N("player-death", n(st.deaths))
                            .N("entity-killed", n(st.kills))
                            .N("log", n(st.logs))
                            .Done())
        .N("deferredLogins", n(st.deferredLogins))
        .N("connectWaitedForPawn", n(st.connectWaitedForPawn))
        .N("connectTimedOut", n(st.connectTimedOut))
        .N("leftBeforeSpawn", n(st.leftBeforeSpawn))
        .N("unknownLogout", n(st.unknownLogout))
        .N("unknownChat", n(st.unknownChat))
        .N("deathNoIdentity", n(st.deathNoIdentity))
        .N("killsNotByPlayer", n(st.killsNotByPlayer))
        .N("dedupedDeaths", n(st.dedupedDeaths))
        .N("pendingDeaths", n(st.pendingDeaths))
        .N("notReady", n(st.notReady))
        .N("dropped", n(st.dropped))
        .N("textConversions", n(st.textConversions))
        .N("deathHandlerMaxUs", n(st.deathHandlerMaxNs) / 1000.0)
        .Raw("hooks", HookDispatch::HealthJson())
        .Raw("logtail", Sh().logtail ? Sh().logtail->HealthJson() : "null")
        .Done();
}

}  // namespace conan
