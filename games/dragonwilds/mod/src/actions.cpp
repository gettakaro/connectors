// Lane L3: the 15 action capabilities behind the native Takaro bridge (and diagnostic HTTP).
//
// Every handler that touches a UObject runs its work in an owned GameThread::Run job; the calling
// background thread (native bridge action worker or diagnostic HTTP) builds the JSON out of what
// the job copied out. Nothing here hard-codes a game struct offset: every
// UPROPERTY is looked up through UStruct::FindPropertyByName at runtime and cached per class, and
// every function address comes from sym.cpp. A missing symbol or property degrades one capability
// with a reason; it never throws out of a handler and never takes the server down.
#include "actions.h"

#include "eos.h"
#include "events.h"
#include "gamethread.h"
#include "perf.h"
#include "reflect.h"
#include "state.h"
#include "sym.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

using UE::FName;
using UE::FString;
using UE::TArray;

namespace {

// Ban/unban jobs that are queued or running on the game thread, including ones whose caller timed
// out. Timed-ban recovery must wait for this to reach zero before it reads the lists back.
std::atomic<size_t> g_pendingBanJobs{0};
struct BanJobLifetime {
    BanJobLifetime() { ++g_pendingBanJobs; }
    ~BanJobLifetime() { --g_pendingBanJobs; }
};

// ---------------------------------------------------------------------------------------------
// resolved game functions (all SysV direct calls; every one may be null)

template <typename T>
T Fn(const char* sym) {
    return (T)(uintptr_t)Sym::Addr(sym);
}

// FString/FText/TArray returns are non-trivial types: SysV returns them through a hidden pointer.
using FnNetIdToString = void (*)(FString* ret, const void* self);
using FnTextFromString = void (*)(void* retFText, const FString* s);
using FnTextDisplayString = const FString* (*)(const void* ftext);
using FnKickBan = void (*)(void* session, void* pc, const void* ftext);
using FnTryGiveItem = bool (*)(const void* pc, const void* itemData, int count);
using FnTeleportTo = bool (*)(void* actor, const double* loc, const double* rot, bool isTest, bool noCheck);
using FnGetAllItems = void (*)(const void* inv, TArray<void*>* out);
using FnLocationNames = void (*)(TArray<FName>* ret, const void* self);
using FnLocationTransform = bool (*)(void* self, const FName* name, void* transformOut);
using FnParseNetId = void (*)(void* retSharedRef, const FString* s);
using FnSetBannedUsers = void (*)(void* self, const void* arr);
using FnVoidSelf = void (*)(void* self);
using FnCanSave = bool (*)(const void* gm, bool a, bool* out);
using FnProcessEvent = void (*)(void* obj, void* func, void* params);
using FnEngineExec = bool (*)(void* engine, void* world, const char16_t* cmd, void* device);
using FnGetChatComponent = void* (*)(const void* pc);
using FnConstructSender = void (*)(const void* pc, void* senderDataOut);
using FnMalloc = void* (*)(size_t, uint32_t);

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
std::map<PropKey, int32_t> g_propCache;

// Offset of a UPROPERTY on `cls` (walking up the class chain). -1 when absent.
int32_t OffOf(void* cls, const char* name) {
    if (!cls) return -1;
    PropKey k{cls, name};
    auto it = g_propCache.find(k);
    if (it != g_propCache.end()) return it->second;
    int32_t off = -1;
    for (void* c = cls; c; c = Reflect::SuperStruct(c)) {
        void* p = Reflect::FindProperty(c, name);
        if (p) {
            int32_t o = Reflect::PropertyOffset(p);
            if (o >= 0 && o < 0x100000) off = o;
            break;
        }
    }
    g_propCache[k] = off;
    return off;
}
int32_t Off(void* obj, const char* name) { return OffOf(Reflect::ObjClass(obj), name); }

std::string ReadFStringAt(void* base, int32_t off) {
    if (!base || off < 0) return "";
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 16)) return "";
    FString s;
    memcpy(&s, p, sizeof s);
    if (s.Num <= 0 || s.Num > 1 << 16) return "";
    return Reflect::Utf16To8(s.Data, s.Num);
}

// A stack FString pointing at caller-owned UTF-16. Only safe for `const FString&` parameters that
// the callee copies (FText::FromString, ParseFromString, ...).
struct TempFString {
    std::vector<char16_t> buf;
    FString fs;
    explicit TempFString(const std::string& s) {
        buf = Reflect::Utf8To16(s);
        fs.Data = buf.data();
        fs.Num = (int32_t)buf.size();  // includes the NUL, as UE's FString does
        fs.Max = fs.Num;
    }
};

// An FString allocated with the game's allocator, so the game may own and free it.
bool MakeGameFString(const std::string& s, FString& out) {
    auto malloc_ = Fn<FnMalloc>("FMemory::Malloc");
    if (!malloc_) return false;
    auto w = Reflect::Utf8To16(s);
    void* mem = malloc_(w.size() * 2, 8);
    if (!mem) return false;
    memcpy(mem, w.data(), w.size() * 2);
    out.Data = (char16_t*)mem;
    out.Num = (int32_t)w.size();
    out.Max = out.Num;
    return true;
}

std::string TextToString(void* base, int32_t off) {
    if (!base || off < 0) return "";
    auto disp = Fn<FnTextDisplayString>("FTextInspector::GetDisplayString");
    const void* p = (const char*)base + off;
    if (!disp || !MemReadable(p, 16)) return "";
    const FString* s = disp(p);
    if (!s || !MemReadable(s, 16) || s->Num <= 0 || s->Num > 1 << 16) return "";
    return Reflect::Utf16To8(s->Data, s->Num);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// A bare 32-hex EOS ProductUserId, lower-cased. Strips a `RedpointEOS:` / `EOS:` / `epic:` prefix.
std::string NormalizeGameId(const std::string& raw) {
    std::string s = raw;
    size_t colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);
    s = Lower(s);
    std::string hex;
    for (char c : s)
        if (isxdigit((unsigned char)c)) hex += c;
    return hex.size() == 32 ? hex : Lower(raw);
}

std::string ErrJson(const std::string& msg) { return "{\"error\":" + JsonStr(msg) + "}"; }
Actions::Result Fail(int status, const std::string& msg) { return {status, ErrJson(msg)}; }

// A job result carried out of the game thread. A job that has a lot to say returns `render`: a
// closure over owned copies (strings, numbers - never UObject pointers it dereferences) that the
// calling background thread turns into JSON, so the serialisation never runs on the game thread.
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

// Runs `fn` on the game thread and waits; 503 when the pump is unavailable or the job timed out.
// The job owns its function and its result: after a caller timeout a started job may still finish,
// so nothing it touches may live on the caller's stack.
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

// ---------------------------------------------------------------------------------------------
// world / players (game thread)

void* WorldClass() { return Reflect::StaticClass("UWorld::StaticClass"); }

void* FindWorld() {
    void* cls = WorldClass();
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
    void* pawn = nullptr;
    std::string gameId;
    std::string name;          // platform / account name
    std::string characterName; // in-game display name
    std::string steamId;
    std::string xboxLiveId;
    int ping = 0;
    bool spawned = false;
};

// The EOS ProductUserId behind an APlayerState::UniqueID (FUniqueNetIdRepl).
std::string NetIdString(void* netIdPtr) {
    if (!netIdPtr || !MemReadable(netIdPtr, 16)) return "";
    uint64_t wantVt = DynSymAddr("_ZTV15FUniqueNetIdEOS");
    void* vt = *(void* const*)netIdPtr;
    // The vtable pointer of the object is the _ZTV symbol address plus the two header words.
    if (wantVt && (uint64_t)(uintptr_t)vt != wantVt + 16) return "";
    auto toString = Fn<FnNetIdToString>("FUniqueNetIdEOS::ToString");
    if (!toString) return "";
    FString out{};
    toString(&out, netIdPtr);
    return Reflect::ToStd(out, true);
}

// APlayerState::UniqueID is an FUniqueNetIdRepl; the shared pointer to the FUniqueNetId sits
// somewhere in its first words, so the offset is never assumed: the struct is scanned for a pointer
// whose vtable is exactly _ZTV15FUniqueNetIdEOS (the same rule lane L2's event identity uses).
std::string PlayerStateGameId(void* ps) {
    int32_t idOff = Off(ps, "UniqueID");
    if (idOff < 0) idOff = Off(ps, "UniqueId");
    if (idOff < 0) return "";
    for (int i = 0; i < 8; i++) {
        void* cand = ReadPtrAt(ps, idOff + i * 8);
        std::string s = NetIdString(cand);
        if (s.empty()) continue;
        std::string id = NormalizeGameId(s);
        if (id.size() == 32) return id;
    }
    return "";
}


// The two native display-name getters (ADominionPlayerState::GetCharacterDisplayName and
// UDisplayNameComponent::GetCharacterDisplayName) crash the server when the display-name registry
// has no entry for the player state yet - they fault inside DoOwnerGuidsMatch. They are never
// called; the name comes from the reflected property, from the display-name component's own
// properties, or from the character name the server prints in its join line.

std::string CharacterNameOf(void* ps) {
    std::string s = ReadFStringAt(ps, Off(ps, "CharacterName"));
    if (!s.empty()) return s;
    void* dnc = ReadPtrAt(ps, Off(ps, "DisplayNameComponent"));
    if (!dnc) return "";
    for (const char* n : {"DisplayName", "CharacterName", "Name", "PlayerName"}) {
        int32_t off = Off(dnc, n);
        if (off < 0) continue;
        std::string s = ReadFStringAt(dnc, off);
        if (s.empty()) s = TextToString(dnc, off);
        if (!s.empty()) return s;
    }
    return "";
}

