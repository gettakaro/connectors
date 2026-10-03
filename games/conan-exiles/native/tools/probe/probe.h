// Conan Exiles reflection probe (dev tool, never shipped). Linux server build 25639945 only.
//
// An LD_PRELOAD library that dumps the live UE reflection data to JSON and serves a loopback,
// token-protected HTTP REPL: find objects, read properties, call UFunctions on the game thread
// and trace ProcessEvent by function name. See README.md next to this file.
//
// Memory policy: every read made off the game thread goes through Mem::Read
// (process_vm_readv on our own pid), so a stale pointer returns an error instead of crashing
// the server. Only /call jobs and the drain itself run on the game thread.
#pragma once

#include <atomic>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace Probe {

constexpr const char* kVersion = "0.1.0";

// ---- memory ----
namespace Mem {
void Init();
bool Safe();  // false: process_vm_readv unavailable, the pipe fallback is used
bool Read(uintptr_t addr, void* out, size_t n);
template <typename T>
T Rd(uintptr_t a) {
    T v{};
    if (!Read(a, &v, sizeof v)) return T{};
    return v;
}
}  // namespace Mem

// ---- names ----
std::string NameStr(uint32_t comparisonIndex);
std::string FNameAt(uintptr_t fnameAddr);  // includes the _N number suffix
// Comparison index of an existing name (case-insensitive), or -1.
int64_t FindName(const std::string& name);
// Names containing `sub` (case-insensitive), up to `limit`.
std::vector<std::string> SearchNames(const std::string& sub, size_t limit);

// ---- objects (layout proven on 25639945) ----
struct Obj {
    uintptr_t addr = 0;
    uint32_t flags = 0;
    int32_t index = -1;
    uintptr_t cls = 0;
    uint32_t nameIdx = 0, nameNum = 0;
    uintptr_t outer = 0;
};
bool ReadObj(uintptr_t a, Obj& o);
int32_t NumObjects();
// Calls f(addr, itemFlags) for every non-null object slot. Off the game thread, safe reads.
template <typename F>
void ForEachObject(F f);
uintptr_t ObjectAtIndex(int32_t i);
bool Alive(uintptr_t obj);
std::string ObjName(uintptr_t obj);
std::string ObjPath(uintptr_t obj);  // outermost first, joined with '.'
std::string ClassNameOf(uintptr_t obj);
uintptr_t SuperOf(uintptr_t st);
bool IsA(uintptr_t cls, uintptr_t base);
std::string Hex(uintptr_t v);
uintptr_t ParseAddr(const std::string& s);  // "0x..." or decimal, 0 on failure

// Core meta classes (UClass, UStruct, UScriptStruct, UEnum, UFunction) found once.
struct Meta {
    uintptr_t Class = 0, Struct = 0, ScriptStruct = 0, Enum = 0, Function = 0;
};
const Meta& Core();
bool EnsureCore(std::string& err);
// A struct/class/enum object by name (case-insensitive). Prefers UClass, then ScriptStruct, Enum.
uintptr_t FindType(const std::string& name);
uintptr_t FindCDO(uintptr_t cls);

// ---- properties ----
struct Prop {
    uintptr_t addr = 0;
    std::string name, type;
    int32_t dim = 1, size = 0, offset = 0;
    uint64_t flags = 0;
    uintptr_t p70 = 0, p78 = 0;  // type-specific pointers
    uint8_t b[4] = {0, 0, 0, 0};  // FBoolProperty FieldSize, ByteOffset, ByteMask, FieldMask
};
bool ReadProp(uintptr_t p, Prop& out);
uintptr_t InnerOf(const Prop& p);
std::vector<std::pair<std::string, int64_t>> EnumEntries(uintptr_t en);
std::vector<Prop> PropsOf(uintptr_t st, bool withSupers);
bool FindProp(uintptr_t st, const std::string& name, Prop& out);

struct FuncInfo {
    uintptr_t addr = 0;
    std::string name, owner;
    uint32_t flags = 0;
    uint8_t numParms = 0;
    uint16_t parmsSize = 0, retOffset = 0;
    uintptr_t native = 0;
    std::vector<Prop> params;
};
bool ReadFunc(uintptr_t f, FuncInfo& out);
std::string FuncFlagNames(uint32_t flags);
std::string ParamFlagNames(uint64_t flags);
// Walks the class chain through UStruct::Children; "Class:Function" restricts the owner.
uintptr_t FindFunction(uintptr_t cls, const std::string& name);

