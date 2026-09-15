#include "world.h"

#include "hooks.h"
#include "scan.h"
#include "state.h"

#include "MinHook.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>

// ------------------------------------------------------------------------------------------------
// Constants that are reflection type hashes (keen qualifiedHash of the type name); they only change if the type is
// renamed. Offsets that are build-specific are derived from matched code in WorldHooksInit (see Layout).
namespace {

const uint32_t kCurrentTransform = 0x027ac564;  // keen::WorldTransform, 56 B
const uint32_t kTeleport = 0x23384d40;          // 80 B, server_only
const uint32_t kInventorySetup = 0xc24df82a;    // 84 B
const uint32_t kInventory = 0xb19528a9;         // ItemStack[] (12 B each)
const uint32_t kItemState = 0x3a2fac75;         // PIDE entity: rarity @16, level @17
const uint32_t kPlayerInput = 0xfb4f945a;       // 1320 B
const uint32_t kServerConsumed = 0xeff8ed59;    // 216 B

const char* kInvCategory[] = {"invalid", "customization", "equipment", "currency", "generic", "virtual"};
const char* kRarity[] = {"Common", "Uncommon", "Rare", "Epic", "Legendary"};

struct Layout {
    uint32_t worldOff = 0;    // Server -> World*            (0x1b0)
    uint32_t slotBase = 0;    // Server -> slots[16]         (0x1c0)
    uint32_t slotStride = 0;  // sizeof(ServerPlayerSlot)    (0x2bb38)
    uint32_t chatOff = 0;     // Server -> ChatMessagesSystem* (0x68)
    uint32_t piCreateVer = 0; // PlayerInput.fromAdminClient.adminInventoryCreateAction.version (0x334)
    uint32_t scCreateVer = 0; // ServerConsumedPlayerInput.consumedAdminInventoryCreateAction  (0x94)
    uint32_t ctxHits = 0x58, ctxDied = 0x50;  // combat_experience_source context: HitEvent / EntityDiedEvent views
} L;
const uint32_t kSlotEid = 0x38, kSlotName = 0x40, kSlotState = 0x0a;

struct CompRef {
    void* ptr;
    uint64_t size;
};
struct Span {
    const void* data;
    uint64_t size;
};
struct StringView {
    const char* ptr;
    uint64_t len;
};
struct Opt64 {
    uint64_t v;
    uint8_t has;
    uint8_t pad[7];
};

using GetCompFn = CompRef*(__fastcall*)(CompRef* out, uint64_t world, uint32_t hash, uint32_t eid);
using AddCompFn = uint64_t(__fastcall*)(uint64_t world, uint32_t eid, uint32_t hash, Span* data);
using Fn4 = uint64_t(__fastcall*)(uint64_t, uint64_t, uint64_t, uint64_t);
using IsAdminFn = uint64_t(__fastcall*)(uint64_t eid, uint64_t table);
using AddMsgFn = uint8_t*(__fastcall*)(uint8_t* err, uint64_t chat, uint64_t type, StringView* text, uint64_t loca1,
                                         uint64_t loca2, uint64_t handle, Opt64* o1, Opt64* o2);
using CtxFn = uint64_t(__fastcall*)(uint64_t sys, uint8_t* buf, uint64_t size);
using TmplFn = uint8_t*(__fastcall*)(uint64_t sys, uint8_t* outGuid, uint64_t eid);

GetCompFn g_getRead = nullptr;
AddCompFn g_addComp = nullptr;
Fn4 g_origUpd = nullptr;
Fn4 g_origInvJob = nullptr;
IsAdminFn g_origIsAdmin = nullptr;
AddMsgFn g_origAddMsg = nullptr;
Fn4 g_origCombat = nullptr;
CtxFn g_getCtx = nullptr;
TmplFn g_tmplOf = nullptr;
uintptr_t g_recvRet = 0;      // return address of the player-chat addMessage call in processIncomingMessages
uintptr_t g_invGateRet = 0;   // return address of the isAdminEntity call in inventory_actions

volatile LONG64 g_server = 0;
volatile LONG64 g_updTicks = 0, g_invJobTicks = 0, g_combatTicks = 0, g_chatSeen = 0, g_bypassHits = 0;
volatile LONG64 g_deaths = 0, g_kills = 0, g_hitsSeen = 0, g_diedSeen = 0;

bool Readable(uint64_t p, size_t n) {
    if (p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return p + n <= (uint64_t)mbi.BaseAddress + mbi.RegionSize;
}

char g_hx[32];
const char* Hx(uint64_t v) {
    snprintf(g_hx, sizeof g_hx, "0x%llx", (unsigned long long)v);
    return g_hx;
}

// ---- world-thread queue (drained after Server::updatePlayers on the server main thread) ----
struct WorldTask {
    std::function<std::string(uint64_t)> fn;
    std::string result;
    HANDLE done = nullptr;
    ~WorldTask() {
        if (done) CloseHandle(done);
    }
};
SrwLock g_wqLock;
std::deque<std::shared_ptr<WorldTask>> g_wq;

uint64_t __fastcall UpdDetour(uint64_t server, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t ret = g_origUpd(server, b, c, d);
    InterlockedIncrement64(&g_updTicks);
    InterlockedExchange64(&g_server, (LONG64)server);
    for (int budget = 0; budget < 16; budget++) {
        std::shared_ptr<WorldTask> t;
        {
            Guard g(g_wqLock);
            if (g_wq.empty()) break;
            t = g_wq.front();
            g_wq.pop_front();
        }
        t->result = t->fn(server);
        SetEvent(t->done);
    }
    return ret;
}

bool RunOnWorld(std::function<std::string(uint64_t)> fn, std::string& result, std::string& err, DWORD timeoutMs = 3000) {
    if (!g_origUpd) {
        err = "server updatePlayers hook not installed";
        return false;
    }
    if (!g_updTicks) {
        err = "server updatePlayers has not run yet";
        return false;
    }
    auto t = std::make_shared<WorldTask>();
    t->fn = std::move(fn);
    t->done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    {
        Guard g(g_wqLock);
        g_wq.push_back(t);
    }
    if (WaitForSingleObject(t->done, timeoutMs) != WAIT_OBJECT_0) {
        err = "server thread did not run the task in time";
        return false;
    }
    result = t->result;
    return true;
}

// ---- slots ----
uint64_t Slot(uint64_t server, int i) { return server + L.slotBase + (uint64_t)i * L.slotStride; }
uint64_t World(uint64_t server) { return *(uint64_t*)(server + L.worldOff); }

std::string SlotName(uint64_t slot) {
    const char* p = *(const char**)(slot + kSlotName);
    uint64_t n = *(uint64_t*)(slot + kSlotName + 8);
    if (!p || n == 0 || n > 256 || !Readable((uint64_t)p, (size_t)n)) return "";
    return std::string(p, (size_t)n);
}

// Slot index of the logged-in player with this name; -1 none, -2 ambiguous.
int SlotByName(uint64_t server, const std::string& name) {
    int found = -1;
    for (int i = 0; i < 16; i++) {
        uint64_t s = Slot(server, i);
        if (!*(uint32_t*)s || !*(uint32_t*)(s + kSlotEid)) continue;
        if (SlotName(s) != name) continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

int SlotByEntity(uint64_t server, uint32_t eid) {
    if (!eid) return -1;
    for (int i = 0; i < 16; i++) {
        uint64_t s = Slot(server, i);
        if (*(uint32_t*)s && *(uint32_t*)(s + kSlotEid) == eid) return i;
    }
    return -1;
}

CompRef Read(uint64_t world, uint32_t hash, uint32_t eid) {
    CompRef r{nullptr, 0};
    g_getRead(&r, world, hash, eid);
    return r;
}

// Resolves the player entity on the server thread; "" + err on failure.
bool PlayerEntity(uint64_t server, const std::string& name, uint32_t& eid, std::string& err) {
    int i = SlotByName(server, name);
    if (i == -2) {
        err = "several player slots share the name '" + name + "'";
        return false;
    }
    if (i < 0) {
        err = "no spawned player slot named '" + name + "'";
        return false;
    }
    eid = *(uint32_t*)(Slot(server, i) + kSlotEid);
    return true;
}

double FromFixed(int64_t v) { return (double)v / 4294967296.0; }
int64_t ToFixed(double m) { return (int64_t)llround(m * 4294967296.0); }

// ---- give item: admin inventory-create action injected into PlayerInput, consumed by the inventory_actions job ----
enum GiveState : LONG { kGiveIdle = 0, kGiveActive = 1, kGiveDone = 2 };
struct Give {
    uint32_t eid = 0, itemId = 0, version = 0, consumedAtDone = 0;
    uint16_t count = 0;  // informational: the game ignores AdminInventoryCreateAction.count
    bool fullStack = false;
    int injections = 0;
};
SrwLock g_giveLock;
Give g_give;
volatile LONG g_giveState = kGiveIdle;
volatile LONG g_bypassEid = 0;
SrwLock g_giveSerial;  // one give at a time

void GiveTryInject() {
    uint64_t server = (uint64_t)g_server;
    if (!server) return;
    Guard g(g_giveLock);
    if (g_giveState != kGiveActive) return;
    uint64_t world = World(server);
    CompRef pi = Read(world, kPlayerInput, g_give.eid);
    CompRef sc = Read(world, kServerConsumed, g_give.eid);
    if (!pi.ptr || !sc.ptr || pi.size < L.piCreateVer + 16 || sc.size < L.scCreateVer + 4) return;
    uint32_t consumed = *(uint32_t*)((uint8_t*)sc.ptr + L.scCreateVer);
    if (!g_give.version) g_give.version = consumed + 1;
    if (consumed == g_give.version) {
        g_give.consumedAtDone = consumed;
        InterlockedExchange(&g_bypassEid, 0);
        InterlockedExchange(&g_giveState, kGiveDone);
        return;
    }
    uint8_t* a = (uint8_t*)pi.ptr + L.piCreateVer - 4;  // AdminInventoryCreateAction (20 B)
    memcpy(a, &g_give.itemId, 4);
    memcpy(a + 4, &g_give.version, 4);
    memcpy(a + 8, &g_give.count, 2);
    memset(a + 10, 0, 10);  // hasItemRarity, rarity, level, createForAllPlayers, createFullStack, fill..., cleanup, flame
    a[14] = g_give.fullStack ? 1 : 0;  // createFullStack: AdminInventoryCreate (0x153ef0) creates maxStackSize, else 1
    g_give.injections++;
    InterlockedExchange(&g_bypassEid, (LONG)g_give.eid);
}

void GiveCheckConsumed() {
    uint64_t server = (uint64_t)g_server;
    if (!server || g_giveState != kGiveActive) return;
    Guard g(g_giveLock);
    if (g_giveState != kGiveActive || !g_give.version) return;
    CompRef sc = Read(World(server), kServerConsumed, g_give.eid);
    if (sc.ptr && *(uint32_t*)((uint8_t*)sc.ptr + L.scCreateVer) == g_give.version) {
        g_give.consumedAtDone = g_give.version;
        InterlockedExchange(&g_bypassEid, 0);
        InterlockedExchange(&g_giveState, kGiveDone);
    }
}

uint64_t __fastcall InvJobDetour(uint64_t sys, uint64_t b, uint64_t c, uint64_t d) {
    InterlockedIncrement64(&g_invJobTicks);
    if (g_giveState == kGiveActive) GiveTryInject();
    uint64_t ret = g_origInvJob(sys, b, c, d);
    if (g_giveState == kGiveActive) GiveCheckConsumed();
    return ret;
}

uint64_t __fastcall IsAdminDetour(uint64_t eid, uint64_t table) {
    LONG bypass = g_bypassEid;
    if (bypass && (uint32_t)eid == (uint32_t)bypass && (uintptr_t)__builtin_return_address(0) == g_invGateRet) {
        InterlockedIncrement64(&g_bypassHits);
        return 1;
    }
    return g_origIsAdmin(eid, table);
}

// ---- chat receive ----
uint8_t* __fastcall AddMsgDetour(uint8_t* err, uint64_t chat, uint64_t type, StringView* text, uint64_t l1, uint64_t l2,
                                 uint64_t handle, Opt64* o1, Opt64* o2) {
    if ((uintptr_t)__builtin_return_address(0) == g_recvRet && text && text->ptr && text->len > 0 && text->len <= 0x400) {
        InterlockedIncrement64(&g_chatSeen);
        std::string msg(text->ptr, (size_t)text->len);
        uint64_t server = (uint64_t)g_server;
        std::string name, player;
        if (server) {
            uint64_t s = Slot(server, (int)(handle & 0x3f));
            if (*(uint32_t*)s == (uint32_t)handle) name = SlotName(s);
        }
        std::string data = "{\"msg\":" + JsonStr(msg) + ",\"channel\":\"global\",\"chatType\":" +
                           std::to_string((unsigned)(type & 0xff)) + ",\"senderName\":" + JsonStr(name);
        if (!name.empty() && PluginState::Get().PlayerJsonByName(name, player)) data += ",\"player\":" + player;
        data += "}";
        PluginState::Get().EmitEvent("chat-message", data);
        PluginLog("chat: type=%u handle=0x%x name=%s len=%llu", (unsigned)(type & 0xff), (unsigned)handle, name.c_str(),
                  (unsigned long long)text->len);
    }
    return g_origAddMsg(err, chat, type, text, l1, l2, handle, o1, o2);
}

// ---- combat: killing blows + deaths ----
struct KillMemo {
    uint32_t victim, killer;
    uint64_t ms;
};
SrwLock g_killLock;
std::deque<KillMemo> g_killMemo;     // victim -> killer from killing-blow HitEvents (kept 10 s)
std::deque<KillMemo> g_deathMemo;    // (victim, 0) of emitted deaths (dedupe, 3 s)

std::string GuidStr(const uint8_t* g) {
    char b[40];
    snprintf(b, sizeof b, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[0], g[1], g[2], g[3],
             g[4], g[5], g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return b;
}

// Entity template code for a raw 16-byte GUID; tries RFC byte order and the mixed-endian (bytes_le) layout.
const EntityDef* EntityByGuid(const uint8_t* raw, const char** form) {
    uint8_t le[16];
    le[0] = raw[3], le[1] = raw[2], le[2] = raw[1], le[3] = raw[0], le[4] = raw[5], le[5] = raw[4], le[6] = raw[7],
    le[7] = raw[6];
    memcpy(le + 8, raw + 8, 8);
    for (size_t i = 0; i < kEntityCount; i++) {
        if (memcmp(kEntities[i].guid, raw, 16) == 0) {
            *form = "be";
            return &kEntities[i];
        }
        if (memcmp(kEntities[i].guid, le, 16) == 0) {
            *form = "le";
            return &kEntities[i];
        }
    }
    return nullptr;
}

std::string PlayerJsonForSlot(uint64_t server, int slot, std::string* nameOut = nullptr) {
    std::string name = SlotName(Slot(server, slot)), json;
    if (nameOut) *nameOut = name;
    if (!name.empty() && PluginState::Get().PlayerJsonByName(name, json)) return json;
    return "";
}

volatile LONG64 g_lastCombatSys = 0;

uint64_t __fastcall CombatDetour(uint64_t sys, uint64_t b, uint64_t c, uint64_t d) {
    InterlockedIncrement64(&g_combatTicks);
    InterlockedExchange64(&g_lastCombatSys, (LONG64)sys);
    uint64_t server = (uint64_t)g_server;
    if (server && g_getCtx) {
        uint8_t ctx[0x68];
        memset(ctx, 0, sizeof ctx);
        g_getCtx(sys, ctx, sizeof ctx);
        uint64_t hits = *(uint64_t*)(ctx + L.ctxHits);
        uint64_t died = *(uint64_t*)(ctx + L.ctxDied);
        uint64_t now = NowMs();
        if (hits && Readable(hits, 0x30)) {
            uint64_t data = *(uint64_t*)(hits + 0x18);
            uint32_t n = *(uint32_t*)(hits + 0x28);
            if (n && n < 4096 && data && Readable(data, (size_t)n * 0xe8)) {
                InterlockedExchangeAdd64(&g_hitsSeen, n);
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t* e = (const uint8_t*)(data + (uint64_t)i * 0xe8);
                    uint32_t flags = *(uint32_t*)(e + 0x98);
                    if (g_hitsSeen < 400)
                        PluginLog("hit: src=%u rootSrc=%u target=%u rootTarget=%u flags=0x%x healthChange=%d", *(uint32_t*)(e + 0xa4),
                                  *(uint32_t*)(e + 0x90), *(uint32_t*)(e + 0x94), *(uint32_t*)(e + 0xa0), flags, *(int32_t*)(e + 0xbc));
                    if (!(flags & 0x2000)) continue;  // HitEventFlags::WasKillingBlow
                    uint32_t killer = *(uint32_t*)(e + 0x90), victim = *(uint32_t*)(e + 0x94);
                    {
                        Guard g(g_killLock);
                        g_killMemo.push_back({victim, killer, now});
                        while (!g_killMemo.empty() && now - g_killMemo.front().ms > 10000) g_killMemo.pop_front();
                    }
                    int ks = SlotByEntity(server, killer);
                    if (ks < 0 || SlotByEntity(server, victim) >= 0) continue;  // player kills of non-players only
                    std::string killerName, pj = PlayerJsonForSlot(server, ks, &killerName);
                    std::string code = "entity#" + std::to_string(victim), guidS, form = "none";
                    bool actor = !g_tmplOf;  // without the template lookup every killing blow is reported
                    if (g_tmplOf) {
                        uint8_t guid[16] = {0};
                        g_tmplOf(sys, guid, victim);
                        guidS = GuidStr(guid);
                        const char* f = "none";
                        if (const EntityDef* ed = EntityByGuid(guid, &f)) {
                            code = ed->code;
                            actor = true;
                        }
                        form = f;
                    }
                    if (!actor) {
                        // destructible props/voxel objects also take killing blows (live: ids like 2372141572 with
                        // templates outside the actor table); they are not creatures, so no entity-killed event
                        if (g_hitsSeen < 400)
                            PluginLog("kill ignored (not an actor template): victim=%u guid=%s", victim, guidS.c_str());
                        continue;
                    }
                    InterlockedIncrement64(&g_kills);
                    std::string dataJson = "{\"entity\":" + JsonStr(code) + ",\"weapon\":\"\",\"victimEntityId\":" +
                                           std::to_string(victim) + ",\"templateGuid\":" + JsonStr(guidS) +
                                           ",\"killerName\":" + JsonStr(killerName) +
                                           ",\"weaponCategory\":" + std::to_string(*(uint32_t*)(e + 0xd8));
                    if (!pj.empty()) dataJson += ",\"player\":" + pj;
                    dataJson += "}";
                    PluginState::Get().EmitEvent("entity-killed", dataJson);
                    PluginLog("kill: killer=%u (%s) victim=%u code=%s guid=%s form=%s", killer, killerName.c_str(),
                              victim, code.c_str(), guidS.c_str(), form.c_str());
                }
            }
        }
        if (died && Readable(died, 0x30)) {
            uint64_t data = *(uint64_t*)(died + 0x18);
            uint32_t n = *(uint32_t*)(died + 0x28);
            if (n && n < 4096 && data && Readable(data, (size_t)n * 0x10)) {
                InterlockedExchangeAdd64(&g_diedSeen, n);
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t dead = *(uint32_t*)(data + (uint64_t)i * 0x10 + 8);
                    int ps = SlotByEntity(server, dead);
                    if (ps < 0) continue;
                    uint32_t killer = 0;
                    {
                        Guard g(g_killLock);
                        bool dup = false;
                        for (auto& m : g_deathMemo)
                            if (m.victim == dead && now - m.ms < 3000) dup = true;
                        while (!g_deathMemo.empty() && now - g_deathMemo.front().ms > 3000) g_deathMemo.pop_front();
                        if (dup) continue;
                        g_deathMemo.push_back({dead, 0, now});
                        for (auto& m : g_killMemo)
                            if (m.victim == dead) killer = m.killer;
                    }
                    std::string name, pj = PlayerJsonForSlot(server, ps, &name);
                    std::string dataJson = "{\"entityId\":" + std::to_string(dead) + ",\"playerName\":" + JsonStr(name);
                    if (!pj.empty()) dataJson += ",\"player\":" + pj;
                    CompRef t = Read(World(server), kCurrentTransform, dead);
                    if (t.ptr && t.size >= 24) {
                        char pb[160];
                        snprintf(pb, sizeof pb, ",\"position\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                                 FromFixed(*(int64_t*)t.ptr), FromFixed(*(int64_t*)((uint8_t*)t.ptr + 8)),
                                 FromFixed(*(int64_t*)((uint8_t*)t.ptr + 16)));
                        dataJson += pb;
                    }
                    if (killer) {
                        int as = SlotByEntity(server, killer);
                        std::string aj = as >= 0 ? PlayerJsonForSlot(server, as) : "";
                        if (!aj.empty()) dataJson += ",\"attacker\":" + aj;
                        else if (g_tmplOf) {
                            uint8_t guid[16] = {0};
                            g_tmplOf(sys, guid, killer);
                            const char* f = "none";
                            const EntityDef* ed = EntityByGuid(guid, &f);
                            dataJson += ",\"killerEntity\":" + JsonStr(ed ? ed->code : "entity#" + std::to_string(killer));
                        }
                    }
                    dataJson += "}";
                    InterlockedIncrement64(&g_deaths);
                    PluginState::Get().EmitEvent("player-death", dataJson);
                    PluginLog("death: player=%s eid=%u killer=%u", name.c_str(), dead, killer);
                }
            }
        }
    }
    return g_origCombat(sys, b, c, d);
}

// ---- resolution ----
uint32_t g_updRva = 0, g_invJobRva = 0, g_isAdminRva = 0, g_addMsgRva = 0, g_combatRva = 0;
bool g_worldOk = false, g_teleportOk = false, g_giveOk = false, g_chatSendOk = false, g_chatRecvOk = false,
     g_combatOk = false;

uint32_t Unique(const char* pattern, const char* what, std::string& how) {
#ifdef TAKARO_DEBUG_CORRUPT_SIG
    // Debug builds only (build.sh DEBUG_CORRUPT_SIG=<name>): corrupt one signature to exercise the degrade path.
    std::string corrupted;
    if (strcmp(what, TAKARO_DEBUG_CORRUPT_SIG) == 0) {
        corrupted = std::string("CC CC CC CC ") + pattern;
        pattern = corrupted.c_str();
        PluginLog("DEBUG: signature '%s' deliberately corrupted", what);
    }
#endif
    auto hits = scan::FindPattern(pattern, 3);
    if (hits.size() != 1) {
        how = std::string(what) + " pattern hits=" + std::to_string(hits.size());
        return 0;
    }
    return hits[0];
}

bool ResolveWorldCore(std::string& how) {
    uint32_t sys = ModerationSystemRva();
    if (!sys) {
        how = "server network update (moderation system) unresolved";
        return false;
    }
    // updatePlayers: unique prologue; its caller must be the network update (functions are split into chunks, so
    // compare call-site function roots), called with `mov rcx, r14` (server)
    std::vector<uint32_t> upd;
    for (uint32_t t : scan::FindPattern("48 8B C4 55 53 56 41 55 41 56 41 57 48 8D A8 ?? ?? FF FF", 64)) {
        bool ok = false;
        for (uint32_t c : scan::FindDirectCalls(t)) ok = ok || (scan::FunctionRoot(c) == sys && scan::MatchAt(c - 3, "49 8B CE"));
        if (ok) upd.push_back(t);
    }
    if (upd.size() != 1) {
        how = "updatePlayers candidates=" + std::to_string(upd.size());
        return false;
    }
    g_updRva = upd[0];
    // slot stride/base: `imul rsi, r15, <stride>; mov [rsp+..], eax; add rsi, <base>`
    uint32_t slHit = 0;
    for (uint32_t a = g_updRva; a < g_updRva + 0x400 && !slHit; a++)
        if (scan::MatchAt(a, "49 69 F7 ?? ?? ?? 00 89 44 24 ?? 48 81 C6 ?? ?? 00 00")) slHit = a;
    if (!slHit) {
        how = "slot stride pattern not found in updatePlayers";
        return false;
    }
    memcpy(&L.slotStride, scan::Ptr(slHit + 3), 4);
    memcpy(&L.slotBase, scan::Ptr(slHit + 14), 4);
    // getComponentRead: two functions share a prologue; the read one's second call matches the read core
    auto gc = scan::FindPattern("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D BA ?? ?? ?? 00 44 89 44 24 40 48 8B F1 "
                                "48 8D 54 24 40 48 8B 0F 41 8B D9 48 81 C1 88 00 00 00 E8 ?? ?? ?? ?? 4C 8B C0 44 8B CB 48 8B D7 48 8B CE E8",
                                4);
    uint32_t readFn = 0;
    int readHits = 0;
    for (uint32_t f : gc) {
        uint32_t core = scan::CallDest(f + 0x41);
        if (core && scan::MatchAt(core, "48 89 5C 24 08 48 89 74 24 10 44 89 4C 24 20 57 48 83 EC 20 48 8D BA 48 01 00 00")) {
            readFn = f;
            readHits++;
        }
    }
    if (readHits != 1) {
        how = "getComponentRead candidates=" + std::to_string(readHits) + " of " + std::to_string(gc.size());
        return false;
    }
    // world offset: updatePlayers calls getComponentRead with `mov rdx, [r14+<worldOff>]` right before the call
    for (uint32_t a : scan::FindDirectCalls(readFn))
        if (scan::FunctionRoot(a) == g_updRva && scan::MatchAt(a - 7, "49 8B 96")) {
            memcpy(&L.worldOff, scan::Ptr(a - 4), 4);
            break;
        }
    if (!L.worldOff || L.worldOff > 0x10000) {
        how = "world offset not found at a getComponentRead call in updatePlayers";
        return false;
    }
    g_getRead = (GetCompFn)scan::Ptr(readFn);
    char b[256];
    snprintf(b, sizeof b, "updatePlayers=0x%x getComponentRead=0x%x world=+0x%x slots=+0x%x*0x%x", g_updRva, readFn,
             L.worldOff, L.slotBase, L.slotStride);
    how = b;
    RecordResolve("getCompRead", readFn, "shared prologue + read-core call", false);
    return true;
}

}  // namespace

void WorldInitCapabilities() {
    auto& st = PluginState::Get();
    for (auto* c : {"playerLocation", "playerInventory", "teleport", "giveItem", "sendMessage", "chatEvents", "deathEvents",
                    "killEvents"})
        st.SetCapability(c, "degraded", "initializing");
    st.SetCapability("listItems", "ok", "static items table (" + std::to_string(kItemCount) + " ItemInfo)");
    st.SetCapability("listEntities", "ok", "static actor templates (" + std::to_string(kEntityCount) + ")");
    st.SetCapability("listLocations", "ok", "static map markers/spawn points (" + std::to_string(kLocationCount) + ")");
    st.SetCapability("executeCommand", "ok", "plugin-defined command set (see /command help)");
}

void WorldHooksInit() {
    auto& st = PluginState::Get();
    std::string how, err;

    // --- core: updatePlayers queue + component read ---
    if (ResolveWorldCore(how)) {
        RecordResolve("updatePlayers", g_updRva, how, false);
        if (InstallHook(g_updRva, (LPVOID)&UpdDetour, (LPVOID*)&g_origUpd, err)) {
            RecordResolve("updatePlayers", g_updRva, "hook installed", true);
            g_worldOk = true;
        } else {
            how = err;
        }
    } else {
        RecordResolve("updatePlayers", g_updRva, how, false);
    }
    std::string coreErr = g_worldOk ? "" : "world core: " + how;
    for (auto* c : {"playerLocation", "playerInventory"})
        st.SetCapability(c, "degraded", g_worldOk ? "hook installed, waiting for first server tick" : coreErr);

    // --- teleport: World_addComponent(Teleport) ---
    uint32_t add = Unique("48 83 EC 38 41 0F 10 01 48 81 C1 ?? ?? ?? 00 4C 8D 4C 24 20", "addComponent", how);
    if (add) {
        bool calledByUpd = false;
        for (uint32_t a : scan::FindDirectCalls(add)) calledByUpd = calledByUpd || (g_updRva && scan::FunctionRoot(a) == g_updRva);
        if (calledByUpd) {
            g_addComp = (AddCompFn)scan::Ptr(add);
            g_teleportOk = g_worldOk;
            RecordResolve("addComponent", add, "unique pattern, called by updatePlayers", false);
        } else {
            how = "addComponent not called by updatePlayers";
        }
    }
    st.SetCapability("teleport", "degraded", g_teleportOk ? "waiting for first server tick" : (g_worldOk ? how : coreErr));

    // --- give: inventory_actions job + isAdminEntity bypass ---
    uint32_t isAdmin = Unique("8D 41 FF 4C 8B C2 3D FE 03 00 00 77 17 48 8D 14 40", "isAdminEntity", how);
    uint32_t inv = isAdmin ? Unique("40 55 41 56 48 8D AC 24 ?? ?? FF FF B8 ?? ?? 00 00 E8", "inventory_actions", how) : 0;
    std::string giveErr;
    if (isAdmin && inv) {
        for (uint32_t a = inv; a < inv + 0x200; a++)
            if (scan::CallDest(a) == isAdmin) {
                g_invGateRet = scan::Base() + a + 5;
                break;
            }
        auto ver = scan::FindPattern("8B 89 ?? ?? 00 00 39 88 ?? 00 00 00", 16);
        for (uint32_t h : ver)
            if (h > inv && h < inv + 0x300) {
                memcpy(&L.piCreateVer, scan::Ptr(h + 2), 4);
                memcpy(&L.scCreateVer, scan::Ptr(h + 8), 4);
                break;
            }
        char pat[64];
        snprintf(pat, sizeof pat, "48 8D B3 %02X %02X 00 00", (L.piCreateVer - 4) & 0xff, ((L.piCreateVer - 4) >> 8) & 0xff);
        bool leaOk = false;
        for (uint32_t a = inv; L.piCreateVer && a < inv + 0x300 && !leaOk; a++) leaOk = scan::MatchAt(a, pat);
        if (!g_invGateRet) giveErr = "isAdminEntity call not found in inventory_actions";
        else if (!L.piCreateVer || L.piCreateVer > 0x500 || L.scCreateVer > 0xd0) giveErr = "create-action version offsets not found";
        else if (!leaOk) giveErr = "create-action base (lea rsi,[rbx+ver-4]) self-check failed";
        else if (!g_worldOk) giveErr = coreErr;
        else if (!InstallHook(isAdmin, (LPVOID)&IsAdminDetour, (LPVOID*)&g_origIsAdmin, err)) giveErr = "isAdminEntity hook: " + err;
        else if (!InstallHook(inv, (LPVOID)&InvJobDetour, (LPVOID*)&g_origInvJob, err)) giveErr = "inventory_actions hook: " + err;
        else {
            g_isAdminRva = isAdmin;
            g_invJobRva = inv;
            g_giveOk = true;
            char b[160];
            snprintf(b, sizeof b, "gate ret=0x%llx PlayerInput+0x%x ServerConsumed+0x%x",
                     (unsigned long long)(g_invGateRet - scan::Base()), L.piCreateVer, L.scCreateVer);
            RecordResolve("isAdminEntity", isAdmin, "hook installed", true);
            RecordResolve("inventoryActions", inv, std::string("hook installed; ") + b, true);
        }
    } else {
        giveErr = how;
    }
    st.SetCapability("giveItem", "degraded", g_giveOk ? "waiting for first server tick" : giveErr);

    // --- chat: ChatSystem::addMessage (send + receive) ---
    uint32_t addMsg = Unique("40 55 57 41 56 41 57 48 81 EC 78 04 00 00 49 81 79 08 00 04 00 00", "addMessage", how);
    std::string chatErr = how;
    if (addMsg) {
        // receive call site: `mov byte [rbp+50h],0; mov byte [rbp+40h],0; call addMessage` inside processIncomingMessages,
        // which is a direct callee of the server network update; the chat system pointer is loaded ~0x50 bytes earlier.
        uint32_t sys = ModerationSystemRva();
        std::vector<uint32_t> sites;
        for (uint32_t c : scan::FindDirectCalls(addMsg)) {
            if (!scan::MatchAt(c - 8, "C6 45 50 00 C6 45 40 00")) continue;
            uint32_t root = scan::FunctionRoot(c);
            bool fromNetUpdate = false;
            for (uint32_t a : scan::FindDirectCalls(root)) fromNetUpdate = fromNetUpdate || (sys && scan::FunctionRoot(a) == sys);
            if (fromNetUpdate) sites.push_back(c);
        }
        if (sites.size() == 1) {
            for (uint32_t a = sites[0] - 0x80; a < sites[0]; a++)
                if (scan::MatchAt(a, "49 8B 55 ?? 4C 8D 4D")) {  // mov rdx,[r13+chat]; lea r9,[rbp+..]
                    L.chatOff = *(uint8_t*)scan::Ptr(a + 3);
                }
        }
        if (sites.size() != 1) chatErr = "player chat call site candidates=" + std::to_string(sites.size());
        else if (!L.chatOff) chatErr = "chat system offset not found before the receive call site";
        else if (!g_worldOk) chatErr = coreErr;
        else if (!InstallHook(addMsg, (LPVOID)&AddMsgDetour, (LPVOID*)&g_origAddMsg, err)) chatErr = "addMessage hook: " + err;
        else {
            g_addMsgRva = addMsg;
            g_recvRet = scan::Base() + sites[0] + 5;
            g_chatSendOk = g_chatRecvOk = true;
            char b[128];
            snprintf(b, sizeof b, "hook installed; receive ret=0x%x chat=+0x%x", sites[0] + 5, L.chatOff);
            RecordResolve("chatAddMessage", addMsg, b, true);
        }
    }
    st.SetCapability("sendMessage", "degraded", g_chatSendOk ? "waiting for first server tick" : chatErr);
    st.SetCapability("chatEvents", g_chatRecvOk ? "ok" : "degraded",
                     g_chatRecvOk ? "ChatSystem::addMessage detour (player chat call site)" : chatErr);

    // --- combat: combat_experience_source system (HitEvent + EntityDiedEvent views) ---
    uint32_t combat = Unique("48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? FF FF FF 48 81 EC ?? ?? 00 00 "
                             "41 B8 68 00 00 00 48 8D 55 00 4C 8B E9 E8 ?? ?? ?? ?? 4C 8B 7D 58",
                             "combat_experience_source", how);
    std::string combatErr = how;
    if (combat) {
        bool diedOk = false;
        for (uint32_t a = combat; a < combat + 0x300 && !diedOk; a++) diedOk = scan::MatchAt(a, "48 8B 55 50 45 33 FF");
        uint32_t tmpl = Unique("48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B DA 48 8D 54 24 40 44 89 44 24 40 48 8B 78 30 "
                               "48 8D 8F 48 01 00 00 E8",
                               "entityTemplateGuid", how);
        if (!diedOk) combatErr = "EntityDiedEvent view load (ctx+0x50) self-check failed";
        else if (!InstallHook(combat, (LPVOID)&CombatDetour, (LPVOID*)&g_origCombat, err)) combatErr = "combat hook: " + err;
        else {
            g_getCtx = (CtxFn)scan::Ptr(scan::CallDest(combat + 0x2c));
            if (tmpl) g_tmplOf = (TmplFn)scan::Ptr(tmpl);
            g_combatRva = combat;
            g_combatOk = true;
            RecordResolve("combatXpSource", combat, std::string("hook installed; template lookup ") + (tmpl ? Hx(tmpl) : "missing"), true);
        }
    }
    st.SetCapability("deathEvents", g_combatOk ? "ok" : "degraded",
                     g_combatOk ? "combat_experience_source detour (EntityDiedEvent of player entities)" : combatErr);
    st.SetCapability("killEvents", g_combatOk ? "ok" : "degraded",
                     g_combatOk ? std::string("combat_experience_source detour (killing-blow HitEvent)") +
                                      (g_tmplOf ? "" : "; entity template lookup unresolved")
                                : combatErr);
}

void WorldHousekeep() {
    static bool promoted = false;
    if (promoted || !g_updTicks) return;
    promoted = true;
    auto& st = PluginState::Get();
    if (g_worldOk) {
        st.SetCapability("playerLocation", "ok", "CurrentTransform via updatePlayers detour");
        st.SetCapability("playerInventory", "ok", "InventorySetup -> Inventory via updatePlayers detour");
    }
    if (g_teleportOk) st.SetCapability("teleport", "ok", "Teleport component via World_addComponent");
    if (g_giveOk) st.SetCapability("giveItem", "ok", "admin inventory-create action + isAdminEntity bypass");
    if (g_chatSendOk) st.SetCapability("sendMessage", "ok", "ChatSystem::addMessage broadcast (+recipient done-mask)");
}

std::string WorldDiagnosticsJson() {
    char b[640];
    snprintf(b, sizeof b,
             "{\"updTicks\":%lld,\"invJobTicks\":%lld,\"combatTicks\":%lld,\"chatSeen\":%lld,\"bypassHits\":%lld,"
             "\"hitsSeen\":%lld,\"diedSeen\":%lld,\"kills\":%lld,\"deaths\":%lld,\"giveState\":%ld,"
             "\"layout\":{\"world\":%u,\"slotBase\":%u,\"slotStride\":%u,\"chat\":%u,\"piCreateVer\":%u,\"scCreateVer\":%u}}",
             (long long)g_updTicks, (long long)g_invJobTicks, (long long)g_combatTicks, (long long)g_chatSeen,
             (long long)g_bypassHits, (long long)g_hitsSeen, (long long)g_diedSeen, (long long)g_kills,
             (long long)g_deaths, (long)g_giveState, L.worldOff, L.slotBase, L.slotStride, L.chatOff, L.piCreateVer,
             L.scCreateVer);
    return b;
}

// ------------------------------------------------------------------------------------------------
bool WorldGetLocation(const std::string& name, Vec3& out, std::string& err) {
    if (!g_worldOk) {
        err = "player location unavailable (world hooks unresolved)";
        return false;
    }
    std::string res, e2;
    if (!RunOnWorld(
            [name](uint64_t server) -> std::string {
                uint32_t eid;
                std::string e;
                if (!PlayerEntity(server, name, eid, e)) return "!" + e;
                CompRef r = Read(World(server), kCurrentTransform, eid);
                if (!r.ptr || r.size < 24) return "!player entity has no CurrentTransform";
                char b[128];
                snprintf(b, sizeof b, "%.17g %.17g %.17g", FromFixed(*(int64_t*)r.ptr),
                         FromFixed(*(int64_t*)((uint8_t*)r.ptr + 8)), FromFixed(*(int64_t*)((uint8_t*)r.ptr + 16)));
                return b;
            },
            res, e2)) {
        err = e2;
        return false;
    }
    if (!res.empty() && res[0] == '!') {
        err = res.substr(1);
        return false;
    }
    return sscanf(res.c_str(), "%lf %lf %lf", &out.x, &out.y, &out.z) == 3 || (err = "bad transform", false);
}

bool WorldTeleport(const std::string& name, const Vec3& to, std::string& err) {
    if (!g_teleportOk) {
        err = "teleport unavailable (addComponent/updatePlayers unresolved)";
        return false;
    }
    if (!std::isfinite(to.x) || !std::isfinite(to.y) || !std::isfinite(to.z) || fabs(to.x) > 1e6 || fabs(to.y) > 1e6 ||
        fabs(to.z) > 1e6) {
        err = "coordinates out of range";
        return false;
    }
    std::string res, e2;
    if (!RunOnWorld(
            [name, to](uint64_t server) -> std::string {
                uint32_t eid;
                std::string e;
                if (!PlayerEntity(server, name, eid, e)) return "!" + e;
                uint64_t world = World(server);
                if (Read(world, kTeleport, eid).ptr) return "!a teleport is already pending for this player";
                uint8_t t[0x50];
                memset(t, 0, sizeof t);
                CompRef cur = Read(world, kCurrentTransform, eid);
                if (cur.ptr && cur.size >= 0x38) {
                    memcpy(t, cur.ptr, 0x38);  // keep orientation and scale
                } else {
                    float one = 1.0f;
                    memcpy(t + 0x24, &one, 4);  // quat w
                    for (int k = 0; k < 3; k++) memcpy(t + 0x28 + 4 * k, &one, 4);
                }
                int64_t px = ToFixed(to.x), py = ToFixed(to.y), pz = ToFixed(to.z);
                memcpy(t, &px, 8);
                memcpy(t + 8, &py, 8);
                memcpy(t + 16, &pz, 8);
                t[0x49] = 1;  // searchBestSpawnPosition: voxel-safe placement (same as the admin teleport)
                Span sp{t, sizeof t};
                uint64_t ok = g_addComp(world, eid, kTeleport, &sp);
                PluginLog("teleport: %s eid=%u -> (%.2f, %.2f, %.2f) addComponent=%u", name.c_str(), eid, to.x, to.y,
                          to.z, (unsigned)(ok & 0xff));
                return "ok";
            },
            res, e2)) {
        err = e2;
        return false;
    }
    if (res != "ok") {
        err = res.substr(1);
        return false;
    }
    return true;
}

bool WorldInventoryJson(const std::string& name, std::string& json, std::string& err) {
    if (!g_worldOk) {
        err = "inventory unavailable (world hooks unresolved)";
        return false;
    }
    std::string res, e2;
    if (!RunOnWorld(
            [name](uint64_t server) -> std::string {
                uint32_t eid;
                std::string e;
                if (!PlayerEntity(server, name, eid, e)) return "!" + e;
                uint64_t world = World(server);
                CompRef su = Read(world, kInventorySetup, eid);
                if (!su.ptr || su.size < 0x50) return "!player entity has no InventorySetup";
                std::string o = "[";
                bool first = true;
                for (int k = 0; k < 16; k++) {
                    uint32_t inv = *(uint32_t*)((uint8_t*)su.ptr + 4 * k);
                    uint8_t cat = *(uint8_t*)((uint8_t*)su.ptr + 0x40 + k);
                    if (!inv) continue;
                    CompRef r = Read(world, kInventory, inv);
                    if (!r.ptr || r.size < 12) continue;
                    size_t stacks = (size_t)(r.size / 12);
                    for (size_t j = 0; j < stacks; j++) {
                        const uint32_t* st = (const uint32_t*)((uint8_t*)r.ptr + j * 12);
                        if (!st[0] || !st[1]) continue;
                        const ItemDef* d = nullptr;
                        for (size_t q = 0; q < kItemCount && !d; q++)
                            if (kItems[q].itemId == st[0]) d = &kItems[q];
                        char idHex[16];
                        snprintf(idHex, sizeof idHex, "0x%08x", st[0]);
                        std::string code = d ? d->code : idHex;
                        o += std::string(first ? "" : ",") + "{\"code\":" + JsonStr(code) + ",\"name\":" +
                             JsonStr(d ? d->name : code) + ",\"amount\":" + std::to_string(st[1]) + ",\"inventory\":" +
                             JsonStr(cat < 6 ? kInvCategory[cat] : "unknown") + ",\"slot\":" + std::to_string(j);
                        if (st[2]) {
                            CompRef is = Read(world, kItemState, st[2]);
                            if (is.ptr && is.size >= 18) {
                                uint8_t rar = *((uint8_t*)is.ptr + 16), lvl = *((uint8_t*)is.ptr + 17);
                                o += ",\"quality\":" + JsonStr(std::to_string(lvl)) + ",\"rarity\":" +
                                     JsonStr(rar < 5 ? kRarity[rar] : std::to_string(rar));
                            }
                        }
                        o += "}";
                        first = false;
                    }
                }
                return o + "]";
            },
            res, e2)) {
        err = e2;
        return false;
    }
    if (!res.empty() && res[0] == '!') {
        err = res.substr(1);
        return false;
    }
    json = res;
    return true;
}

bool WorldGiveItem(const std::string& name, const ItemDef& item, uint32_t amount, std::string& err, std::string& detail) {
    if (!g_giveOk) {
        err = "giveItem unavailable (inventory_actions/isAdminEntity unresolved)";
        return false;
    }
    Guard serial(g_giveSerial);
    uint32_t eid = 0;
    std::string res, e2;
    if (!RunOnWorld(
            [name](uint64_t server) -> std::string {
                uint32_t id;
                std::string e;
                if (!PlayerEntity(server, name, id, e)) return "!" + e;
                return std::to_string(id);
            },
            res, e2)) {
        err = e2;
        return false;
    }
    if (res.empty() || res[0] == '!') {
        err = res.empty() ? "player lookup failed" : res.substr(1);
        return false;
    }
    eid = (uint32_t)strtoul(res.c_str(), nullptr, 10);
    // AdminInventoryCreate ignores `count`: each action creates exactly 1, or maxStackSize with createFullStack.
    // So `amount` = full-stack actions + single-item actions. PIDE items (unique instances) always come one by one.
    uint32_t maxStack = item.pide ? 1 : (item.maxStack ? item.maxStack : 1);
    uint32_t fullActions = maxStack > 1 ? amount / maxStack : 0;
    uint32_t singles = maxStack > 1 ? amount % maxStack : amount;
    uint32_t actions = fullActions + singles;
    if (actions > 64) {
        err = "amount needs " + std::to_string(actions) + " create actions (max 64 per request; use multiples of the stack size " +
              std::to_string(maxStack) + ")";
        return false;
    }
    uint32_t left = amount;
    for (uint32_t k = 0; k < actions; k++) {
        bool full = k < fullActions;
        uint16_t count = (uint16_t)(full ? maxStack : 1);
        {
            Guard g(g_giveLock);
            g_give = Give{};
            g_give.eid = eid;
            g_give.itemId = item.itemId;
            g_give.count = count;
            g_give.fullStack = full;
            InterlockedExchange(&g_giveState, kGiveActive);
        }
        uint64_t t0 = NowMs();
        while (g_giveState == kGiveActive && NowMs() - t0 < 4000) Sleep(10);
        Give snap;
        {
            Guard g(g_giveLock);
            snap = g_give;
            if (g_giveState == kGiveActive) InterlockedExchange(&g_giveState, kGiveIdle);
            InterlockedExchange(&g_bypassEid, 0);
        }
        char b[200];
        snprintf(b, sizeof b, "action %u/%u: item=0x%08x count=%u version=%u injections=%d consumed=%s", k + 1, actions,
                 item.itemId, count, snap.version, snap.injections, g_giveState == kGiveDone ? "yes" : "no");
        PluginLog("give: %s eid=%u %s", name.c_str(), eid, b);
        detail += (detail.empty() ? "" : "; ") + std::string(b);
        if (g_giveState != kGiveDone) {
            err = snap.injections ? "the game did not consume the create action within 4 s"
                                  : "inventory_actions never ran with the player's input (no injection)";
            InterlockedExchange(&g_giveState, kGiveIdle);
            return false;
        }
        InterlockedExchange(&g_giveState, kGiveIdle);
        left -= count < left ? count : left;
    }
    return true;
}

bool WorldSendMessage(const std::string& text, const std::string& recipientMachine, uint8_t type, uint32_t senderHandle,
                      std::string& err) {
    if (!g_chatSendOk) {
        err = "sendMessage unavailable (chat hooks unresolved)";
        return false;
    }
    if (text.empty() || text.size() > 0x400) {
        err = "message must be 1..1024 bytes";
        return false;
    }
    int target = recipientMachine.empty() ? -1 : atoi(recipientMachine.c_str());
    if (target > 31) {
        err = "recipient machine index out of range";
        return false;
    }
    std::string res, e2;
    if (!RunOnWorld(
            [text, target, type, senderHandle](uint64_t server) -> std::string {
                uint64_t chat = *(uint64_t*)(server + L.chatOff);
                if (!chat || !Readable(chat, 0x1e0)) return "!chat system not available";
                if (target >= 0 && !*(uint8_t*)(chat + 0x14 + (uint64_t)target * 0xc))
                    return "!recipient machine is not an active chat recipient";
                // The client only renders free text for chat type 0 with a valid player handle, shown as
                // "<character name>: text" (live-tested: handle 0 and types 1-3 are dropped or become canned notices).
                // So the message is carried by the recipient's own handle (whisper) or the first logged-in player.
                uint32_t handle = senderHandle;
                for (int i = 0; i < 16 && !handle; i++) {
                    uint64_t s = Slot(server, i);
                    uint32_t h = *(uint32_t*)s;
                    if (!h || *(uint8_t*)(s + kSlotState) != 3) continue;
                    if (target >= 0 && (int)(*(uint32_t*)(s + 4) & 0x7f) != target) continue;
                    handle = h;
                }
                if (!handle) return std::string(target >= 0 ? "!recipient has no logged-in player slot" : "=nobody online");
                uint64_t countBefore = *(uint64_t*)(chat + 0x1d8);
                StringView sv{text.data(), text.size()};
                Opt64 o1{}, o2{};
                uint8_t e = 0;
                g_origAddMsg(&e, chat, type, &sv, 0, 0, handle, &o1, &o2);
                if (e) return "!addMessage error " + std::to_string(e);
                uint64_t rec = *(uint64_t*)(chat + 0x1d0);
                if (*(uint64_t*)(chat + 0x1d8) != countBefore + 1 || !rec) return "!message was not queued";
                int excluded = 0;
                if (target >= 0) {
                    // whisper: mark the record as already delivered to every other active machine (mirrors the flush's
                    // per-machine "sent" bookkeeping: done bit + pending byte count)
                    uint32_t size = *(uint32_t*)(rec + 8) + 0xf;
                    if (size & 3) size = size - (size & 3) + 4;
                    for (int k = 0; k < 32; k++) {
                        if (k == target || !*(uint8_t*)(chat + 0x14 + (uint64_t)k * 0xc)) continue;
                        uint32_t* mask = (uint32_t*)(rec + 0x20);
                        if (*mask & (1u << k)) continue;
                        *mask |= (1u << k);
                        *(uint32_t*)(chat + 0x1c + (uint64_t)k * 0xc) -= size;
                        excluded++;
                    }
                }
                PluginLog("message: queued type=%u handle=0x%x len=%zu target=%d excluded=%d", type, handle,
                          text.size(), target, excluded);
                return "ok";
            },
            res, e2)) {
        err = e2;
        return false;
    }
    if (res == "=nobody online") return true;  // nothing to deliver
    if (res != "ok") {
        err = res.substr(1);
        return false;
    }
    return true;
}

bool WorldSlotsJson(std::string& json, std::string& err) {
    if (!g_worldOk) {
        err = "world hooks unresolved";
        return false;
    }
    return RunOnWorld(
        [](uint64_t server) -> std::string {
            std::string o = "[";
            bool first = true;
            for (int i = 0; i < 16; i++) {
                uint64_t s = Slot(server, i);
                uint32_t h = *(uint32_t*)s;
                if (!h) continue;
                char b[200];
                snprintf(b, sizeof b, "{\"slot\":%d,\"handle\":%u,\"machineHandle\":%u,\"loginState\":%u,\"perms\":%u,\"entityId\":%u,",
                         i, h, *(uint32_t*)(s + 4), *(uint8_t*)(s + kSlotState), *(uint8_t*)(s + 0xb),
                         *(uint32_t*)(s + kSlotEid));
                o += std::string(first ? "" : ",") + b + "\"name\":" + JsonStr(SlotName(s)) + "}";
                first = false;
            }
            return o + "]";
        },
        json, err);
}

bool WorldNearbyJson(const std::string& name, double radius, std::string& json, std::string& err) {
    if (!g_worldOk) {
        err = "world hooks unresolved";
        return false;
    }
    std::string res;
    if (!RunOnWorld(
            [name, radius](uint64_t server) -> std::string {
                uint32_t eid;
                std::string e;
                if (!PlayerEntity(server, name, eid, e)) return "!" + e;
                uint64_t world = World(server);
                CompRef me = Read(world, kCurrentTransform, eid);
                if (!me.ptr) return "!no transform";
                double px = FromFixed(*(int64_t*)me.ptr), py = FromFixed(*(int64_t*)((uint8_t*)me.ptr + 8)),
                       pz = FromFixed(*(int64_t*)((uint8_t*)me.ptr + 16));
                static const std::pair<uint32_t, const char*> kinds[] = {
                    {0x422798fd, "enemy"}, {0x8fb61d93, "animal"}, {0x072dbdb7, "npc"}, {0x9475619e, "boss"}, {0xa1f30f21, "faction"}};
                struct Hit {
                    double d;
                    std::string json;
                };
                std::vector<Hit> hits;
                uint64_t sys = (uint64_t)g_lastCombatSys;
                for (uint32_t id = 1; id < 0x20000; id++) {
                    CompRef t = Read(world, kCurrentTransform, id);
                    if (!t.ptr || t.size < 24 || id == eid) continue;
                    double x = FromFixed(*(int64_t*)t.ptr), y = FromFixed(*(int64_t*)((uint8_t*)t.ptr + 8)),
                           z = FromFixed(*(int64_t*)((uint8_t*)t.ptr + 16));
                    double d = sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
                    if (d > radius) continue;
                    CompRef hp = Read(world, 0x216eb5f9 /*NetworkHealth*/, id);
                    if (!hp.ptr) continue;  // only things that can die
                    std::string tags;
                    for (auto& k : kinds)
                        if (Read(world, k.first, id).ptr) tags += std::string(tags.empty() ? "" : ",") + "\"" + k.second + "\"";
                    std::string code;
                    if (sys && g_tmplOf) {
                        uint8_t guid[16] = {0};
                        g_tmplOf(sys, guid, id);
                        const char* f = "none";
                        if (const EntityDef* ed = EntityByGuid(guid, &f)) code = ed->code;
                    }
                    char b[240];
                    snprintf(b, sizeof b, "{\"entityId\":%u,\"distance\":%.1f,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"health\":[%d,%d],\"tags\":[",
                             id, d, x, y, z, *(int32_t*)hp.ptr, hp.size >= 8 ? *(int32_t*)((uint8_t*)hp.ptr + 4) : 0);
                    hits.push_back({d, b + tags + "],\"code\":" + JsonStr(code) + "}"});
                }
                std::sort(hits.begin(), hits.end(), [](const Hit& l, const Hit& r) { return l.d < r.d; });
                std::string o = "[";
                for (size_t i = 0; i < hits.size() && i < 80; i++) o += (i ? "," : "") + hits[i].json;
                return o + "]";
            },
            res, err, 5000))
        return false;
    if (!res.empty() && res[0] == '!') {
        err = res.substr(1);
        return false;
    }
    json = res;
    return true;
}

// ------------------------------------------------------------------------------------------------
const ItemDef* FindItemByCode(const std::string& code) {
    for (size_t i = 0; i < kItemCount; i++)
        if (code == kItems[i].code) return &kItems[i];
    for (size_t i = 0; i < kItemCount; i++)
        if (_stricmp(code.c_str(), kItems[i].code) == 0) return &kItems[i];
    char* end = nullptr;
    unsigned long long v = strtoull(code.c_str(), &end, 0);
    if (end && *end == 0 && !code.empty())
        for (size_t i = 0; i < kItemCount; i++)
            if (kItems[i].itemId == v) return &kItems[i];
    return nullptr;
}

const std::string& ItemsJson() {
    static std::string s;
    static SrwLock l;
    Guard g(l);
    if (s.empty()) {
        s = "[";
        for (size_t i = 0; i < kItemCount; i++)
            s += std::string(i ? "," : "") + "{\"code\":" + JsonStr(kItems[i].code) + ",\"name\":" + JsonStr(kItems[i].name) +
                 ",\"description\":" + JsonStr(std::string(kItems[i].category) + ", " + kItems[i].rarity) +
                 ",\"itemId\":" + std::to_string(kItems[i].itemId) + ",\"maxStackSize\":" + std::to_string(kItems[i].maxStack) + "}";
        s += "]";
    }
    return s;
}

const std::string& EntitiesJson() {
    static std::string s;
    static SrwLock l;
    Guard g(l);
    if (s.empty()) {
        s = "[";
        for (size_t i = 0; i < kEntityCount; i++)
            s += std::string(i ? "," : "") + "{\"code\":" + JsonStr(kEntities[i].code) + ",\"name\":" +
                 JsonStr(kEntities[i].code) + ",\"type\":" + JsonStr(kEntities[i].type) + ",\"description\":" +
                 JsonStr(std::string("faction ") + kEntities[i].faction + ", family " + kEntities[i].family) + "}";
        s += "]";
    }
    return s;
}

const std::string& LocationsJson() {
    static std::string s;
    static SrwLock l;
    Guard g(l);
    if (s.empty()) {
        s = "[";
        char b[160];
        for (size_t i = 0; i < kLocationCount; i++) {
            auto& x = kLocations[i];
            snprintf(b, sizeof b, "\"position\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}", x.x, x.y, x.z);
            s += std::string(i ? "," : "") + "{\"code\":" + JsonStr(x.code) + ",\"name\":" + JsonStr(x.code) + ",\"kind\":" +
                 JsonStr(x.kind) + (x.spawnType[0] ? ",\"spawnType\":" + JsonStr(x.spawnType) : std::string()) + "," + b + "}";
        }
        s += "]";
    }
    return s;
}
