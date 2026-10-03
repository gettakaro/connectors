#include "ue/ue.h"

#include "common.h"
#include "conan/text.h"

#include <cstring>
#include <unordered_map>

namespace UE {
namespace {

uintptr_t kGObjObjects = 0;  // FChunkedFixedUObjectArray, from the pins
uintptr_t kNameBlocks = 0;   // FNamePool blocks; CurrentBlock/Cursor just before, from the pins
ProcessEventFn g_callProcessEvent = nullptr;

// ---- core layout (verified live on 25639945; El-Limon context/games/conan-exiles/native-spike-349) ----
constexpr size_t kItemStride = 0x18;  // FUObjectItem: +4 internal flags, +8 UObject*
constexpr size_t kItemFlags = 0x04;
constexpr size_t kItemObject = 0x08;
constexpr int kChunkItems = 65536;
constexpr int kStepItems = 16384;  // objects per DiscoverStep; divides kChunkItems
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kStructSuper = 0x40, kStructChildProps = 0x50;
constexpr size_t kFieldClass = 0x08, kFieldNext = 0x18, kFieldName = 0x20;
constexpr size_t kFieldClassName = 0x08;
constexpr size_t kPropElementSize = 0x30, kPropOffset = 0x44;

constexpr uint32_t kRfSkip = 0x10 | 0x20 | 0x8000 | 0x10000;  // CDO, archetype, begin/finish destroyed
constexpr uint32_t kInternalDead = 0x10000000 | 0x20000000;   // unreachable, garbage
constexpr int32_t kMaxPlayers = 256;

template <typename T>
T rd(uintptr_t a) {
    T v;
    memcpy(&v, (const void*)a, sizeof v);
    return v;
}

bool EqualsN(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// ---- names ----
enum NameId {
    N_ClientReceiveChatMessage, N_ConanPlayerController, N_Function, N_Class, N_GameStateBase,
    N_PlayerArray, N_Owner, N_UserIDFromURLOptions, N_PlayerNamePrivate,
    N_ArrayProperty, N_ObjectProperty, N_StrProperty, N_Count
};
const char* const kNames[N_Count] = {
    "ClientReceiveChatMessage", "ConanPlayerController", "Function", "Class", "GameStateBase",
    "PlayerArray", "Owner", "UserIDFromURLOptions", "PlayerNamePrivate",
    "ArrayProperty", "ObjectProperty", "StrProperty",
};
uint32_t g_name[N_Count];
bool g_namesReady = false;

// One linear pass over the pool, matching every wanted ANSI name at once. FNames compare
// case-insensitively, and the pool keeps the casing that was registered first.
bool FindNames() {
    uint32_t found[N_Count] = {0};
    int missing = N_Count;
    uint32_t cur = rd<uint32_t>(kNameBlocks - 8), cursor = rd<uint32_t>(kNameBlocks - 4);
    if (cur > 8192) return false;
    size_t lens[N_Count];
    for (int i = 0; i < N_Count; i++) lens[i] = strlen(kNames[i]);
    for (uint32_t b = 0; b <= cur && missing; ++b) {
        uintptr_t base = rd<uintptr_t>(kNameBlocks + 8 * b);
        if (!base) continue;
        uint32_t size = b < cur ? 0x20000 : cursor;
        for (uint32_t off = 0; off + 2 <= size;) {
            uint16_t h = rd<uint16_t>(base + off);
            unsigned len = h >> 6;
            if (!len) break;
            bool wide = h & 1;
            if (!wide) {
                for (int i = 0; i < N_Count; i++) {
                    if (!found[i] && len == lens[i] && EqualsN((const char*)(base + off + 2), kNames[i], len)) {
                        found[i] = (b << 16) | (off / 2);
                        missing--;
                    }
                }
            }
            off += 2 + len * (wide ? 2 : 1);
            off += off & 1;
        }
    }
    if (missing) return false;
    memcpy(g_name, found, sizeof g_name);
    return true;
}

bool NameIs(uintptr_t fnameAddr, NameId id) {
    return rd<uint32_t>(fnameAddr) == g_name[id] && rd<uint32_t>(fnameAddr + 4) == 0;
}

// ---- objects ----
uintptr_t ItemAt(int32_t index) {
    uintptr_t objects = rd<uintptr_t>(kGObjObjects);
    int32_t num = rd<int32_t>(kGObjObjects + 8);
    if (!objects || index < 0 || index >= num) return 0;
    uintptr_t chunk = rd<uintptr_t>(objects + 8 * (uintptr_t)(index / kChunkItems));
    return chunk ? chunk + (uintptr_t)(index % kChunkItems) * kItemStride : 0;
}

// The object is still registered at its own index and not being destroyed.
bool Alive(uintptr_t obj) {
    if (!obj) return false;
    uintptr_t item = ItemAt(rd<int32_t>(obj + kObjIndex));
    if (!item || rd<uintptr_t>(item + kItemObject) != obj) return false;
    if (rd<uint32_t>(item + kItemFlags) & kInternalDead) return false;
    return !(rd<uint32_t>(obj + kObjFlags) & (0x8000 | 0x10000));
}

bool IsA(uintptr_t obj, uintptr_t cls) {
    int depth = 0;
    for (uintptr_t c = rd<uintptr_t>(obj + kObjClass); c && depth < 64; c = rd<uintptr_t>(c + kStructSuper), depth++)
        if (c == cls) return true;
    return false;
}

// Offset of a property declared on `cls` or a superclass, or -1 if missing or of another type.
int32_t PropertyOffset(uintptr_t cls, NameId name, NameId type, int32_t elementSize) {
    for (uintptr_t c = cls; c; c = rd<uintptr_t>(c + kStructSuper)) {
        for (uintptr_t p = rd<uintptr_t>(c + kStructChildProps); p; p = rd<uintptr_t>(p + kFieldNext)) {
            if (!NameIs(p + kFieldName, name)) continue;
            uintptr_t fc = rd<uintptr_t>(p + kFieldClass);
            if (!fc || !NameIs(fc + kFieldClassName, type)) return -1;
            if (rd<int32_t>(p + kPropElementSize) != elementSize) return -1;
            return rd<int32_t>(p + kPropOffset);
        }
    }
    return -1;
}

std::string ReadFString(uintptr_t at) {
    uintptr_t data = rd<uintptr_t>(at);
    int32_t num = rd<int32_t>(at + 8);
    if (!data || num <= 1 || num > 512) return "";
    std::u16string s((size_t)num - 1, u'\0');
    memcpy(&s[0], (const void*)data, ((size_t)num - 1) * 2);
    return conan::Utf16To8(s);
}

// ---- discovery state (game thread only) ----
struct Discovery {
    int32_t next = 0;  // next object index to scan
    uintptr_t chatFunc = 0, pcClass = 0, gsClass = 0;
    std::vector<uintptr_t> gameStates;
    std::unordered_map<uintptr_t, bool> gsClassCache;
    uint64_t scanNs = 0;
};
// Function-local: independent of static init order (the library constructor runs first).
struct State {
    Discovery scan;   // in progress
    Discovery ready;  // last complete pass
    std::string error = "discovery has not run";
};
State& St() {
    static State* s = new State;
    return *s;
}
bool g_haveReady = false;
int32_t g_offPlayerArray = -1, g_offOwner = -1, g_offUserId = -1, g_offPlayerName = -1;

bool ResolveOffsets(const Discovery& d) {
    g_offPlayerArray = PropertyOffset(d.gsClass, N_PlayerArray, N_ArrayProperty, 0x10);
    g_offUserId = PropertyOffset(d.pcClass, N_UserIDFromURLOptions, N_StrProperty, 0x10);
    // Owner (AActor) and PlayerNamePrivate (APlayerState) come from a live PlayerState's class.
    g_offOwner = g_offPlayerName = -1;
    for (uintptr_t gs : d.gameStates) {
        if (g_offPlayerArray < 0) break;
        uintptr_t arr = gs + (uintptr_t)g_offPlayerArray;
        uintptr_t data = rd<uintptr_t>(arr);
        int32_t num = rd<int32_t>(arr + 8);
        for (int32_t i = 0; data && i < num && i < kMaxPlayers; i++) {
            uintptr_t ps = rd<uintptr_t>(data + 8 * (uintptr_t)i);
            if (!Alive(ps)) continue;
            uintptr_t psClass = rd<uintptr_t>(ps + kObjClass);
            g_offOwner = PropertyOffset(psClass, N_Owner, N_ObjectProperty, 8);
            g_offPlayerName = PropertyOffset(psClass, N_PlayerNamePrivate, N_StrProperty, 0x10);
            break;
        }
    }
    return g_offPlayerArray >= 0 && g_offUserId >= 0;
}
}  // namespace

void SetGlobals(uintptr_t objObjects, uintptr_t nameBlocks, ProcessEventFn callProcessEvent) {
    kGObjObjects = objObjects;
    kNameBlocks = nameBlocks;
    g_callProcessEvent = callProcessEvent;
}

bool HaveGlobals() { return kGObjObjects && kNameBlocks && g_callProcessEvent; }

void CallProcessEvent(void* obj, void* func, void* parms) { g_callProcessEvent(obj, func, parms); }

bool ResolveNames() {
    if (!HaveGlobals()) return false;
    if (!g_namesReady) {
        const uint64_t t0 = NowNs();
        g_namesReady = FindNames();
        NativeLog("names: %s in %.2f ms", g_namesReady ? "resolved" : "not all present yet", (NowNs() - t0) / 1e6);
    }
    return g_namesReady;
}

bool DiscoverStep() {
    if (!HaveGlobals() || !g_namesReady) {
        St().error = "names not resolved";
        return false;
    }
    const uint64_t t0 = NowNs();
    uintptr_t objects = rd<uintptr_t>(kGObjObjects);
    int32_t num = rd<int32_t>(kGObjObjects + 8);
    Discovery& d = St().scan;
    std::string& error = St().error;
    int32_t end = d.next + kStepItems < num ? d.next + kStepItems : num;
    uintptr_t chunk = objects ? rd<uintptr_t>(objects + 8 * (uintptr_t)(d.next / kChunkItems)) : 0;
    for (int32_t i = d.next; chunk && i < end; i++) {
        uintptr_t item = chunk + (uintptr_t)(i % kChunkItems) * kItemStride;
        uintptr_t o = rd<uintptr_t>(item + kItemObject);
        if (!o || (rd<uint32_t>(item + kItemFlags) & kInternalDead)) continue;
        uintptr_t cls = rd<uintptr_t>(o + kObjClass);
        if (!cls) continue;
        if (!d.chatFunc && NameIs(o + kObjName, N_ClientReceiveChatMessage) && NameIs(cls + kObjName, N_Function)) {
            uintptr_t outer = rd<uintptr_t>(o + kObjOuter);
            if (outer && NameIs(outer + kObjName, N_ConanPlayerController)) {
                d.chatFunc = o;
                d.pcClass = outer;
            }
        }
        if (!d.gsClass && NameIs(o + kObjName, N_GameStateBase) && NameIs(cls + kObjName, N_Class)) d.gsClass = o;
        if (d.gsClass && !(rd<uint32_t>(o + kObjFlags) & kRfSkip)) {
            auto it = d.gsClassCache.find(cls);
            if (it == d.gsClassCache.end()) it = d.gsClassCache.emplace(cls, IsA(o, d.gsClass)).first;
            if (it->second) d.gameStates.push_back(o);
        }
    }
    d.next = end;
    d.scanNs += NowNs() - t0;
    if (d.next < num) return false;

    // Pass complete: publish it, then start the next pass from scratch.
    Discovery done = std::move(St().scan);
    St().scan = Discovery();
    if (!done.chatFunc) error = "ClientReceiveChatMessage not found";
    else if (!done.gsClass) error = "GameStateBase class not found";
    else if (done.gameStates.empty()) error = "no live GameState (world not loaded yet)";
    else if (!ResolveOffsets(done)) error = "PlayerArray/UserIDFromURLOptions property not found";
    else error.clear();
    NativeLog("discovery: %d objects in %.2f ms total, chatFunc=%s gameStates=%zu playerArray=%d userId=%d "
              "owner=%d playerName=%d%s%s",
              num, done.scanNs / 1e6, done.chatFunc ? "yes" : "no", done.gameStates.size(), g_offPlayerArray,
              g_offUserId, g_offOwner, g_offPlayerName, error.empty() ? "" : " error=", error.c_str());
    done.gsClassCache.clear();
    St().ready = std::move(done);
    g_haveReady = error.empty();
    return true;
}

bool Ready() {
    if (!HaveGlobals()) return false;
    const Discovery& r = St().ready;
    if (!g_haveReady || !Alive(r.chatFunc)) return false;
    for (uintptr_t gs : r.gameStates)
        if (Alive(gs)) return true;
    return false;
}

void ResetDiscovery() {
    g_haveReady = false;
    St().scan = Discovery();
}

std::string DiscoveryError() { return St().error; }

void* ChatFunction() { return (void*)St().ready.chatFunc; }

std::vector<Controller> OnlineControllers() {
    std::vector<Controller> out;
    const Discovery& r = St().ready;
    for (uintptr_t gs : r.gameStates) {
        if (!Alive(gs)) continue;
        uintptr_t arr = gs + (uintptr_t)g_offPlayerArray;
        uintptr_t data = rd<uintptr_t>(arr);
        int32_t num = rd<int32_t>(arr + 8);
        if (!data || num < 0 || num > kMaxPlayers) continue;
        for (int32_t i = 0; i < num; i++) {
            uintptr_t ps = rd<uintptr_t>(data + 8 * (uintptr_t)i);
            if (!Alive(ps)) continue;
            if (g_offOwner < 0) {
                g_offOwner = PropertyOffset(rd<uintptr_t>(ps + kObjClass), N_Owner, N_ObjectProperty, 8);
                g_offPlayerName =
                    PropertyOffset(rd<uintptr_t>(ps + kObjClass), N_PlayerNamePrivate, N_StrProperty, 0x10);
                if (g_offOwner < 0) continue;
            }
            uintptr_t pc = rd<uintptr_t>(ps + (uintptr_t)g_offOwner);
            if (!Alive(pc) || !IsA(pc, r.pcClass)) continue;
            bool dup = false;
            for (auto& c : out) dup = dup || c.object == pc;
            if (dup) continue;
            Controller c;
            c.object = pc;
            c.userId = ReadFString(pc + (uintptr_t)g_offUserId);
            if (g_offPlayerName >= 0) c.playerName = ReadFString(ps + (uintptr_t)g_offPlayerName);
            out.push_back(std::move(c));
        }
    }
    return out;
}

}  // namespace UE