// ---- JSON writing of reflection ----
struct JW {  // tiny streaming JSON writer
    std::string s;
    std::vector<bool> first{true};
    bool afterKey = false;
    void pre() {
        if (afterKey) { afterKey = false; return; }
        if (!first.back()) s += ',';
        first.back() = false;
    }
    void key(const char* k) { pre(); s += JsonStr(k); s += ':'; afterKey = true; }
    void beginObj() { pre(); s += '{'; first.push_back(true); }
    void endObj() { s += '}'; first.pop_back(); }
    void beginArr() { pre(); s += '['; first.push_back(true); }
    void endArr() { s += ']'; first.pop_back(); }
    void str(const std::string& v) { pre(); s += JsonStr(v); }
    void raw(const std::string& v) { pre(); s += v; }
    void num(long long v) { pre(); s += std::to_string(v); }
    void dbl(double v);
    void boolean(bool v) { pre(); s += v ? "true" : "false"; }
    void null() { pre(); s += "null"; }
    void kstr(const char* k, const std::string& v) { key(k); str(v); }
    void knum(const char* k, long long v) { key(k); num(v); }
    void kbool(const char* k, bool v) { key(k); boolean(v); }
};
void WriteTypeExtra(JW& w, const Prop& p, int depth);
void WriteProp(JW& w, const Prop& p);
void WriteFunc(JW& w, const FuncInfo& f);
void WriteStruct(JW& w, uintptr_t st, long long instances, uintptr_t cdo);
void WriteEnum(JW& w, uintptr_t en);

// ---- values ----
struct DecodeOpts {
    int depth = 2;
    int arrayLimit = 32;
};
void DecodeValue(JW& w, const Prop& p, uintptr_t at, const DecodeOpts& o, int depth);
struct Arena {
    std::vector<std::vector<uint8_t>> blocks;
    uint8_t* alloc(size_t n) {
        blocks.emplace_back(n + 16, 0);
        uintptr_t a = (uintptr_t)blocks.back().data();
        return (uint8_t*)((a + 15) & ~(uintptr_t)15);
    }
};
bool EncodeValue(const Prop& p, const JsonValue& v, uint8_t* dst, Arena& arena, std::string& err);
// Resolves "0x..", "cdo:<Class>", "first:<Class>", "name:<object name>".
uintptr_t ResolveObjectSpec(const std::string& spec, std::string& err);

// ---- hook / game thread ----
void CallProcessEvent(void* obj, void* func, void* parms);
bool OnGameThread();

// ---- trace ----
namespace Trace {
extern std::atomic<bool> g_on;
// Returns a non-zero cookie when the call matched a spec that wants post-call decoding.
uint32_t Pre(void* obj, void* func, void* parms);
void Post(uint32_t cookie, void* obj, void* func, void* parms);
std::string Add(const JsonValue& spec, std::string& err);
bool Remove(const std::string& id);
std::string List();
void StartWriter();
}  // namespace Trace

// ---- dump ----
namespace Dump {
std::string Start(const std::string& outPath, std::string& err);
std::string Status();
}  // namespace Dump

// ---- server ----
std::string ProbeDir();
bool StartServer(std::string& err);

}  // namespace Probe

// Template body (needs the object array layout).
namespace Probe {
namespace detail {
constexpr uintptr_t kGObjObjects = 0xc35a580;
constexpr int kChunkItems = 65536;
constexpr size_t kItemStride = 0x18;
}  // namespace detail

template <typename F>
void ForEachObject(F f) {
    using namespace detail;
    uintptr_t objects = Mem::Rd<uintptr_t>(kGObjObjects);
    int32_t num = Mem::Rd<int32_t>(kGObjObjects + 8);
    if (!objects || num <= 0 || num > 16 * 1024 * 1024) return;
    std::vector<uint8_t> buf(kChunkItems * kItemStride);
    for (int32_t base = 0; base < num; base += kChunkItems) {
        uintptr_t chunk = Mem::Rd<uintptr_t>(objects + 8 * (uintptr_t)(base / kChunkItems));
        if (!chunk) continue;
        int32_t n = num - base < kChunkItems ? num - base : kChunkItems;
        if (!Mem::Read(chunk, buf.data(), (size_t)n * kItemStride)) continue;
        for (int32_t i = 0; i < n; i++) {
            const uint8_t* it = buf.data() + (size_t)i * kItemStride;
            uintptr_t o;
            uint32_t fl;
            memcpy(&o, it + 8, 8);
            memcpy(&fl, it + 4, 4);
            if (o) f(o, fl);
        }
    }
}
}  // namespace Probe