// Fills in one player from an APlayerState.
bool ReadPlayer(void* ps, PlayerInfo& out) {
    if (!ps || !MemReadable(ps, 0x40)) return false;
    out.playerState = ps;
    out.gameId = PlayerStateGameId(ps);
    out.name = ReadFStringAt(ps, Off(ps, "PlayerNamePrivate"));
    out.characterName = CharacterNameOf(ps);
    if (out.characterName.empty()) out.characterName = state::CharacterName(out.gameId);
    else if (!out.gameId.empty()) state::NoteCharacterName(out.gameId, out.characterName);
    Eos::Linked linked;
    Eos::LinkedAccounts(out.gameId, linked);
    out.steamId = linked.steamId;
    out.xboxLiveId = linked.xboxLiveId;
    out.pawn = ReadPtrAt(ps, Off(ps, "PawnPrivate"));
    out.spawned = out.pawn != nullptr;
    out.controller = ReadPtrAt(ps, Off(ps, "Owner"));
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

// connectedAt, kept across snapshots so /players can report it.
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

std::string PlayerJson(const PlayerInfo& p) {
    std::string name = !p.characterName.empty() ? p.characterName : p.name;
    std::string o = "{\"gameId\":" + JsonStr(p.gameId) + ",\"name\":" + JsonStr(name) +
                    ",\"characterName\":" + JsonStr(p.characterName) + ",\"platformName\":" + JsonStr(p.name) +
                    ",\"epicOnlineServicesId\":" + JsonStr(p.gameId) + ",\"platformId\":" + JsonStr("epic:" + p.gameId);
    if (!p.steamId.empty()) o += ",\"steamId\":" + JsonStr(p.steamId);
    if (!p.xboxLiveId.empty()) o += ",\"xboxLiveId\":" + JsonStr(p.xboxLiveId);
    o += ",\"ping\":" + std::to_string(p.ping) + ",\"spawned\":" + (p.spawned ? "true" : "false") +
         ",\"online\":true,\"connectedAt\":" + JsonStr(FirstSeen(p.gameId)) + "}";
    return o;
}

bool FindPlayerById(const std::string& id, PlayerInfo& out) {
    std::string needle = NormalizeGameId(id);
    for (auto& p : ReadPlayers()) {
        if (Lower(p.gameId) == needle || Lower(p.characterName) == Lower(id) || Lower(p.name) == Lower(id)) {
            out = p;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// positions

bool PawnLocation(void* pawn, double loc[3], double rot[3]) {
    void* root = ReadPtrAt(pawn, Off(pawn, "RootComponent"));
    if (!root) return false;
    int32_t lo = Off(root, "RelativeLocation");
    int32_t ro = Off(root, "RelativeRotation");
    if (lo < 0 || !MemReadable((const char*)root + lo, 24)) return false;
    memcpy(loc, (const char*)root + lo, 24);
    if (ro >= 0 && MemReadable((const char*)root + ro, 24)) memcpy(rot, (const char*)root + ro, 24);
    return true;
}

// ---------------------------------------------------------------------------------------------
// item catalogue

struct ItemDef {
    std::string code, name, description, category;
    void* asset = nullptr;
};
Mutex g_itemLock;
std::vector<ItemDef> g_items;
bool g_itemsBuilt = false;

void* ItemDataClass() {
    void* c = Reflect::StaticClass("UItemData::StaticClass");
    if (!c) c = Reflect::FindObjectByPath("/Script/Dominion", "ItemData");
    return c;
}

std::string GameplayTagName(void* base, int32_t off) {
    // FGameplayTag is a single FName.
    if (off < 0 || !base || !MemReadable((const char*)base + off, 8)) return "";
    FName n;
    memcpy(&n, (const char*)base + off, 8);
    if (!n.Comparison) return "";
    return Reflect::NameToString(n);
}

// Game thread. Builds (once) the ItemData catalogue.
bool BuildItems() {
    void* cls = ItemDataClass();
    if (!cls) return false;
    std::vector<void*> objs;
    if (!Reflect::GetObjectsOfClass(cls, objs, true)) return false;
    std::vector<ItemDef> built;
    built.reserve(objs.size());
    for (void* o : objs) {
        ItemDef d;
        d.asset = o;
        d.code = Reflect::ObjName(o);
        if (d.code.empty() || d.code.rfind("Default__", 0) == 0) continue;
        d.name = TextToString(o, Off(o, "Name"));
        if (d.name.empty()) d.name = d.code;
        d.description = TextToString(o, Off(o, "FlavourText"));
        d.category = GameplayTagName(o, Off(o, "Category"));
        built.push_back(std::move(d));
    }
    if (built.empty()) return false;
    Guard g(g_itemLock);
    g_items = std::move(built);
    g_itemsBuilt = true;
    return true;
}

// Exact code (ci), then a unique case-insensitive substring.
const ItemDef* LookupItem(const std::string& code, std::string& err) {
    Guard g(g_itemLock);
    if (!g_itemsBuilt) {
        err = "item catalogue not loaded yet";
        return nullptr;
    }
    std::string needle = Lower(code);
    for (auto& d : g_items)
        if (Lower(d.code) == needle) return &d;
    for (auto& d : g_items)
        if (Lower(d.name) == needle) return &d;
    const ItemDef* hit = nullptr;
    size_t n = 0;
    for (auto& d : g_items) {
        if (Lower(d.code).find(needle) != std::string::npos || Lower(d.name).find(needle) != std::string::npos) {
            hit = &d;
            n++;
        }
    }
    if (n == 1) return hit;
    err = n ? ("item code '" + code + "' is ambiguous (" + std::to_string(n) + " matches)")
            : ("unknown item code '" + code + "'");
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// inventory

void* InventoryComponentClass() { return Reflect::FindObjectByPath("/Script/Dominion", "InventoryComponent"); }

struct InvItem {
    std::string code, name, inventory;
    int amount = 1;
    int slot = -1;
};

// Game thread. Reads every UInventoryComponent owned by the pawn or the controller.
std::vector<InvItem> ReadInventory(const PlayerInfo& p, std::string& detail) {
    std::vector<InvItem> out;
    void* invCls = InventoryComponentClass();
    auto getAll = Fn<FnGetAllItems>("UInventoryComponent::GetAllItems");
    if (!invCls || !getAll) {
        detail = "UInventoryComponent or GetAllItems unavailable";
        return out;
    }
    std::vector<void*> comps, tmp;
    for (void* owner : {p.pawn, p.controller}) {
        if (!owner) continue;
        if (Reflect::GetObjectsWithOuter(owner, tmp, true))
            for (void* c : tmp)
                if (Reflect::IsA(c, invCls)) comps.push_back(c);
    }
    if (comps.empty()) {
        detail = "no UInventoryComponent found under the pawn or controller";
        return out;
    }
    for (void* c : comps) {
        TArray<void*> items{};
        getAll(c, &items);
        if (!items.Data || items.Num <= 0 || items.Num > 4096 || !MemReadable(items.Data, (size_t)items.Num * 8)) {
            if (items.Data) {
                // GetAllItems allocates through the game allocator; give it back.
                auto freeFn = Fn<void (*)(void*)>("FMemory::Free");
                if (freeFn) freeFn(items.Data);
            }
            continue;
        }
        std::string invName = Reflect::ObjName(c);
        for (int32_t i = 0; i < items.Num; i++) {
            void* item = items.Data[i];
            if (!item || !MemReadable(item, 0x40)) continue;
            InvItem e;
            e.inventory = invName;
            e.slot = i;
            int32_t countOff = Off(item, "Count");
            if (countOff >= 0 && MemReadable((const char*)item + countOff, 4))
                e.amount = *(const int32_t*)((const char*)item + countOff);
            void* data = ReadPtrAt(item, Off(item, "ItemData"));
            if (data) {
                e.code = Reflect::ObjName(data);
                e.name = TextToString(data, Off(data, "Name"));
            }
            if (e.code.empty()) e.code = Reflect::ObjName(item);
            if (e.name.empty()) e.name = e.code;
            if (e.amount <= 0) e.amount = 1;
            out.push_back(std::move(e));
        }
        auto freeFn = Fn<void (*)(void*)>("FMemory::Free");
        if (freeFn) freeFn(items.Data);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// settings / bans

void* SettingsObject() {
    void* cls = Reflect::FindObjectByPath("/Script/Dominion", "DedicatedServerSettings");
    if (!cls) return nullptr;
    std::vector<void*> objs;
    Reflect::GetObjectsOfClass(cls, objs, true);
    for (void* o : objs)
        if (o) return o;
    return Reflect::ClassDefaultObject(cls);  // the settings live on the CDO when no instance exists
}

// The server's own moderation state lives in UDedicatedServerSettings::KnownPlayerList, an array of
// FServerKnownPlayer {UserId: FUniqueNetIdRepl, UserName: FString, Privileges, bIsBanned, ...}. That
// flag is what the ini persists (`KnownPlayerList=(...,bIsBanned=True)`) and what the game checks on
// login, so ban/unban set it and then call PerformConfigSave. Every offset below comes from
// reflection of the FServerKnownPlayer script struct at call time.
struct KnownPlayerLayout {
    void* settings = nullptr;
    char* base = nullptr;
    int32_t num = 0;
    size_t stride = 0;
    int32_t userId = -1, userName = -1, isBanned = -1;
    bool ok() const { return settings && base && stride && userId >= 0 && isBanned >= 0; }
};

KnownPlayerLayout ReadKnownPlayers() {
    KnownPlayerLayout k;
    k.settings = SettingsObject();
    if (!k.settings) return k;
    int32_t off = Off(k.settings, "KnownPlayerList");
    if (off < 0 || !MemReadable((const char*)k.settings + off, 16)) return k;
    TArray<char> arr{};
    memcpy(&arr, (const char*)k.settings + off, sizeof arr);
    void* st = Reflect::FindObjectByPath("/Script/Dominion", "ServerKnownPlayer");
    if (!st) return k;
    void* pSize = (char*)st + 0;  // placeholder; the real size comes from PropertiesSize below
    (void)pSize;
    void* pId = Reflect::FindProperty(st, "UserId");
    void* pName = Reflect::FindProperty(st, "UserName");
    void* pBan = Reflect::FindProperty(st, "bIsBanned");
    if (!pId || !pBan) return k;
    k.userId = Reflect::PropertyOffset(pId);
    k.userName = pName ? Reflect::PropertyOffset(pName) : -1;
    k.isBanned = Reflect::PropertyOffset(pBan);
    // FServerKnownPlayer's size is the struct's PropertiesSize, aligned to 8.
    uint32_t size = 0;
    if (MemReadable((const char*)st + Reflect::Lay().structPropertiesSize, 4))
        size = *(const uint32_t*)((const char*)st + Reflect::Lay().structPropertiesSize);
    if (!size || size > 1024) return k;
    k.stride = (size + 7) & ~7u;
    if (arr.Num < 0 || arr.Num > 4096) return k;
    if (arr.Num && !MemReadable(arr.Data, (size_t)arr.Num * k.stride)) return k;
    k.base = arr.Data;
    k.num = arr.Num;
    return k;
}

// The EOS id of one FServerKnownPlayer entry (the FUniqueNetIdRepl is scanned, never assumed).
std::string EntryGameId(const KnownPlayerLayout& k, int32_t i) {
    char* e = k.base + (size_t)i * k.stride;
    for (int w = 0; w < 8; w++) {
        void* cand = ReadPtrAt(e, k.userId + w * 8);
        std::string s = NetIdString(cand);
        if (!s.empty()) {
            std::string id = NormalizeGameId(s);
            if (id.size() == 32) return id;
        }
    }
    return "";
}

struct BanEntry {
    std::string gameId, name;
};

std::vector<BanEntry> ReadBans() {
    std::vector<BanEntry> out;
    KnownPlayerLayout k = ReadKnownPlayers();
    if (!k.ok()) return out;
    for (int32_t i = 0; i < k.num; i++) {
        char* e = k.base + (size_t)i * k.stride;
        if (!MemReadable(e + k.isBanned, 1) || !*(const uint8_t*)(e + k.isBanned)) continue;
        BanEntry b;
        b.gameId = EntryGameId(k, i);
        b.name = k.userName >= 0 ? ReadFStringAt(e, k.userName) : "";
        out.push_back(std::move(b));
    }
    return out;
}

// The live login check reads ADominionGameStateBase::BannedUsers (a replicated
// TArray<FKickOrBanUser>) that is built once at boot from the settings, so ban/unban must edit that
// array too or the change only takes effect after a restart. FKickOrBanUser is
// {UserId: FUniqueNetIdRepl @0, UserDisplayName: FString @48, UserStateFlags @64}.
struct BannedArray {
    void* gameState = nullptr;
    int32_t propOff = -1;
    char* data = nullptr;
    int32_t num = 0, max = 0;
    size_t stride = 0;
    int32_t userId = -1, displayName = -1;
    bool ok() const { return gameState && propOff >= 0 && stride && userId >= 0; }
};

BannedArray ReadGameStateBans() {
    BannedArray b;
    b.gameState = GameStateOf(FindWorld());
    if (!b.gameState) return b;
    b.propOff = Off(b.gameState, "BannedUsers");
    if (b.propOff < 0 || !MemReadable((const char*)b.gameState + b.propOff, 16)) return b;
    void* st = Reflect::FindObjectByPath("/Script/Dominion", "KickOrBanUser");
    if (!st) return b;
    void* pId = Reflect::FindProperty(st, "UserId");
    void* pName = Reflect::FindProperty(st, "UserDisplayName");
    if (!pId) return b;
    b.userId = Reflect::PropertyOffset(pId);
    b.displayName = pName ? Reflect::PropertyOffset(pName) : -1;
    uint32_t size = 0;
    if (MemReadable((const char*)st + Reflect::Lay().structPropertiesSize, 4))
        size = *(const uint32_t*)((const char*)st + Reflect::Lay().structPropertiesSize);
    if (!size || size > 1024) return b;
    b.stride = (size + 7) & ~7u;
    TArray<char> arr{};
    memcpy(&arr, (const char*)b.gameState + b.propOff, sizeof arr);
    if (arr.Num < 0 || arr.Num > 4096) return b;
    if (arr.Num && !MemReadable(arr.Data, (size_t)arr.Num * b.stride)) return b;
    b.data = arr.Data;
    b.num = arr.Num;
    b.max = arr.Max;
    return b;
}

std::string GameStateEntryId(const BannedArray& b, int32_t i) {
    char* e = b.data + (size_t)i * b.stride;
    for (int w = 0; w < 8; w++) {
        std::string s = NetIdString(ReadPtrAt(e, b.userId + w * 8));
        if (!s.empty()) {
            std::string id = NormalizeGameId(s);
            if (id.size() == 32) return id;
        }
    }
    return "";
}

// Removes `gameId` from the live GameState ban list. Element bytes are shifted down and Num is
// decremented; the removed element's FString/shared pointer is intentionally leaked rather than
// destructed by us.
bool GameStateUnban(const std::string& gameId) {
    BannedArray b = ReadGameStateBans();
    if (!b.ok() || !b.num) return false;
    bool removed = false;
    for (int32_t i = b.num - 1; i >= 0; i--) {
        if (GameStateEntryId(b, i) != gameId) continue;
        char* e = b.data + (size_t)i * b.stride;
        if (!MemWritable(e, b.stride)) continue;
        if (i + 1 < b.num) memmove(e, e + b.stride, (size_t)(b.num - 1 - i) * b.stride);
        b.num--;
        removed = true;
    }
    if (!removed) return false;
    char* numField = (char*)b.gameState + b.propOff + 8;  // TArray {Data, Num, Max}
    if (!MemWritable(numField, 4)) return false;
    *(int32_t*)numField = b.num;
    return true;
}

// Appends `gameId` to the live GameState ban list, reusing the FUniqueNetIdRepl bytes of the
// player's KnownPlayerList entry. The shared-reference count of the copied net id is incremented by
// hand so that the game's own destruction of the array cannot free an id we do not own.
bool GameStateBan(const std::string& gameId, const KnownPlayerLayout& k, int32_t knownIndex) {
    BannedArray b = ReadGameStateBans();
    auto malloc_ = Fn<FnMalloc>("FMemory::Malloc");
    if (!b.ok() || !malloc_ || knownIndex < 0) return false;
    for (int32_t i = 0; i < b.num; i++)
        if (GameStateEntryId(b, i) == gameId) return true;  // already there
    size_t count = (size_t)b.num + 1;
    char* mem = (char*)malloc_(count * b.stride, 8);
    if (!mem) return false;
    memset(mem, 0, count * b.stride);
    if (b.num) memcpy(mem, b.data, (size_t)b.num * b.stride);
    char* e = mem + (size_t)b.num * b.stride;
    char* src = k.base + (size_t)knownIndex * k.stride + k.userId;
    size_t idSize = 48;  // FUniqueNetIdRepl
    if (!MemReadable(src, idSize)) return false;
    memcpy(e + b.userId, src, idSize);
    // TSharedPtr {Object, FReferenceControllerBase*}; the controller counts live at +8 / +12.
    void* controller = *(void**)(e + b.userId + 8);
    if (controller && MemWritable((char*)controller + 8, 8)) {
        __atomic_add_fetch((int32_t*)((char*)controller + 8), 1, __ATOMIC_SEQ_CST);
        __atomic_add_fetch((int32_t*)((char*)controller + 12), 1, __ATOMIC_SEQ_CST);
    }
    char* arrField = (char*)b.gameState + b.propOff;
    if (!MemWritable(arrField, 16)) return false;
    *(void**)arrField = mem;
    *(int32_t*)(arrField + 8) = (int32_t)count;
    *(int32_t*)(arrField + 12) = (int32_t)count;
    return true;
}

// The game's login check does not read KnownPlayerList: ADominionGameMode::PreLogin asks
// UDomMatchmakerSubsystem::IsOnlineUserBanned, which forwards to the FNetworkMatcherSession it owns.
// That session keeps a TMap<FUniqueNetIdWrapper, TSharedRef<FOnlineUser const>> of banned users,
// loaded from the settings at start-up. A player banned when the server started therefore stays
// refused ("PreLogin failure: PLogBanned") after any unban - Takaro's or the timed-ban expiry - until
// the next restart, unless the session's own RequestRemoveBanPlayer is called with the FOnlineUser
// the map holds. Nothing here is assumed: the session pointer's offset is read out of the
// subsystem's forwarding stub, the map's offset out of RequestRemoveBanPlayer, and a map element is
// only used when its key resolves to exactly the player being unbanned.
using FnRequestRemoveBan = void (*)(void* session, void* userRef /* TSharedRef<FOnlineUser const>* */);

// UDomMatchmakerSubsystem::IsOnlineUserBanned is `mov rdi,[rdi+disp32]; jmp FNetworkMatcherSession::IsOnlineUserBanned`.
int32_t MatcherSessionOffset(std::string& why) {
    uint64_t f = Sym::Addr("UDomMatchmakerSubsystem::IsOnlineUserBanned");
    uint64_t want = Sym::Addr("FNetworkMatcherSession::IsOnlineUserBanned");
    if (!f || !want) { why = "matchmaker ban-check symbols unresolved"; return -1; }
    const uint8_t* b = (const uint8_t*)(uintptr_t)f;
    if (!MemReadable(b, 12)) { why = "matchmaker ban-check stub not readable"; return -1; }
    if (b[0] != 0x48 || b[1] != 0x8b || b[2] != 0xbf || b[7] != 0xe9) { why = "unexpected matchmaker ban-check stub"; return -1; }
    int32_t disp = 0, rel = 0;
    memcpy(&disp, b + 3, 4);
    memcpy(&rel, b + 8, 4);
    if (f + 12 + (int64_t)rel != want) { why = "matchmaker stub does not forward to the session ban check"; return -1; }
    if (disp <= 0 || disp > 0x10000 || disp % 8) { why = "implausible matcher-session offset"; return -1; }
    return disp;
}

// RequestRemoveBanPlayer addresses the banned-user map as `lea r12,[rbx+disp32]` (4C 8D A3 disp32).
int32_t BannedMapOffset(std::string& why) {
    uint64_t f = Sym::Addr("FNetworkMatcherSession::RequestRemoveBanPlayer");
    if (!f) { why = "FNetworkMatcherSession::RequestRemoveBanPlayer unresolved"; return -1; }
    const uint8_t* b = (const uint8_t*)(uintptr_t)f;
    if (!MemReadable(b, 512)) { why = "RequestRemoveBanPlayer not readable"; return -1; }
    for (int i = 0; i + 7 <= 512; i++) {
        if (b[i] != 0x4c || b[i + 1] != 0x8d || b[i + 2] != 0xa3) continue;
        int32_t disp = 0;
        memcpy(&disp, b + i + 3, 4);
        if (disp > 0 && disp < 0x10000 && disp % 8 == 0) return disp;
    }
    why = "banned-user map offset not found in RequestRemoveBanPlayer";
    return -1;
}

void* MatcherSession(std::string& why) {
    int32_t off = MatcherSessionOffset(why);
    if (off < 0) return nullptr;
    void* cls = Reflect::StaticClass("UDomMatchmakerSubsystem::StaticClass");
    if (!cls) { why = "UDomMatchmakerSubsystem class not found"; return nullptr; }
    std::vector<void*> objs;
    if (!Reflect::GetObjectsOfClass(cls, objs, true)) { why = "no UDomMatchmakerSubsystem objects"; return nullptr; }
    for (void* o : objs) {
        if (!o || !MemReadable((char*)o + off, 8)) continue;
        if (Reflect::ObjName(o).rfind("Default__", 0) == 0) continue;
        void* session = *(void**)((char*)o + off);
        if (session && MemReadable(session, 0x400)) return session;
    }
    why = "the matchmaker subsystem has no matcher session";
    return nullptr;
}

// Removes `gameId` from the session's banned-user map through the game's own RequestRemoveBanPlayer
// (which also rebuilds the login-check set and saves the ban config). Returns true when the player
// was in the map and is gone afterwards; `detail` says what happened otherwise.
bool SessionUnban(const std::string& gameId, std::string& detail) {
    std::string why;
    void* session = MatcherSession(why);
    int32_t mapOff = session ? BannedMapOffset(why) : -1;
    auto removeFn = Fn<FnRequestRemoveBan>("FNetworkMatcherSession::RequestRemoveBanPlayer");
    if (!session || mapOff < 0 || !removeFn) { detail = why.empty() ? "session unban unavailable" : why; return false; }
    const size_t kStride = 0x38;  // TSetElement<TTuple<FUniqueNetIdWrapper(0x20), TSharedRef(0x10)>> + hash links
    auto find = [&](void*& obj, void*& ctrl) -> bool {
        // TSparseArray {TArray Data/Num/Max @0, TBitArray AllocationFlags {inline words @0x10,
        // secondary data @0x20, NumBits @0x28}}: a removed element keeps its bytes, so only slots
        // whose allocation bit is set are ever looked at.
        char* sa = (char*)session + mapOff;
        char* data = *(char**)sa;
        int32_t num = *(int32_t*)(sa + 8);
        if (!data || num <= 0 || num > 4096 || !MemReadable(data, (size_t)num * kStride)) return false;
        const uint32_t* bits = *(const uint32_t* const*)(sa + 0x20);
        if (!bits) bits = (const uint32_t*)(sa + 0x10);
        int32_t numBits = *(int32_t*)(sa + 0x28);
        if (numBits < num || !MemReadable(bits, (size_t)((num + 31) / 32) * 4)) return false;
        for (int32_t i = 0; i < num; i++) {
            if (!(bits[i / 32] & (1u << (i % 32)))) continue;
            char* e = data + (size_t)i * kStride;
            std::string id;
            for (int w = 0; w < 4 && id.empty(); w++) {
                std::string s = NetIdString(ReadPtrAt(e, w * 8));
                if (!s.empty()) id = NormalizeGameId(s);
            }
            if (id != gameId) continue;
            obj = *(void**)(e + 0x20);
            ctrl = *(void**)(e + 0x28);
            if (obj && ctrl && MemReadable(obj, 0x40) && MemWritable((char*)ctrl + 8, 8)) return true;
        }
        return false;
    };
    void* obj = nullptr;
    void* ctrl = nullptr;
    if (!find(obj, ctrl)) { detail = "not in the login ban set"; return false; }
    // The argument is a TSharedRef passed by value, i.e. by pointer to a caller-owned copy. Take one
    // shared reference for that copy and never release it: one FOnlineUser outlives the unban.
    __atomic_add_fetch((int32_t*)((char*)ctrl + 8), 1, __ATOMIC_SEQ_CST);
    void* ref[2] = {obj, ctrl};
    removeFn(session, ref);
    void* o2 = nullptr;
    void* c2 = nullptr;
    if (find(o2, c2)) { detail = "RequestRemoveBanPlayer left the player in the login ban set"; return false; }
    detail = "removed from the login ban set";
    return true;
}

// Sets or clears bIsBanned for one known player and persists the settings to DedicatedServer.ini.
bool WriteBans(const std::string& gameId, const std::string& /*userName*/, bool add, std::string& err) {
    KnownPlayerLayout k = ReadKnownPlayers();
    auto saveFn = Fn<FnVoidSelf>("UDedicatedServerSettings::PerformConfigSave");
    if (!k.ok()) {
        err = "UDedicatedServerSettings::KnownPlayerList is not reflectable";
        return false;
    }
    if (!saveFn) {
        err = "PerformConfigSave unresolved";
        return false;
    }
    std::string needle = NormalizeGameId(gameId);
    for (int32_t i = 0; i < k.num; i++) {
        if (EntryGameId(k, i) != needle) continue;
        char* flag = k.base + (size_t)i * k.stride + k.isBanned;
        if (!MemWritable(flag, 1)) {
            err = "the known-player entry is not writable";
            return false;
        }
        bool already = *(uint8_t*)flag != 0;
        *(uint8_t*)flag = add ? 1 : 0;
        saveFn(k.settings);
        // Make it effective without a restart as well.
        bool live = add ? GameStateBan(needle, k, i) : GameStateUnban(needle);
        std::string sessionDetail;
        if (!add) {
            bool removed = SessionUnban(needle, sessionDetail);
            PluginLog("actions: unban %s: %s", needle.c_str(), sessionDetail.c_str());
            (void)removed;
        }
        err = std::string(already == add ? (add ? "already banned; " : "not banned; ") : "") +
              (live ? "live ban list updated" : "live ban list unchanged (restart to apply)") +
              (sessionDetail.empty() ? "" : "; " + sessionDetail);
        return true;
    }
    err = "the server has never seen that player (no KnownPlayerList entry), so it cannot be banned offline";
    return false;
}

// ---------------------------------------------------------------------------------------------
// chat

void* ChatComponentOf(void* pc) {
    auto get = Fn<FnGetChatComponent>("ADominionPlayerController::GetPlayerChatComponent");
    if (get && pc) {
        void* c = get(pc);
        if (c && MemReadable(c, 0x40)) return c;
    }
    void* cls = Reflect::StaticClass("UPlayerChatComponent::StaticClass");
    if (!cls || !pc) return nullptr;
    std::vector<void*> comps;
    if (!Reflect::GetObjectsWithOuter(pc, comps, true)) return nullptr;
    for (void* c : comps)
        if (Reflect::IsA(c, cls)) return c;
    return nullptr;
}

// FChatMessageData is 136 bytes: SenderData (FChatPlayerSenderData) @0, MessageBody (FString) @120.
// The offsets are read from reflection at call time, never assumed.
bool SendChatTo(const PlayerInfo& p, const std::string& text, const std::string& senderName, std::string& err) {
    void* comp = ChatComponentOf(p.controller);
    if (!comp) {
        err = "no UPlayerChatComponent for that player";
        return false;
    }
    void* fn = Reflect::FindFunction(comp, "Client_ReceiveChatMessage");
    auto processEvent = Fn<FnProcessEvent>("UObject::ProcessEvent");
    void* msgStruct = Reflect::FindObjectByPath("/Script/JagexChatBackend", "ChatMessageData");
    if (!fn || !processEvent || !msgStruct) {
        err = "Client_ReceiveChatMessage / ProcessEvent / FChatMessageData unavailable";
        return false;
    }
    void* senderProp = Reflect::FindProperty(msgStruct, "SenderData");
    void* bodyProp = Reflect::FindProperty(msgStruct, "MessageBody");
    if (!senderProp || !bodyProp) {
        err = "FChatMessageData layout not reflectable";
        return false;
    }
    int32_t senderOff = Reflect::PropertyOffset(senderProp);
    int32_t bodyOff = Reflect::PropertyOffset(bodyProp);
    if (senderOff < 0 || bodyOff < 0 || bodyOff > 512) {
        err = "FChatMessageData offsets implausible";
        return false;
    }

    std::vector<char> params(256, 0);
    // Sender: the receiver's own controller fills a valid FChatPlayerSenderData (net ids + colour);
    // the sender's identity is carried in the message text, because the client renders the name
    // from the sender's net id.
    auto construct = Fn<FnConstructSender>("ADominionPlayerController::ConstructPlayerChatSenderData");
    bool haveSender = false;
    if (construct && p.controller) {
        construct(p.controller, params.data() + senderOff);
        haveSender = true;
    }
    std::string body = senderName.empty() ? text : ("[" + senderName + "] " + text);
    FString bodyStr{};
    if (!MakeGameFString(body, bodyStr)) {
        err = "could not allocate the message body";
        return false;
    }
    memcpy(params.data() + bodyOff, &bodyStr, sizeof bodyStr);
    state::MarkInjectedMessage(body);
    state::MarkInjectedMessage(text);
    processEvent(comp, fn, params.data());
    if (!haveSender) PluginLog("actions: chat sent without sender data (ConstructPlayerChatSenderData missing)");
    return true;
}

// ---------------------------------------------------------------------------------------------
// teleport targets

void* TeleportSubsystem() {
    void* cls = Reflect::StaticClass("UTeleportationSubsystem::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Dominion", "TeleportationSubsystem");
    if (!cls) return nullptr;
    std::vector<void*> objs;
    Reflect::GetObjectsOfClass(cls, objs, true);
    return objs.empty() ? nullptr : objs[0];
}

struct NamedLocation {
    std::string code;
    double x = 0, y = 0, z = 0;
};

// Asks the teleport subsystem for a named target's transform. `name` is looked up in the FName pool
// (never inserted), so an unknown name simply misses instead of producing a bogus FName.
bool NamedLocationTransform(const std::string& name, double out[3]) {
    void* sub = TeleportSubsystem();
    auto xform = Fn<FnLocationTransform>("UTeleportationSubsystem::GetTargetLocationTransform");
    if (!sub || !xform) return false;
    FName n = Reflect::MakeName(name);
    if (!n.Comparison) return false;
    // FTransform (double): Rotation quat @0 (32B), Translation @32, Scale3D @64.
    alignas(16) char tf[128];
    memset(tf, 0, sizeof tf);
    if (!xform(sub, &n, tf)) return false;
    memcpy(out, tf + 32, 24);
    return true;
}

// NOTE: UTeleportationSubsystem::GetTargetLocationNames() is deliberately NOT called. Its return
// convention does not match a by-value TArray<FName> on this build, and stringifying the FNames it
// appeared to produce crashed the server inside FName::ToString (2026-09-16). The location list is
// therefore built from the in-world lodestone actors, whose names come from validated UObject
// reflection, and named targets are resolved through GetTargetLocationTransform with a pool lookup.
std::vector<NamedLocation> ReadLocations() {
    std::vector<NamedLocation> out;
    void* lodeCls = Reflect::StaticClass("AWorldLodestone::StaticClass");
    if (!lodeCls) lodeCls = Reflect::FindObjectByPath("/Script/Dominion", "WorldLodestone");
    if (lodeCls) {
        std::vector<void*> actors;
        Reflect::GetObjectsOfClass(lodeCls, actors, true);
        for (void* a : actors) {
            NamedLocation l;
            l.code = Reflect::ObjName(a);
            if (l.code.empty()) continue;
            bool dup = false;
            for (auto& e : out) dup = dup || Lower(e.code) == Lower(l.code);
            if (dup) continue;
            double loc[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
            if (PawnLocation(a, loc, rot) || NamedLocationTransform(l.code, loc)) {
                l.x = loc[0];
                l.y = loc[1];
                l.z = loc[2];
            }
            out.push_back(std::move(l));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// capability bookkeeping

void SetCap(const char* name, const char* status, const std::string& detail = "") {
    // A capability this build should have but whose symbol did not resolve is not a static gap: it
    // is what a game update looks like, so it is reported as degraded (and so named in
    // testReachability's reason) rather than unimplemented.
    if (strcmp(status, "unimplemented") == 0 && detail.find("unresolved") != std::string::npos) status = "degraded";
    PluginState::Get().SetCapability(name, status, detail);
}


std::atomic<bool> g_shutdownRequested{false};

void* ShutdownThread(void*) {
    // The HTTP response is already on the wire; give it a moment, then save and leave.
    struct timespec half{0, 500 * 1000 * 1000};
    nanosleep(&half, nullptr);
    auto saveRequested = std::make_shared<bool>(false);
    bool saveCompleted = GameThread::Run(
        [saveRequested] {
            void* gm = GameModeOf(FindWorld());
            auto canSave = Fn<FnCanSave>("ADominionGameMode::CanSave");
            auto request = Fn<FnVoidSelf>("ADominionGameMode::RequestSaveGame");
            if (!gm || !request) return;
            bool why = false;
            bool ok = canSave ? canSave(gm, false, &why) : true;
            PluginLog("shutdown: CanSave=%d", (int)ok);
            if (ok) {
                request(gm);
                *saveRequested = true;
            }
        },
        5000);
    if (saveCompleted && *saveRequested) {
        // Wait for the save to land (bounded); the game logs it, but we only need the time.
        for (int i = 0; i < 20; i++) {
            struct timespec ts{0, 500 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
    }
    PluginLog("shutdown: sending SIGTERM to pid %d", getpid());
    FlushPluginLogs();
    kill(getpid(), SIGTERM);
    return nullptr;
}

}  // namespace

// ================================================================================================
// lifecycle

void Actions::Init() {
    // Status reflects what resolved and what the live proofs on 2026-09-16 showed; "ok" means wired
    // and proven on the rig, "degraded" carries the exact limitation in capabilityDetails.
    bool identity = Sym::Addr("FUniqueNetIdEOS::ToString") != 0;
    SetCap("players", identity ? "ok" : "degraded",
           identity ? "" : "FUniqueNetIdEOS::ToString unresolved; gameId would be empty");
    SetCap("playerLocation", "ok");
    SetCap("playerInventory", Sym::Addr("UInventoryComponent::GetAllItems") ? "ok" : "degraded",
           Sym::Addr("UInventoryComponent::GetAllItems") ? "" : "UInventoryComponent::GetAllItems unresolved");
    SetCap("listItems", "degraded", "item catalogue not built yet (built on the first world tick)");
    SetCap("listEntities", "degraded",
           "AI data assets are streamed in on demand: the list is the AI character classes and data assets "
           "loaded so far, not the full bestiary");
    SetCap("listLocations", "degraded",
           "UTeleportationSubsystem::GetTargetLocationNames is not callable safely on this build; the list is "
           "the lodestone actors currently streamed in");
    bool give = Sym::Addr("UDominionRuntimeBlueprintLibrary::TryGiveItemToPlayer") != 0;
    SetCap("giveItem", give ? "ok" : "unimplemented", give ? "" : "TryGiveItemToPlayer unresolved");
    bool chat = Sym::Addr("UObject::ProcessEvent") && Sym::Addr("ADominionPlayerController::GetPlayerChatComponent");
    SetCap("sendMessage", chat ? "ok" : "degraded",
           chat ? "messages render under the receiving player's own name with a [sender] prefix (the client "
                  "renders the name from the sender net id)"
                : "chat component or ProcessEvent unresolved");
    SetCap("teleport", Sym::Addr("AActor::TeleportTo") ? "ok" : "unimplemented",
           Sym::Addr("AActor::TeleportTo") ? "" : "AActor::TeleportTo unresolved");
    bool moderation = Sym::Addr("ADominionGameSession::KickPlayer") && Sym::Addr("FText::FromString");
    SetCap("kick", moderation ? "ok" : "unimplemented", moderation ? "" : "KickPlayer/FText::FromString unresolved");
    bool banList = Sym::Addr("UDedicatedServerSettings::PerformConfigSave") != 0;
    bool liveBans = Events::BanEnforcementLive();
    SetCap("ban", liveBans ? "ok" : (banList ? "degraded" : "unimplemented"),
           liveBans ? "the player is disconnected at once, flagged in the game's own KnownPlayerList "
                      "(persisted to DedicatedServer.ini) and added to the plugin ban list, which the "
                      "PreLogin hook refuses a rejoin with immediately - no restart needed"
                    : (banList ? "enforced, but one step later than it could be: the PreLogin hook is not bound "
                                 "on this build, so a banned player's rejoin is refused by the plugin's PostLogin "
                                 "ban kick (~1.5 s in the world) instead of being turned away at login. The ban "
                                 "itself is immediate and is persisted to both DedicatedServer.ini and takaro/bans.json"
                               : "UDedicatedServerSettings::PerformConfigSave unresolved"));
    SetCap("unban", banList ? "ok" : "unimplemented", banList ? "" : "PerformConfigSave unresolved");
    SetCap("listBans", "ok",
           "the union of the game's KnownPlayerList entries with bIsBanned=True and the plugin ban list "
           "(reason and createdAt come from the plugin list; the game stores neither; the connector "
           "lifts timed bans at expiresAt itself)");
    SetCap("executeCommand", "ok",
           "plugin command set (players, say, whisper, give, tp, kick, ban, unban, bans, items, entities, "
           "locations, save, shutdown, help) plus `raw <cmd>` through UEngine::Exec with captured output; "
           "`cheat <id> <cmd>` is the one unimplemented form - the Shipping dedicated server creates no "
           "CheatManager");
    SetCap("shutdown", Sym::Addr("ADominionGameMode::RequestSaveGame") ? "ok" : "degraded",
           Sym::Addr("ADominionGameMode::RequestSaveGame") ? "" : "RequestSaveGame unresolved; SIGTERM without saving");
}

void Actions::Housekeep() {
    // Also flush mutations that finished after a caller timed out.
    state::FlushBans();
    FlushPluginLogs();
    // Build the item catalogue once the world is up; it never changes afterwards.
    {
        Guard g(g_itemLock);
        if (g_itemsBuilt) return;
    }
    if (GameThread::TickCount() == 0) return;
    auto built = std::make_shared<bool>(false);
    bool completed = GameThread::Run([built] { Perf::Scope sc("catalogue.items"); *built = BuildItems(); }, 5000);
    if (completed && *built) {
        Guard g(g_itemLock);
        SetCap("listItems", "ok", "");
        PluginLog("actions: item catalogue built (%zu items)", g_items.size());
    }
}

// ================================================================================================
// handlers

Actions::Result Actions::Players() {
    return OnGameThread("GET /players", []() -> JobOut {
        auto players = ReadPlayers();
        for (auto& p : players) p.playerState = p.controller = p.pawn = nullptr;  // render needs no pointers
        return {200, [players = std::move(players)] {
            std::string o = "[";
            bool first = true;
            for (auto& p : players) {
                if (!first) o += ",";
                first = false;
                o += PlayerJson(p);
            }
            return o + "]";
        }};
    });
}

Actions::Result Actions::Player(const std::string& gameId) {
    return OnGameThread("GET /players/{id}", [gameId]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(gameId, p)) return JobOut::Error(404, "player not online");
        p.playerState = p.controller = p.pawn = nullptr;
        return {200, [p] { return PlayerJson(p); }};
    });
}

Actions::Result Actions::PlayerLocation(const std::string& gameId) {
    return OnGameThread("GET /players/{id}/location", [gameId]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(gameId, p)) return {404, ErrJson("player not online")};
        if (!p.pawn) return {503, ErrJson("player has no pawn yet")};
        double loc[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
        if (!PawnLocation(p.pawn, loc, rot)) return {503, ErrJson("pawn has no readable root component")};
        return {200, "{\"x\":" + JsonNum(loc[0]) + ",\"y\":" + JsonNum(loc[1]) + ",\"z\":" + JsonNum(loc[2]) +
                         ",\"yaw\":" + JsonNum(rot[1]) + ",\"pitch\":" + JsonNum(rot[0]) + "}"};
    });
}

Actions::Result Actions::PlayerInventory(const std::string& gameId) {
    return OnGameThread("GET /players/{id}/inventory", [gameId]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(gameId, p)) return {404, ErrJson("player not online")};
        std::string detail;
        auto items = ReadInventory(p, detail);
        if (items.empty() && !detail.empty()) {
            SetCap("playerInventory", "degraded", detail);
            return {503, ErrJson(detail)};
        }
        return {200, [items = std::move(items)] {
            std::string o = "[";
            for (size_t i = 0; i < items.size(); i++) {
                if (i) o += ",";
                o += "{\"code\":" + JsonStr(items[i].code) + ",\"name\":" + JsonStr(items[i].name) +
                     ",\"amount\":" + std::to_string(items[i].amount) + ",\"inventory\":" +
                     JsonStr(items[i].inventory) + ",\"slot\":" + std::to_string(items[i].slot) + "}";
            }
            return o + "]";
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
        if (!needle.empty() && Lower(d.code).find(needle) == std::string::npos &&
            Lower(d.name).find(needle) == std::string::npos)
            continue;
        if (!first) o += ",";
        first = false;
        o += "{\"code\":" + JsonStr(d.code) + ",\"name\":" + JsonStr(d.name) +
             ",\"description\":" + JsonStr(d.description) + ",\"category\":" + JsonStr(d.category) + "}";
    }
    return {200, o + "]"};
}

namespace {

// ---------------------------------------------------------------------------------------------
// The cooked asset registry: the full list of AI data assets, loaded or not.
//
// Dragonwilds streams its AI content, so GetObjectsOfClass only ever sees what is in memory. The
// registry that ships with the build knows every cooked asset, and
// UAssetRegistryImpl::GetAssetsByClass(FTopLevelAssetPath, TArray<FAssetData>&, bool) hands them
// over. FAssetData's layout is *not* assumed: the result array is scanned for the {stride, offset}
// pair whose AssetClassPath FName pair equals - word for word, with no stringification - the class
// path we asked for, in every element. Only then is the AssetName FName turned into a string.
// The call is made once per boot and the result cached; the copied FAssetData tag views are handed
// back to the allocator without destructing them (they are shared with the registry's own copies).
using FnGetAssetsByClass = bool (*)(const void* self, UE::FTopLevelAssetPath path, TArray<char>* out, bool subclasses);

struct RegistryAsset {
    std::string name;
    std::string package;
};

bool RegistryAssetsByClass(const char* pkg, const char* cls, std::vector<RegistryAsset>& out, std::string& why) {
    auto fn = Fn<FnGetAssetsByClass>("UAssetRegistryImpl::GetAssetsByClass");
    if (!fn) { why = "UAssetRegistryImpl::GetAssetsByClass unresolved"; return false; }
    void* regCls = Reflect::FindObjectByPath("/Script/AssetRegistry", "AssetRegistryImpl");
    std::vector<void*> regs;
    void* reg = nullptr;
    if (regCls && Reflect::GetObjectsOfClass(regCls, regs, true))
        for (void* r : regs)
            if (r) { reg = r; break; }
    if (!reg) { why = "no live UAssetRegistryImpl"; return false; }

    UE::FTopLevelAssetPath path;
    path.PackageName = Reflect::MakeName(pkg);
    path.AssetName = Reflect::MakeName(cls);
    if (!path.PackageName.Comparison || !path.AssetName.Comparison) {
        why = std::string("the name pool has no ") + pkg + "." + cls;
        return false;
    }
    TArray<char> arr{};
    if (!fn(reg, path, &arr, false) || !arr.Data || arr.Num <= 0) {
        why = "the asset registry returned nothing for that class";
        return false;
    }
    int32_t num = arr.Num;
    if (num > 100000) { why = "implausible asset count"; return false; }

    // Find the layout: FAssetData is at most a few hundred bytes and 8-byte aligned.
    size_t stride = 0;
    int32_t classOff = -1;
    for (size_t st = 40; st <= 256 && !stride; st += 8) {
        if (!MemReadable(arr.Data, (size_t)num * st)) break;
        for (int32_t off = 0; off + 16 <= (int32_t)st; off += 8) {
            bool all = true;
            for (int32_t i = 0; i < num && all; i++) {
                UE::FName a{}, b{};
                memcpy(&a, arr.Data + (size_t)i * st + off, 8);
                memcpy(&b, arr.Data + (size_t)i * st + off + 8, 8);
                all = a == path.PackageName && b == path.AssetName;
            }
            if (all) { stride = st; classOff = off; break; }
        }
    }
    auto free_ = Fn<void (*)(void*)>("FMemory::Free");
    if (!stride) {
        if (free_) free_(arr.Data);
        why = "FAssetData's layout could not be confirmed against the class path we asked for";
        return false;
    }
    // AssetName is the FName two words before AssetClassPath (PackageName, PackagePath, AssetName,
    // AssetClassPath); confirm the words exist before reading them.
    int32_t nameOff = classOff - 8;
    int32_t pkgOff = classOff - 24;
    for (int32_t i = 0; i < num; i++) {
        const char* e = arr.Data + (size_t)i * stride;
        RegistryAsset ra;
        if (nameOff >= 0) {
            UE::FName n{};
            memcpy(&n, e + nameOff, 8);
            ra.name = Reflect::NameToString(n);
        }
        if (pkgOff >= 0) {
            UE::FName n{};
            memcpy(&n, e + pkgOff, 8);
            ra.package = Reflect::NameToString(n);
        }
        if (!ra.name.empty()) out.push_back(ra);
    }
    if (free_) free_(arr.Data);
    if (out.empty()) { why = "the registry rows carried no readable asset name"; return false; }
    PluginLog("actions: asset registry returned %d %s.%s asset(s) (stride %zu, classOff %d)", num, pkg, cls, stride,
              classOff);
    return true;
}

Mutex g_entityLock;
bool g_entitiesFromRegistry = false;
std::vector<RegistryAsset> g_registryEntities;
std::string g_registryWhy;
bool g_registryTried = false;

}  // namespace

Actions::Result Actions::Entities() {
    return OnGameThread("GET /entities", []() -> JobOut {
        // Two sources: loaded UAIDataAsset assets (rich names) and every class deriving from
        // ADominionAICharacter (always present, even when no AI asset has been loaded yet).
        std::map<std::string, std::pair<std::string, std::string>> found;  // code -> {name, description}
        void* assetCls = Reflect::FindObjectByPath("/Script/Dominion", "AIDataAsset");
        std::vector<void*> objs;
        if (assetCls && Reflect::GetObjectsOfClass(assetCls, objs, true)) {
            for (void* a : objs) {
                std::string code = Reflect::ObjName(a);
                if (code.empty() || code.rfind("Default__", 0) == 0) continue;
                std::string name = TextToString(a, Off(a, "AIName"));
                int32_t bossOff = Off(a, "bIsBoss");
                bool boss = bossOff >= 0 && MemReadable((const char*)a + bossOff, 1) &&
                            *(const uint8_t*)((const char*)a + bossOff);
                if (name.empty()) name = HumanizeClassName(code);
                if (name.empty()) continue;
                found[code] = {name, boss ? "boss" : "AI data asset"};
            }
        }
        void* aiCls = Reflect::StaticClass("ADominionAICharacter::StaticClass");
        if (!aiCls) aiCls = Reflect::FindObjectByPath("/Script/Dominion", "DominionAICharacter");
        void* classCls = Reflect::StaticClass("UClass::StaticClass");
        std::vector<void*> classes;
        if (aiCls && classCls && Reflect::GetObjectsOfClass(classCls, classes, true)) {
            for (void* c : classes) {
                bool derives = false;
                for (void* s = c; s && !derives; s = Reflect::SuperStruct(s)) derives = s == aiCls;
                if (!derives || c == aiCls) continue;
                std::string code = Reflect::ObjName(c);
                if (code.empty() || code.rfind("SKEL_", 0) == 0 || code.rfind("REINST_", 0) == 0) continue;
                if (found.count(code)) continue;
                std::string human = HumanizeClassName(code);
                if (human.empty()) continue;  // engine base class, not a creature of its own
                found[code] = {human, "AI character class"};
                // NOT the class default object: a Blueprint CDO's AIName is not initialised on this
                // build and reads back as another asset's text (BP_AI_KalphiteGuardian_Character_C
                // came back as "Giant Rat"). Only a spawned AI's AIName is trustworthy.
            }
        }
        // Every AI currently spawned in the world knows its own readable name: ADominionAICharacter
        // fills AIName from the UAIDataAsset it was configured with. Cached per Blueprint class so
        // that the name survives the creature despawning, and so entity-killed reports the same one.
        if (aiCls) {
            std::vector<void*> live;
            if (Reflect::GetObjectsOfClass(aiCls, live, true)) {
                for (void* a : live) {
                    if (!a) continue;
                    std::string inst = Reflect::ObjName(a);
                    if (inst.rfind("Default__", 0) == 0) continue;
                    std::string code = Reflect::ClassName(a);
                    std::string nm = TextToString(a, Off(a, "AIName"));
                    if (!code.empty() && !nm.empty()) state::NoteEntityName(code, nm);
                }
            }
        }
        // Third source, and the only complete one: the cooked asset registry.
        {
            Guard g(g_entityLock);
            if (!g_registryTried) {
                g_registryTried = true;
                g_entitiesFromRegistry =
                    RegistryAssetsByClass("/Script/Dominion", "AIDataAsset", g_registryEntities, g_registryWhy);
                if (!g_entitiesFromRegistry)
                    PluginLog("actions: asset-registry entity list unavailable: %s", g_registryWhy.c_str());
            }
            for (auto& ra : g_registryEntities)
                if (!found.count(ra.name)) {
                    std::string human = HumanizeClassName(ra.name);
                    if (!human.empty()) found[ra.name] = {human, "AI data asset (asset registry)"};
                }
            SetCap("listEntities", g_entitiesFromRegistry ? "ok" : "degraded",
                   g_entitiesFromRegistry
                       ? "every cooked UAIDataAsset, enumerated through the asset registry, plus the AI "
                         "character classes and data assets currently loaded"
                       : "AI data assets are streamed in on demand and the asset registry could not be used (" +
                             g_registryWhy + "): the list is the AI character classes and data assets loaded so far");
        }
        if (found.empty()) return {503, ErrJson("no AI data assets or AI character classes are loaded")};
        // One naming rule for the whole connector: `code` is the class/asset name, `name` is the
        // AIName display text whenever any AI of that class has been seen. entity-killed uses the
        // same cache, so Takaro's entity list and its kill events line up.
        return {200, [found = std::move(found)]() mutable {
            for (auto& kv : found) {
                std::string nm = state::EntityName(kv.first);
                if (!nm.empty()) kv.second.first = nm;
            }
            std::string o = "[";
            bool first = true;
            for (auto& kv : found) {
                if (!first) o += ",";
                first = false;
                o += "{\"code\":" + JsonStr(kv.first) + ",\"name\":" + JsonStr(kv.second.first) +
                     ",\"type\":\"hostile\",\"description\":" + JsonStr(kv.second.second) + "}";
            }
            return o + "]";
        }};
    });
}

Actions::Result Actions::Locations() {
    return OnGameThread("GET /locations", []() -> JobOut {
        auto locations = ReadLocations();
        return {200, [locations = std::move(locations)] {
            std::string o = "[";
            bool first = true;
            for (auto& l : locations) {
                if (!first) o += ",";
                first = false;
                // The lodestone actor carries no display name on this build (its object name is an
                // editor id such as StaticMeshActor_UAID_...), so the name players see is built from
                // what it is and where it stands, in metres; `code` keeps the actor name for teleports.
                char where[96];
                snprintf(where, sizeof where, "Lodestone (%ld, %ld)", lround(l.x / 100.0), lround(l.y / 100.0));
                o += "{\"code\":" + JsonStr(l.code) + ",\"name\":" + JsonStr(where) + ",\"position\":{\"x\":" +
                     JsonNum(l.x) + ",\"y\":" + JsonNum(l.y) + ",\"z\":" + JsonNum(l.z) + "}}";
            }
            return o + "]";
        }};
    });
}

Actions::Result Actions::Bans() {
    return OnGameThread("GET /bans", []() -> JobOut {
        // The union of the two lists: the game's own KnownPlayerList flags (which it re-reads at
        // start-up) and the plugin list (which the PreLogin hook - when bound - and the PostLogin
        // ban kick enforce live). `enforcedBy` says which mechanism refuses the rejoin *now*:
        // "plugin" when the PreLogin hook is installed and the id is in our list, "game" for an
        // entry only the game's own list holds. `inGameList` / `inPluginList` are the raw facts the
        // native bridge's ban recovery verifies against.
        auto gameRows = ReadBans();
        bool live = Events::BanEnforcementLive();
        return {200, [gameRows = std::move(gameRows), live] {
            std::map<std::string, std::string> names;  // gameId -> name, from the game's list
            std::set<std::string> inGame;
            std::vector<std::string> order;
            for (auto& b : gameRows) {
                if (b.gameId.empty()) continue;
                if (!names.count(b.gameId)) order.push_back(b.gameId);
                names[b.gameId] = b.name;
                inGame.insert(b.gameId);
            }
            std::map<std::string, state::BanRecord> plugin;
            for (auto& b : state::BanList()) {
                plugin[b.gameId] = b;
                if (!names.count(b.gameId)) {
                    names[b.gameId] = b.name;
                    order.push_back(b.gameId);
                }
            }
            std::string o = "[";
            bool first = true;
            for (auto& id : order) {
                auto it = plugin.find(id);
                bool inPlugin = it != plugin.end();
                if (!first) o += ",";
                first = false;
                std::string name = names[id];
                if (name.empty() && inPlugin) name = it->second.name;
                o += "{\"gameId\":" + JsonStr(id) + ",\"name\":" + JsonStr(name) + ",\"reason\":" +
                     JsonStr(inPlugin ? it->second.reason : std::string()) + ",\"expiresAt\":" +
                     ((inPlugin && !it->second.expiresAt.empty()) ? JsonStr(it->second.expiresAt)
                                                                  : std::string("null")) +
                     ",\"createdAt\":" + ((inPlugin && !it->second.createdAt.empty()) ? JsonStr(it->second.createdAt)
                                                                                      : std::string("null")) +
                     ",\"enforcedBy\":" + JsonStr((live && inPlugin) ? "plugin" : "game") +
                     ",\"inGameList\":" + (inGame.count(id) ? "true" : "false") +
                     ",\"inPluginList\":" + (inPlugin ? "true" : "false") + "}";
            }
            return o + "]";
        }};
    });
}

Actions::Result Actions::Message(const JsonValue& body) {
    const JsonValue* textV = body.get("text");
    if (!textV || !textV->isStr() || textV->str.empty()) return Fail(400, "'text' is required");
    std::string text = textV->str;
    const JsonValue* rcpt = body.get("recipientGameId");
    std::string recipient = rcpt && rcpt->isStr() ? rcpt->str : "";
    const JsonValue* snd = body.get("senderName");
    std::string sender = snd && snd->isStr() ? snd->str : ConfigValue("TAKARO_SENDER_NAME", "senderName", "Server");

    return OnGameThread("POST /message", [text, recipient, sender]() -> JobOut {
        auto players = ReadPlayers();
        if (players.empty()) return {200, "{\"success\":true,\"delivered\":0,\"detail\":\"no players online\"}"};
        size_t delivered = 0;
        std::string lastErr;
        std::string needle = NormalizeGameId(recipient);
        bool targeted = !recipient.empty();
        bool found = false;
        for (auto& p : players) {
            if (targeted && Lower(p.gameId) != needle && Lower(p.characterName) != Lower(recipient) &&
                Lower(p.name) != Lower(recipient))
                continue;
            found = true;
            std::string err;
            if (SendChatTo(p, text, sender, err)) delivered++;
            else lastErr = err;
        }
        if (targeted && !found) return {404, ErrJson("player not online")};
        if (!delivered) {
            SetCap("sendMessage", "degraded", lastErr);
            return {503, ErrJson(lastErr.empty() ? "message could not be delivered" : lastErr)};
        }
        SetCap("sendMessage", "ok", "");
        return {200, "{\"success\":true,\"delivered\":" + std::to_string(delivered) + "}"};
    });
}

Actions::Result Actions::Teleport(const JsonValue& body) {
    const JsonValue* idV = body.get("gameId");
    if (!idV || !idV->isStr()) return Fail(400, "'gameId' is required");
    std::string id = idV->str;
    const JsonValue* tgt = body.get("target");
    std::string target = tgt && tgt->isStr() ? tgt->str : "";
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

    return OnGameThread("POST /teleport", [id, target, haveXyz, x, y, z, haveYaw, yaw]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(id, p)) return {404, ErrJson("player not online")};
        if (!p.pawn) return {503, ErrJson("player has no pawn yet")};
        auto tp = Fn<FnTeleportTo>("AActor::TeleportTo");
        if (!tp) return {501, ErrJson("AActor::TeleportTo unresolved")};
        double dest[3] = {x, y, z};
        double rot[3] = {0, 0, 0};
        double cur[3] = {0, 0, 0};
        PawnLocation(p.pawn, cur, rot);
        if (!haveXyz) {
            bool found = false;
            for (auto& l : ReadLocations()) {
                if (Lower(l.code) != Lower(target)) continue;
                dest[0] = l.x;
                dest[1] = l.y;
                dest[2] = l.z;
                found = true;
                break;
            }
            if (!found && NamedLocationTransform(target, dest)) found = true;
            if (!found) return {404, ErrJson("unknown teleport target '" + target + "'")};
        }
        if (haveYaw) rot[1] = yaw;
        bool ok = tp(p.pawn, dest, rot, false, true);
        if (!ok) {
            // Retry ignoring collision checks entirely before giving up.
            ok = tp(p.pawn, dest, rot, false, false);
        }
        if (!ok) return {409, ErrJson("the game refused the teleport (blocked destination)")};
        double after[3] = {0, 0, 0}, arot[3] = {0, 0, 0};
        PawnLocation(p.pawn, after, arot);
        return {200, "{\"success\":true,\"position\":{\"x\":" + JsonNum(after[0]) + ",\"y\":" + JsonNum(after[1]) +
                         ",\"z\":" + JsonNum(after[2]) + "}}"};
    });
}

Actions::Result Actions::Give(const JsonValue& body) {
    const JsonValue* idV = body.get("gameId");
    const JsonValue* codeV = body.get("code");
    if (!idV || !idV->isStr()) return Fail(400, "'gameId' is required");
    if (!codeV || !codeV->isStr() || codeV->str.empty()) return Fail(400, "'code' is required");
    const JsonValue* amtV = body.get("amount");
    int amount = amtV && amtV->isNum() ? (int)amtV->num : 1;
    if (amount <= 0) return Fail(400, "'amount' must be positive");
    std::string id = idV->str, code = codeV->str;

    Housekeep();  // make sure the catalogue exists before the lookup
    std::string lookupErr;
    const ItemDef* def = nullptr;
    void* asset = nullptr;
    std::string resolvedCode, resolvedName;
    {
        def = LookupItem(code, lookupErr);
        if (!def) return Fail(lookupErr.find("not loaded") != std::string::npos ? 503 : 404, lookupErr);
        asset = def->asset;
        resolvedCode = def->code;
        resolvedName = def->name;
    }

    return OnGameThread("POST /give", [id, amount, asset, resolvedCode, resolvedName]() -> JobOut {
        PlayerInfo p;
        if (!FindPlayerById(id, p)) return {404, ErrJson("player not online")};
        if (!p.controller) return {503, ErrJson("player has no controller")};
        auto give = Fn<FnTryGiveItem>("UDominionRuntimeBlueprintLibrary::TryGiveItemToPlayer");
        if (!give) return {501, ErrJson("TryGiveItemToPlayer unresolved")};
        bool ok = give(p.controller, asset, amount);
        if (!ok) return {409, ErrJson("the game refused the item (inventory full?)")};
        return {200, "{\"success\":true,\"code\":" + JsonStr(resolvedCode) + ",\"name\":" + JsonStr(resolvedName) +
                         ",\"amount\":" + std::to_string(amount) + "}"};
    });
}

namespace {

// Kick or ban an online player through ADominionGameSession.
JobOut SessionAction(const std::string& id, const std::string& reason, bool ban, bool& wasOnline) {
    PlayerInfo p;
    wasOnline = FindPlayerById(id, p);
    if (!wasOnline) return {404, ErrJson("player not online")};
    void* gm = GameModeOf(FindWorld());
    void* session = gm ? ReadPtrAt(gm, Off(gm, "GameSession")) : nullptr;
    auto fromString = Fn<FnTextFromString>("FText::FromString");
    auto fn = Fn<FnKickBan>(ban ? "ADominionGameSession::BanPlayer" : "ADominionGameSession::KickPlayer");
    if (!session || !fromString || !fn || !p.controller)
        return {501, ErrJson("game session / FText::FromString unavailable")};
    TempFString reasonStr(reason.empty() ? (ban ? "Banned" : "Kicked") : reason);
    char ftext[16];
    memset(ftext, 0, sizeof ftext);
    fromString(ftext, &reasonStr.fs);
    fn(session, p.controller, ftext);
    return {200, "{\"success\":true,\"gameId\":" + JsonStr(p.gameId) + ",\"online\":true}"};
}

std::string BodyString(const JsonValue& body, const char* key) {
    const JsonValue* v = body.get(key);
    return v && v->isStr() ? v->str : "";
}

}  // namespace

// Called from lane L2's join resolver, already on the game thread.
bool Actions::KickBanned(const std::string& gameId) {
    bool online = false;
    JobOut r = SessionAction(gameId, "Banned", true, online);
    return online && r.status == 200;
}

Actions::Result Actions::Kick(const JsonValue& body) {
    std::string id = BodyString(body, "gameId");
    if (id.empty()) return Fail(400, "'gameId' is required");
    std::string reason = BodyString(body, "reason");
    return OnGameThread("POST /kick", [id, reason]() -> JobOut {
        bool online = false;
        return SessionAction(id, reason, false, online);
    });
}

Actions::Result Actions::Ban(const JsonValue& body) {
    std::string id = BodyString(body, "gameId");
    if (id.empty()) return Fail(400, "'gameId' is required");
    std::string reason = BodyString(body, "reason");
    // ISO-8601 UTC; empty = permanent. The plugin only stores it: the native bridge lifts the ban
    // when it expires (Takaro never sends an unban for a timed ban).
    std::string expiresAt = BodyString(body, "expiresAt");
    Result result = OnGameThread("POST /ban", [id, reason, expiresAt,
                                              lifetime = std::make_shared<BanJobLifetime>()]() -> JobOut {
        bool online = false;
        PlayerInfo found;
        std::string name = FindPlayerById(id, found) ? (found.characterName.empty() ? found.name : found.characterName)
                                                     : std::string();
        // The plugin list is written first and unconditionally: it accepts a gameId the server has
        // never seen, it survives a restart on its own (flushed by the caller, off the game
        // thread), and it is what PreLogin / the PostLogin ban kick refuse a rejoin with while the
        // server keeps running.
        state::BanRecord rec;
        rec.gameId = NormalizeGameId(id);
        rec.name = name;
        rec.reason = reason;
        rec.expiresAt = expiresAt;
        bool pluginList = state::BanAdd(rec);
        std::string normalized = NormalizeGameId(id);
        const char* enforcedBy = Events::BanEnforcementLive() ? "plugin" : "game";
        JobOut r = SessionAction(id, reason, true, online);
        if (online) {
            // ADominionGameSession::BanPlayer disconnects the player but (on this build) does not
            // flag them in KnownPlayerList, so the persistent flag is always written here too.
            std::string err;
            bool persisted = WriteBans(normalized, "", true, err);
            if (r.status != 200) return r;
            return {200, [normalized, persisted, pluginList, enforcedBy, err] {
                return "{\"success\":true,\"gameId\":" + JsonStr(normalized) + ",\"online\":true,\"persisted\":" +
                       (persisted ? "true" : "false") + ",\"pluginList\":" + (pluginList ? "true" : "false") +
                       ",\"enforcedBy\":" + JsonStr(enforcedBy) + ",\"detail\":" + JsonStr(err) + "}";
            }};
        }
        // Offline: edit the settings list and persist it to DedicatedServer.ini. That only works
        // for a player the server has seen before; the plugin list (written above) always does.
        std::string err;
        bool gameList = WriteBans(normalized, name, true, err);
        if (!gameList && !pluginList) {
            SetCap("ban", "degraded", "offline ban failed: " + err);
            return JobOut::Error(503, "offline ban failed: " + err);
        }
        return {200, [normalized, gameList, pluginList, enforcedBy, err] {
            return "{\"success\":true,\"gameId\":" + JsonStr(normalized) + ",\"online\":false,\"persisted\":" +
                   (gameList ? "true" : "false") + ",\"pluginList\":" + (pluginList ? "true" : "false") +
                   ",\"enforcedBy\":" + JsonStr(enforcedBy) + ",\"detail\":" +
                   JsonStr(gameList ? (err.empty() ? "added to the server ban list" : err)
                                    : ("the game's own list refused it (" + err + "); the plugin ban list carries it")) +
                   "}";
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
        // A timed-ban expiry captured the ban revision when it was scheduled; a newer ban (a
        // permanent one, or a longer one) must never be lifted by the old job.
        if (checkRevision && state::BanRevision() != expectedRevision)
            return JobOut::Error(409, "ban changed before timed expiry; preserving current ban");
        std::string normalized = NormalizeGameId(id);
        std::string err;
        bool pluginList = state::BanRemove(normalized);
        bool gameList = WriteBans(normalized, "", false, err);
        if (!gameList && !pluginList) {
            // Neither list holds it: for a player the server never saw and we never banned that is
            // the desired state already.
            bool knownToGame = err.find("never seen") == std::string::npos;
            if (knownToGame) {
                SetCap("unban", "degraded", err);
                return JobOut::Error(503, "unban failed: " + err);
            }
        }
        return {200, [normalized, pluginList, gameList, err] {
            return "{\"success\":true,\"gameId\":" + JsonStr(normalized) + ",\"pluginList\":" +
                   (pluginList ? "true" : "false") + ",\"persisted\":" + (gameList ? "true" : "false") +
                   ",\"detail\":" +
                   JsonStr(gameList ? (err.empty() ? "removed from the server ban list" : err)
                                    : (pluginList ? "removed from the plugin ban list" : "no ban held for that id")) +
                   "}";
        }};
    });
    if (!state::FlushBans()) return Fail(503, state::BanPersistenceError());
    return result;
}

Actions::Result Actions::Unban(const JsonValue& body) { return UnbanWithRevision(body, false, 0); }
Actions::Result Actions::UnbanIfRevision(const JsonValue& body, uint64_t expectedRevision) {
    return UnbanWithRevision(body, true, expectedRevision);
}

// ================================================================================================
// POST /debug/kill-nearest  (debug builds only; the HTTP layer gates it on TAKARO_PLUGIN_DEBUG)

namespace {
using FnApplyDamage = float (*)(void* damaged, float amount, void* instigatorController, void* causer, void* dmgType);
using FnDecreaseHealth = void (*)(void* self, float amount, const FString* reason);
using FnGetLocalHealth = float (*)(const void* self);

// The health component hanging off an actor.
void* HealthComponentOf(void* actor) {
    void* cls = Reflect::StaticClass("UHealthComponent::StaticClass");
    if (!cls) cls = Reflect::FindObjectByPath("/Script/Dominion", "HealthComponent");
    if (!cls || !actor) return nullptr;
    std::vector<void*> comps;
    if (!Reflect::GetObjectsWithOuter(actor, comps, true)) return nullptr;
    for (void* c : comps)
        if (c && Reflect::IsA(c, cls)) return c;
    return nullptr;
}

double Dist(const double a[3], const double b[3]) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

Actions::Result Actions::KillNearest(const JsonValue& body) {
    std::string id = BodyString(body, "gameId");
    const JsonValue* rv = body.get("radius");
    double radius = rv && rv->isNum() ? rv->num : 6000.0;
    return OnGameThread("POST /debug/kill-nearest", [id, radius]() -> JobOut {
        PlayerInfo p;
        if (!id.empty() && !FindPlayerById(id, p)) {
            auto all = ReadPlayers();
            if (all.empty()) return {404, ErrJson("no player online")};
            p = all[0];
        } else if (id.empty()) {
            auto all = ReadPlayers();
            if (all.empty()) return {404, ErrJson("no player online")};
            p = all[0];
        }
        if (!p.pawn) return {503, ErrJson("player has no pawn")};
        double me[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
        if (!PawnLocation(p.pawn, me, rot)) return {503, ErrJson("pawn has no readable root component")};

        void* aiCls = Reflect::StaticClass("ADominionAICharacter::StaticClass");
        if (!aiCls) aiCls = Reflect::FindObjectByPath("/Script/Dominion", "DominionAICharacter");
        if (!aiCls) return {503, ErrJson("ADominionAICharacter class not found")};
        std::vector<void*> ais;
        Reflect::GetObjectsOfClass(aiCls, ais, true);
        void* best = nullptr;
        double bestDist = radius;
        size_t considered = 0;
        std::string listed;
        auto liveHealth = Fn<FnGetLocalHealth>("UHealthComponent::GetLocalHealth");
        for (void* a : ais) {
            if (!a || !MemReadable(a, 0x40)) continue;
            std::string cn = Reflect::ClassName(a);
            if (cn.rfind("Default__", 0) == 0) continue;
            // A corpse is still an ADominionAICharacter until it despawns: never pick a dead one.
            if (void* hc0 = HealthComponentOf(a); hc0 && liveHealth && liveHealth(hc0) <= 0.f) continue;
            double loc[3] = {0, 0, 0}, r2[3] = {0, 0, 0};
            if (!PawnLocation(a, loc, r2)) continue;
            considered++;
            double d = Dist(me, loc);
            if (listed.size() < 1200)
                listed += (listed.empty() ? "" : ",") + cn + "@" + std::to_string((long)d) + "(" +
                          std::to_string((long)loc[0]) + " " + std::to_string((long)loc[1]) + " " +
                          std::to_string((long)loc[2]) + ")";
            if (d < bestDist) { bestDist = d; best = a; }
        }
        if (!best)
            return {404, "{\"error\":\"no AI character within radius\",\"considered\":" + std::to_string(considered) +
                             ",\"nearby\":" + JsonStr(listed) + "}"};

        std::string victim = Reflect::ObjName(best), victimClass = Reflect::ClassName(best);
        void* hc = HealthComponentOf(best);
        auto getHealth = Fn<FnGetLocalHealth>("UHealthComponent::GetLocalHealth");
        float before = hc && getHealth ? getHealth(hc) : -1.f;

        // 1. the game's own damage pipeline, with the player as instigator.
        std::string method;
        auto applyDamage = Fn<FnApplyDamage>("UGameplayStatics::ApplyDamage");
        // The damage-type class must never be null: UGameplayStatics::ApplyDamage dereferences the
        // TSubclassOf without checking it (SIGSEGV at 0x0, rig log RSDragonwilds-backup-2026.09.16-20.20.05).
        void* dmgType = Reflect::FindObjectByPath("/Script/Engine", "DamageType");
        float dealt = -1.f;
        if (applyDamage && p.controller && dmgType) {
            dealt = applyDamage(best, 999999.f, p.controller, p.pawn, dmgType);
            method = "UGameplayStatics::ApplyDamage";
        } else if (!dmgType) {
            method = "UDamageType class not found, skipped ApplyDamage";
        }
        float after = hc && getHealth ? getHealth(hc) : -1.f;
        // 2. if that did nothing, drain the health component directly.
        if (hc && after > 0.f) {
            auto dec = Fn<FnDecreaseHealth>("UHealthComponent::DecreaseHealth");
            if (dec) {
                TempFString why("takaro debug kill");
                dec(hc, after + 1000.f, &why.fs);
                method += method.empty() ? "UHealthComponent::DecreaseHealth" : " + UHealthComponent::DecreaseHealth";
                after = getHealth ? getHealth(hc) : after;
            }
        }
        return {200, "{\"success\":true,\"victim\":" + JsonStr(victim) + ",\"victimClass\":" + JsonStr(victimClass) +
                         ",\"distance\":" + JsonNum(bestDist) + ",\"method\":" + JsonStr(method) +
                         ",\"damageReturned\":" + JsonNum(dealt) + ",\"healthBefore\":" + JsonNum(before) +
                         ",\"healthAfter\":" + JsonNum(after) + ",\"considered\":" + std::to_string(considered) +
                         ",\"killer\":" + JsonStr(p.gameId) + "}"};
    });
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
// POST /command

namespace {

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t start = i;
        while (i < s.size() && !isspace((unsigned char)s[i])) i++;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::string Rest(const std::string& s, size_t skipWords) {
    size_t i = 0, w = 0;
    while (i < s.size() && w < skipWords) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        while (i < s.size() && !isspace((unsigned char)s[i])) i++;
        w++;
    }
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

const char* kHelp =
    "players | say <msg> | whisper <gameId> <msg> | give <gameId> <code> [n] | tp <gameId> <x> <y> <z> | "
    "kick <gameId> [reason] | ban <gameId> [reason] | unban <gameId> | bans | items [query] | entities | "
    "locations | save | shutdown | raw <console command> | cheat <gameId> <cmd> | help";

JobOut CommandOutput(bool success, const std::string& out) {
    return {200, "{\"success\":" + std::string(success ? "true" : "false") + ",\"output\":" + JsonStr(out) + "}"};
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

bool RunExec(const std::string& cmd, std::string& out, std::string& err, bool& handled) {
    handled = false;
    auto exec = Fn<FnEngineExec>("UEngine::Exec");
    if (!exec) {
        err = "UEngine::Exec unresolved";
        return false;
    }
    void* engineCls = Reflect::FindObjectByPath("/Script/Engine", "Engine");
    std::vector<void*> engines;
    if (engineCls) Reflect::GetObjectsOfClass(engineCls, engines, true);
    void* engine = engines.empty() ? nullptr : engines[0];
    void* world = FindWorld();
    if (!engine) {
        err = "no live UEngine object";
        return false;
    }
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
    // FOutputDevice declares Serialize(msg, verbosity, category, time) immediately before
    // Serialize(msg, verbosity, category), so the slot after the one holding the resolved 4-argument
    // implementation is the 3-argument one.
    uint64_t ser4 = Sym::Addr("FOutputDeviceFile::Serialize");
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
    handled = exec(engine, world, w.data(), &dev);
    out = g_execOutput;
    return true;
}

}  // namespace

Actions::Result Actions::Command(const JsonValue& body) {
    std::string command = BodyString(body, "command");
    if (command.empty()) return Fail(400, "'command' is required");
    auto w = Words(command);
    if (w.empty()) return Fail(400, "'command' is required");
    std::string verb = Lower(w[0]);

    if (verb == "help") return {200, "{\"success\":true,\"output\":" + JsonStr(kHelp) + "}"};

    if (verb == "players") {
        Result r = Players();
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    }
    if (verb == "bans") {
        Result r = Bans();
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    }
    if (verb == "items") {
        Result r = Items(w.size() > 1 ? Rest(command, 1) : "");
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    }
    if (verb == "entities" || verb == "locations") {
        Result r = verb == "entities" ? Entities() : Locations();
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    }
    auto wrap = [&](const Result& r) -> Result {
        if (r.status != 200) return r;
        return {200, "{\"success\":true,\"output\":" + JsonStr(r.body) + "}"};
    };
    if (verb == "say") {
        if (w.size() < 2) return Fail(400, "usage: say <message>");
        JsonValue b;
        b.type = JsonValue::Object;
        JsonValue t;
        t.type = JsonValue::String;
        t.str = Rest(command, 1);
        b.obj.push_back({"text", t});
        return wrap(Message(b));
    }
    if (verb == "whisper") {
        if (w.size() < 3) return Fail(400, "usage: whisper <gameId> <message>");
        JsonValue b;
        b.type = JsonValue::Object;
        JsonValue t, r;
        t.type = r.type = JsonValue::String;
        t.str = Rest(command, 2);
        r.str = w[1];
        b.obj.push_back({"text", t});
        b.obj.push_back({"recipientGameId", r});
        return wrap(Message(b));
    }
    if (verb == "give") {
        if (w.size() < 3) return Fail(400, "usage: give <gameId> <code> [amount]");
        JsonValue b;
        b.type = JsonValue::Object;
        JsonValue id, code, amt;
        id.type = code.type = JsonValue::String;
        id.str = w[1];
        code.str = w[2];
        amt.type = JsonValue::Number;
        amt.num = w.size() > 3 ? atof(w[3].c_str()) : 1;
        amt.str = w.size() > 3 ? w[3] : "1";
        b.obj.push_back({"gameId", id});
        b.obj.push_back({"code", code});
        b.obj.push_back({"amount", amt});
        return wrap(Give(b));
    }
    if (verb == "tp" || verb == "teleport") {
        if (w.size() < 5) return Fail(400, "usage: tp <gameId> <x> <y> <z>");
        JsonValue b;
        b.type = JsonValue::Object;
        JsonValue id;
        id.type = JsonValue::String;
        id.str = w[1];
        b.obj.push_back({"gameId", id});
        const char* keys[] = {"x", "y", "z"};
        for (int i = 0; i < 3; i++) {
            JsonValue n;
            n.type = JsonValue::Number;
            n.num = atof(w[2 + i].c_str());
            n.str = w[2 + i];
            b.obj.push_back({keys[i], n});
        }
        return wrap(Teleport(b));
    }
    if (verb == "kick" || verb == "ban" || verb == "unban") {
        if (w.size() < 2) return Fail(400, "usage: " + verb + " <gameId> [reason]");
        JsonValue b;
        b.type = JsonValue::Object;
        JsonValue id, reason;
        id.type = reason.type = JsonValue::String;
        id.str = w[1];
        reason.str = w.size() > 2 ? Rest(command, 2) : "";
        b.obj.push_back({"gameId", id});
        if (!reason.str.empty()) b.obj.push_back({"reason", reason});
        return wrap(verb == "kick" ? Kick(b) : verb == "ban" ? Ban(b) : Unban(b));
    }
    if (verb == "save") {
        return OnGameThread("command save", []() -> JobOut {
            void* gm = GameModeOf(FindWorld());
            auto request = Fn<FnVoidSelf>("ADominionGameMode::RequestSaveGame");
            if (!gm || !request) return {501, ErrJson("RequestSaveGame unresolved")};
            request(gm);
            return CommandOutput(true, "save requested");
        });
    }
    if (verb == "shutdown") return Shutdown();
    if (verb == "cheat") {
        return {501,
                "{\"error\":\"unimplemented\",\"capability\":\"executeCommand\",\"detail\":\"cheat commands need a "
                "CheatManager, which the Shipping dedicated server does not create\"}"};
    }
    if (verb == "raw") {
        if (w.size() < 2) return Fail(400, "usage: raw <console command>");
        std::string cmd = Rest(command, 1);
        return OnGameThread("command raw", [cmd]() -> JobOut {
            std::string out, err;
            bool handled = false;
            if (!RunExec(cmd, out, err, handled)) return {501, ErrJson(err)};
            // UEngine::Exec returning false with no output = no exec handler took the command.
            if (!handled && out.empty()) return {400, ErrJson("command not recognised by the engine: " + cmd)};
            return CommandOutput(true, out);
        }, 10000);
    }
    return Fail(400, std::string("unknown command '") + w[0] + "'. " + kHelp);
}
