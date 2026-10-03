// Memory, names, objects and reflection for the probe. Offsets are the ones proven on build
// 25639945 (El-Limon context/games/conan-exiles/HANDOFF-native-stage2.md); the ones first
// used here (UStruct::Children, UFunction fields, UEnum::Names, FProperty type pointers) are
// listed in the dump's "layout" block and sanity-checked by the dump.
#include "probe.h"

#include "proto.h"

#include <fcntl.h>
#include <strings.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace Probe {

// ---------------------------------------------------------------- memory
namespace Mem {
namespace {
bool g_safe = false;
pid_t g_pid = 0;
thread_local int t_pipe[2] = {-1, -1};

bool PipeRead(uintptr_t addr, void* out, size_t n) {
    if (t_pipe[0] < 0 && pipe2(t_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    uint8_t* dst = (uint8_t*)out;
    while (n) {
        size_t c = n > 32768 ? 32768 : n;
        ssize_t w = write(t_pipe[1], (const void*)addr, c);
        if (w <= 0) return false;
        ssize_t r = read(t_pipe[0], dst, (size_t)w);
        if (r != w) return false;
        addr += (size_t)w;
        dst += w;
        n -= (size_t)w;
    }
    return true;
}
}  // namespace

void Init() {
    g_pid = getpid();
    volatile uint64_t probe = 0x1122334455667788ULL;
    uint64_t got = 0;
    iovec l{&got, 8}, r{(void*)&probe, 8};
    g_safe = process_vm_readv(g_pid, &l, 1, &r, 1, 0) == 8 && got == probe;
}
bool Safe() { return g_safe; }

bool Read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000 || addr > 0x7fffffffffffULL || n == 0) return false;
    if (g_safe) {
        iovec l{out, n}, r{(void*)addr, n};
        return process_vm_readv(g_pid, &l, 1, &r, 1, 0) == (ssize_t)n;
    }
    return PipeRead(addr, out, n);
}
}  // namespace Mem

using Mem::Rd;

// ---------------------------------------------------------------- names
namespace {
constexpr uintptr_t kNameBlocks = 0xc2a5d40;
std::mutex g_nameLock;
std::unordered_map<uint32_t, std::string>& NameCache() {
    static auto* m = new std::unordered_map<uint32_t, std::string>;
    return *m;
}
std::unordered_map<std::string, int64_t>& NameLookupCache() {
    static auto* m = new std::unordered_map<std::string, int64_t>;
    return *m;
}
std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Walks the name pool; f(index, name) returns false to stop.
template <typename F>
void ForEachName(F f) {
    uint32_t cur = Rd<uint32_t>(kNameBlocks - 8), cursor = Rd<uint32_t>(kNameBlocks - 4);
    if (cur > 8192) return;
    std::vector<uint8_t> buf(0x20000);
    for (uint32_t b = 0; b <= cur; ++b) {
        uintptr_t base = Rd<uintptr_t>(kNameBlocks + 8 * b);
        uint32_t size = b < cur ? 0x20000 : cursor;
        if (!base || size > 0x20000 || !Mem::Read(base, buf.data(), size)) continue;
        for (uint32_t off = 0; off + 2 <= size;) {
            uint16_t h;
            memcpy(&h, buf.data() + off, 2);
            unsigned len = h >> 6;
            if (!len) break;
            bool wide = h & 1;
            size_t bytes = len * (wide ? 2 : 1);
            if (off + 2 + bytes > size) break;
            std::string s;
            if (wide) {
                std::u16string w(len, u'\0');
                memcpy(&w[0], buf.data() + off + 2, bytes);
                s = Utf16To8(w);
            } else {
                s.assign((const char*)buf.data() + off + 2, len);
            }
            if (!f((b << 16) | (off / 2), s)) return;
            off += 2 + (uint32_t)bytes;
            off += off & 1;
        }
    }
}
}  // namespace

std::string NameStr(uint32_t idx) {
    {
        std::lock_guard<std::mutex> g(g_nameLock);
        auto it = NameCache().find(idx);
        if (it != NameCache().end()) return it->second;
    }
    uintptr_t blk = Rd<uintptr_t>(kNameBlocks + 8 * (idx >> 16));
    if (!blk) return "?";
    uintptr_t e = blk + (uintptr_t)(idx & 0xffff) * 2;
    uint16_t h = Rd<uint16_t>(e);
    unsigned len = h >> 6;
    if (!len || len > 1024) return "?";
    std::string s;
    if (h & 1) {
        std::u16string w(len, u'\0');
        if (!Mem::Read(e + 2, &w[0], len * 2)) return "?";
        s = Utf16To8(w);
    } else {
        s.resize(len);
        if (!Mem::Read(e + 2, &s[0], len)) return "?";
    }
    std::lock_guard<std::mutex> g(g_nameLock);
    if (NameCache().size() < 4000000) NameCache()[idx] = s;
    return s;
}

std::string FNameAt(uintptr_t at) {
    uint32_t v[2];
    if (!Mem::Read(at, v, 8)) return "?";
    std::string s = NameStr(v[0]);
    if (v[1]) s += "_" + std::to_string(v[1] - 1);
    return s;
}

int64_t FindName(const std::string& name) {
    const std::string key = Lower(name);
    {
        std::lock_guard<std::mutex> g(g_nameLock);
        auto it = NameLookupCache().find(key);
        if (it != NameLookupCache().end() && it->second >= 0) return it->second;
    }
    int64_t found = -1;
    ForEachName([&](uint32_t idx, const std::string& s) {
        if (s.size() == key.size() && strcasecmp(s.c_str(), key.c_str()) == 0) {
            found = idx;
            return false;
        }
        return true;
    });
    std::lock_guard<std::mutex> g(g_nameLock);
    NameLookupCache()[key] = found;
    return found;
}

std::vector<std::string> SearchNames(const std::string& sub, size_t limit) {
    std::vector<std::string> out;
    const std::string l = Lower(sub);
    ForEachName([&](uint32_t, const std::string& s) {
        if (Lower(s).find(l) != std::string::npos) out.push_back(s);
        return out.size() < limit;
    });
    return out;
}

// ---------------------------------------------------------------- objects
namespace {
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50, kStructSize = 0x58;
constexpr size_t kFieldNextUField = 0x28;  // UField::Next
constexpr size_t kFClass = 0x08, kFNext = 0x18, kFName = 0x20;
constexpr size_t kFClassName = 0x08;
constexpr size_t kFuncFlags = 0xB0, kFuncNumParms = 0xB4, kFuncParmsSize = 0xB6, kFuncRetOffset = 0xB8,
                 kFuncNative = 0xD8;
constexpr size_t kEnumCppType = 0x30;
// This build stores enum entries as two parallel arrays (low pointer bit set): FName[] at +0x40,
// int64[] at +0x48, count at +0x50. Proven on ENetRole (5 entries).
constexpr size_t kEnumNamesPtr = 0x40, kEnumValuesPtr = 0x48, kEnumNum = 0x50;
constexpr uint32_t kInternalDead = 0x10000000 | 0x20000000;
constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;
}  // namespace

std::string Hex(uintptr_t v) {
    char b[24];
    snprintf(b, sizeof b, "0x%lx", (unsigned long)v);
    return b;
}
uintptr_t ParseAddr(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    unsigned long long v = strtoull(s.c_str(), &end, 0);
    return end && *end == 0 ? (uintptr_t)v : 0;
}

bool ReadObj(uintptr_t a, Obj& o) {
    uint8_t b[0x28];
    if (!Mem::Read(a, b, sizeof b)) return false;
    o.addr = a;
    memcpy(&o.flags, b + kObjFlags, 4);
    memcpy(&o.index, b + kObjIndex, 4);
    memcpy(&o.cls, b + kObjClass, 8);
    memcpy(&o.nameIdx, b + kObjName, 4);
    memcpy(&o.nameNum, b + kObjName + 4, 4);
    memcpy(&o.outer, b + kObjOuter, 8);
    return true;
}

int32_t NumObjects() { return Rd<int32_t>(detail::kGObjObjects + 8); }

uintptr_t ObjectAtIndex(int32_t i) {
    using namespace detail;
    uintptr_t objects = Rd<uintptr_t>(kGObjObjects);
    if (!objects || i < 0 || i >= NumObjects()) return 0;
    uintptr_t chunk = Rd<uintptr_t>(objects + 8 * (uintptr_t)(i / kChunkItems));
    if (!chunk) return 0;
    uintptr_t item = chunk + (uintptr_t)(i % kChunkItems) * kItemStride;
    if (Rd<uint32_t>(item + 4) & kInternalDead) return 0;
    return Rd<uintptr_t>(item + 8);
}

bool Alive(uintptr_t obj) {
    Obj o;
    if (!ReadObj(obj, o)) return false;
    if (ObjectAtIndex(o.index) != obj) return false;
    return !(o.flags & (0x8000 | 0x10000));
}

std::string ObjName(uintptr_t obj) {
    if (!obj) return "null";
    return FNameAt(obj + kObjName);
}

std::string ObjPath(uintptr_t obj) {
    std::vector<std::string> parts;
    for (uintptr_t o = obj; o && parts.size() < 32; o = Rd<uintptr_t>(o + kObjOuter)) parts.push_back(ObjName(o));
    std::string s;
    for (size_t i = parts.size(); i-- > 0;) {
        s += parts[i];
        if (i) s += '.';
    }
    return s;
}

std::string ClassNameOf(uintptr_t obj) { return ObjName(Rd<uintptr_t>(obj + kObjClass)); }
uintptr_t SuperOf(uintptr_t st) { return Rd<uintptr_t>(st + kStructSuper); }

bool IsA(uintptr_t cls, uintptr_t base) {
    int d = 0;
    for (uintptr_t c = cls; c && d < 64; c = SuperOf(c), d++)
        if (c == base) return true;
    return false;
}

// ---------------------------------------------------------------- core meta + type index
namespace {
std::mutex g_metaLock;
Meta g_meta;
bool g_metaOk = false;
std::unordered_map<std::string, std::vector<uintptr_t>>& TypeIndex() {  // lower name -> type objects
    static auto* m = new std::unordered_map<std::string, std::vector<uintptr_t>>;
    return *m;
}
std::unordered_map<uintptr_t, uintptr_t>& CdoIndex() {
    static auto* m = new std::unordered_map<uintptr_t, uintptr_t>;
    return *m;
}
uint64_t g_indexMs = 0;

void BuildIndexLocked() {
    TypeIndex().clear();
    CdoIndex().clear();
    std::unordered_map<uintptr_t, int> kindCache;  // class -> 1 type object, 0 not
    ForEachObject([&](uintptr_t o, uint32_t fl) {
        if (fl & kInternalDead) return;
        Obj ob;
        if (!ReadObj(o, ob) || !ob.cls) return;
        if (ob.flags & 0x10) CdoIndex()[ob.cls] = o;  // RF_ClassDefaultObject
        auto it = kindCache.find(ob.cls);
        if (it == kindCache.end())
            it = kindCache.emplace(ob.cls, (IsA(ob.cls, g_meta.Struct) && !IsA(ob.cls, g_meta.Function)) ||
                                               IsA(ob.cls, g_meta.Enum)).first;
        if (it->second) TypeIndex()[Lower(NameStr(ob.nameIdx))].push_back(o);
    });
    g_indexMs = NowMs();
}
}  // namespace

const Meta& Core() { return g_meta; }

bool EnsureCore(std::string& err) {
    std::lock_guard<std::mutex> g(g_metaLock);
    if (g_metaOk) return true;
    Meta m;
    ForEachObject([&](uintptr_t o, uint32_t fl) {
        if (fl & kInternalDead) return;
        Obj ob;
        if (!ReadObj(o, ob)) return;
        std::string n = NameStr(ob.nameIdx);
        if (n != "Class" && n != "Struct" && n != "ScriptStruct" && n != "Enum" && n != "Function") return;
        if (ObjName(ob.outer) != "/Script/CoreUObject") return;
        if (n == "Class") m.Class = o;
        else if (n == "Struct") m.Struct = o;
        else if (n == "ScriptStruct") m.ScriptStruct = o;
        else if (n == "Enum") m.Enum = o;
        else m.Function = o;
    });
    if (!m.Class || !m.Struct || !m.ScriptStruct || !m.Enum || !m.Function) {
        err = "core meta classes not found (engine not initialised yet?)";
        return false;
    }
    g_meta = m;
    BuildIndexLocked();
    g_metaOk = true;
    return true;
}

static uintptr_t FindTypeLocked(const std::string& name) {
    auto it = TypeIndex().find(Lower(name));
    if (it == TypeIndex().end()) return 0;
    uintptr_t best = 0;
    int bestRank = 9;
    for (uintptr_t t : it->second) {
        uintptr_t mc = Rd<uintptr_t>(t + kObjClass);
        int rank = IsA(mc, g_meta.Class) ? 0 : IsA(mc, g_meta.ScriptStruct) ? 1 : 2;
        if (rank < bestRank && Alive(t)) best = t, bestRank = rank;
    }
    return best;
}

uintptr_t FindType(const std::string& name) {
    std::string err;
    if (!EnsureCore(err)) return 0;
    std::lock_guard<std::mutex> g(g_metaLock);
    uintptr_t t = FindTypeLocked(name);
    if (!t && NowMs() - g_indexMs > 5000) {  // newly loaded Blueprint class: rebuild once
        BuildIndexLocked();
        t = FindTypeLocked(name);
    }
    return t;
}

uintptr_t FindCDO(uintptr_t cls) {
    std::string err;
    if (!EnsureCore(err)) return 0;
    std::lock_guard<std::mutex> g(g_metaLock);
    auto it = CdoIndex().find(cls);
    if (it != CdoIndex().end() && Alive(it->second)) return it->second;
    BuildIndexLocked();
    it = CdoIndex().find(cls);
    return it == CdoIndex().end() ? 0 : it->second;
}

// ---------------------------------------------------------------- properties
// FArrayProperty: ArrayFlags +0x70, Inner +0x78 (proven on GameStateBase.PlayerArray).
// FSetProperty: ElementProp +0x70.
uintptr_t InnerOf(const Prop& p) { return p.type == "ArrayProperty" ? p.p78 : p.p70; }

bool ReadProp(uintptr_t p, Prop& out) {
    if (!p) return false;
    uint8_t b[0x80];
    if (!Mem::Read(p, b, sizeof b)) return false;
    uintptr_t fc;
    memcpy(&fc, b + kFClass, 8);
    out.addr = p;
    out.name = FNameAt(p + kFName);
    out.type = fc ? FNameAt(fc + kFClassName) : "?";
    memcpy(&out.dim, b + 0x2C, 4);
    memcpy(&out.size, b + 0x30, 4);
    memcpy(&out.flags, b + 0x38, 8);
    memcpy(&out.offset, b + 0x44, 4);
    memcpy(&out.p70, b + 0x70, 8);
    memcpy(&out.p78, b + 0x78, 8);
    memcpy(out.b, b + 0x70, 4);
    return true;
}

std::vector<Prop> PropsOf(uintptr_t st, bool withSupers) {
    std::vector<Prop> out;
    int d = 0;
    for (uintptr_t c = st; c && d < 64; c = withSupers ? SuperOf(c) : 0, d++) {
        int n = 0;
        for (uintptr_t p = Rd<uintptr_t>(c + kStructChildProps); p && n < 4096; p = Rd<uintptr_t>(p + kFNext), n++) {
            Prop pr;
            if (ReadProp(p, pr)) out.push_back(std::move(pr));
        }
        if (!withSupers) break;
    }
    return out;
}

bool FindProp(uintptr_t st, const std::string& name, Prop& out) {
    for (auto& p : PropsOf(st, true))
        if (strcasecmp(p.name.c_str(), name.c_str()) == 0) {
            out = p;
            return true;
        }
    return false;
}

bool ReadFunc(uintptr_t f, FuncInfo& out) {
    uint8_t b[0xE0];
    if (!Mem::Read(f, b, sizeof b)) return false;
    out.addr = f;
    out.name = ObjName(f);
    out.owner = ObjName(Rd<uintptr_t>(f + kObjOuter));
    memcpy(&out.flags, b + kFuncFlags, 4);
    out.numParms = b[kFuncNumParms];
    memcpy(&out.parmsSize, b + kFuncParmsSize, 2);
    memcpy(&out.retOffset, b + kFuncRetOffset, 2);
    memcpy(&out.native, b + kFuncNative, 8);
    out.params.clear();
    for (auto& p : PropsOf(f, false))
        if (p.flags & CPF_Parm) out.params.push_back(p);
    return true;
}

std::string FuncFlagNames(uint32_t f) {
    static const std::pair<uint32_t, const char*> k[] = {
        {0x1, "Final"}, {0x2, "RequiredAPI"}, {0x4, "BlueprintAuthorityOnly"}, {0x8, "BlueprintCosmetic"},
        {0x40, "Net"}, {0x80, "NetReliable"}, {0x100, "NetRequest"}, {0x200, "Exec"}, {0x400, "Native"},
        {0x800, "Event"}, {0x1000, "NetResponse"}, {0x2000, "Static"}, {0x4000, "NetMulticast"},
        {0x8000, "UbergraphFunction"}, {0x10000, "MulticastDelegate"}, {0x20000, "Public"},
        {0x40000, "Private"}, {0x80000, "Protected"}, {0x100000, "Delegate"}, {0x200000, "NetServer"},
        {0x400000, "HasOutParms"}, {0x800000, "HasDefaults"}, {0x1000000, "NetClient"}, {0x2000000, "DLLImport"},
        {0x4000000, "BlueprintCallable"}, {0x8000000, "BlueprintEvent"}, {0x10000000, "BlueprintPure"},
        {0x20000000, "EditorOnly"}, {0x40000000, "Const"}, {0x80000000, "NetValidate"}};
    std::string s;
    for (auto& e : k)
        if (f & e.first) s += (s.empty() ? "" : "|") + std::string(e.second);
    return s;
}

std::string ParamFlagNames(uint64_t f) {
    std::string s;
    auto add = [&](uint64_t bit, const char* n) {
        if (f & bit) s += (s.empty() ? "" : "|") + std::string(n);
    };
    add(0x2, "Const");
    add(CPF_OutParm, "Out");
    add(CPF_ReturnParm, "Return");
    add(0x8000000, "Ref");
    return s;
}

uintptr_t FindFunction(uintptr_t cls, const std::string& spec) {
    std::string owner, name = spec;
    size_t colon = spec.find(':');
    if (colon != std::string::npos) owner = spec.substr(0, colon), name = spec.substr(colon + 1);
    int d = 0;
    for (uintptr_t c = cls; c && d < 64; c = SuperOf(c), d++) {
        if (!owner.empty() && strcasecmp(ObjName(c).c_str(), owner.c_str()) != 0) continue;
        int n = 0;
        for (uintptr_t f = Rd<uintptr_t>(c + kStructChildren); f && n < 8192; f = Rd<uintptr_t>(f + kFieldNextUField), n++) {
            if (strcasecmp(ObjName(f).c_str(), name.c_str()) == 0 && IsA(Rd<uintptr_t>(f + kObjClass), g_meta.Function))
                return f;
        }
    }
    return 0;
}

std::vector<std::pair<std::string, int64_t>> EnumEntries(uintptr_t en) {
    std::vector<std::pair<std::string, int64_t>> out;
    uintptr_t names = Rd<uintptr_t>(en + kEnumNamesPtr) & ~(uintptr_t)7;
    uintptr_t values = Rd<uintptr_t>(en + kEnumValuesPtr) & ~(uintptr_t)7;
    int32_t num = Rd<int32_t>(en + kEnumNum);
    for (int32_t i = 0; names && values && i < num && i < 4096; i++)
        out.push_back({FNameAt(names + 8 * (uintptr_t)i), Rd<int64_t>(values + 8 * (uintptr_t)i)});
    return out;
}

// ---------------------------------------------------------------- writers
void JW::dbl(double v) {
    pre();
    if (!std::isfinite(v)) {
        s += "null";
        return;
    }
    char b[40];
    snprintf(b, sizeof b, "%.9g", v);
    s += b;
}

void WriteTypeExtra(JW& w, const Prop& p, int depth) {
    const std::string& t = p.type;
    if (t == "StructProperty") w.kstr("struct", ObjName(p.p70));
    else if (t == "ObjectProperty" || t == "WeakObjectProperty" || t == "LazyObjectProperty" ||
             t == "SoftObjectProperty" || t == "InterfaceProperty" || t == "ObjectPtrProperty")
        w.kstr("class", ObjName(p.p70));
    else if (t == "ClassProperty" || t == "SoftClassProperty" || t == "ClassPtrProperty") {
        w.kstr("class", ObjName(p.p70));
        w.kstr("metaClass", ObjName(p.p78));
    } else if (t == "ByteProperty") {
        if (p.p70) w.kstr("enum", ObjName(p.p70));
    } else if (t == "EnumProperty") {
        w.kstr("enum", ObjName(p.p78));
        Prop u;
        if (ReadProp(p.p70, u)) w.kstr("underlying", u.type);
    } else if (t == "BoolProperty") {
        w.knum("fieldSize", p.b[0]);
        w.knum("byteOffset", p.b[1]);
        w.knum("byteMask", p.b[2]);
        w.knum("fieldMask", p.b[3]);
    } else if ((t == "ArrayProperty" || t == "SetProperty") && depth < 3) {
        Prop in;
        if (ReadProp(InnerOf(p), in)) {
            w.key("inner");
            w.beginObj();
            w.kstr("type", in.type);
            w.knum("size", in.size);
            WriteTypeExtra(w, in, depth + 1);
            w.endObj();
        }
    } else if (t == "MapProperty" && depth < 3) {
        Prop k, v;
        if (ReadProp(p.p70, k) && ReadProp(p.p78, v)) {
            w.key("key");
            w.beginObj();
            w.kstr("type", k.type);
            w.knum("size", k.size);
            WriteTypeExtra(w, k, depth + 1);
            w.endObj();
            w.key("value");
            w.beginObj();
            w.kstr("type", v.type);
            w.knum("size", v.size);
            WriteTypeExtra(w, v, depth + 1);
            w.endObj();
        }
    } else if (t == "DelegateProperty" || t == "MulticastDelegateProperty" ||
               t == "MulticastInlineDelegateProperty" || t == "MulticastSparseDelegateProperty") {
        if (p.p70) w.kstr("signature", ObjPath(p.p70));
    }
}

void WriteProp(JW& w, const Prop& p) {
    w.beginObj();
    w.kstr("name", p.name);
    w.kstr("type", p.type);
    w.knum("offset", p.offset);
    w.knum("size", p.size);
    if (p.dim != 1) w.knum("dim", p.dim);
    w.kstr("flags", Hex(p.flags));
    std::string pf = ParamFlagNames(p.flags);
    if ((p.flags & CPF_Parm) && !pf.empty()) w.kstr("param", pf);
    WriteTypeExtra(w, p, 0);
    w.endObj();
}

void WriteFunc(JW& w, const FuncInfo& f) {
    w.beginObj();
    w.kstr("name", f.name);
    w.kstr("flags", Hex(f.flags));
    w.kstr("flagNames", FuncFlagNames(f.flags));
    w.knum("parmsSize", f.parmsSize);
    w.knum("numParms", f.numParms);
    if (f.retOffset != 0xffff) w.knum("returnOffset", f.retOffset);
    if (f.native) w.kstr("func", Hex(f.native));
    w.key("params");
    w.beginArr();
    for (auto& p : f.params) WriteProp(w, p);
    w.endArr();
    w.endObj();
}

void WriteStruct(JW& w, uintptr_t st, long long instances, uintptr_t cdo) {
    w.beginObj();
    w.kstr("name", ObjName(st));
    w.kstr("kind", ClassNameOf(st));
    w.kstr("path", ObjPath(st));
    w.kstr("addr", Hex(st));
    uintptr_t sup = SuperOf(st);
    w.kstr("super", sup ? ObjName(sup) : "");
    w.key("superChain");
    w.beginArr();
    int d = 0;
    for (uintptr_t c = sup; c && d < 64; c = SuperOf(c), d++) w.str(ObjName(c));
    w.endArr();
    w.knum("size", Rd<int32_t>(st + kStructSize));
    if (instances >= 0) w.knum("instances", instances);
    if (cdo) w.kstr("cdo", Hex(cdo));
    w.key("properties");
    w.beginArr();
    for (auto& p : PropsOf(st, false)) WriteProp(w, p);
    w.endArr();
    w.key("functions");
    w.beginArr();
    int n = 0;
    for (uintptr_t f = Rd<uintptr_t>(st + kStructChildren); f && n < 8192; f = Rd<uintptr_t>(f + kFieldNextUField), n++) {
        if (!IsA(Rd<uintptr_t>(f + kObjClass), g_meta.Function)) continue;
        FuncInfo fi;
        if (ReadFunc(f, fi)) WriteFunc(w, fi);
    }
    w.endArr();
    w.endObj();
}

void WriteEnum(JW& w, uintptr_t en) {
    w.beginObj();
    w.kstr("name", ObjName(en));
    w.kstr("kind", ClassNameOf(en));
    w.kstr("path", ObjPath(en));
    {
        uintptr_t d = Rd<uintptr_t>(en + kEnumCppType);
        int32_t n = Rd<int32_t>(en + kEnumCppType + 8);
        std::string cpp;
        if (d && n > 1 && n < 512) {
            std::u16string s((size_t)n - 1, u'\0');
            if (Mem::Read(d, &s[0], ((size_t)n - 1) * 2)) cpp = Utf16To8(s);
        }
        w.kstr("cppType", cpp);
    }
    w.key("values");
    w.beginArr();
    for (auto& e : EnumEntries(en)) {
        w.beginArr();
        w.str(e.first);
        w.num(e.second);
        w.endArr();
    }
    w.endArr();
    w.endObj();
}

// ---------------------------------------------------------------- decode
namespace {
std::string ReadFStringAt(uintptr_t at, bool& ok) {
    uintptr_t d = Rd<uintptr_t>(at);
    int32_t n = Rd<int32_t>(at + 8);
    ok = true;
    if (!d || n <= 0) return "";
    if (n > 65536) {
        ok = false;
        return "";
    }
    std::u16string s((size_t)n, u'\0');
    if (!Mem::Read(d, &s[0], (size_t)n * 2)) {
        ok = false;
        return "";
    }
    while (!s.empty() && s.back() == 0) s.pop_back();
    return Utf16To8(s);
}
std::string HexBytes(uintptr_t at, int n) {
    if (n <= 0) return "";
    if (n > 256) n = 256;
    std::vector<uint8_t> b((size_t)n);
    if (!Mem::Read(at, b.data(), (size_t)n)) return "<unreadable>";
    std::string s;
    char x[4];
    for (auto c : b) snprintf(x, sizeof x, "%02x", c), s += x;
    return s;
}
std::string EnumNameOf(uintptr_t en, int64_t v) {
    for (auto& e : EnumEntries(en))
        if (e.second == v) return e.first;
    return "";
}
void WriteObjRef(JW& w, uintptr_t o) {
    if (!o) {
        w.null();
        return;
    }
    w.beginObj();
    w.kstr("addr", Hex(o));
    w.kstr("name", ObjName(o));
    w.kstr("class", ClassNameOf(o));
    w.endObj();
}
int64_t ReadInt(uintptr_t at, int size, bool sign) {
    switch (size) {
        case 1: return sign ? (int64_t)Rd<int8_t>(at) : (int64_t)Rd<uint8_t>(at);
        case 2: return sign ? (int64_t)Rd<int16_t>(at) : (int64_t)Rd<uint16_t>(at);
        case 4: return sign ? (int64_t)Rd<int32_t>(at) : (int64_t)Rd<uint32_t>(at);
        default: return Rd<int64_t>(at);
    }
}
}  // namespace

void DecodeValue(JW& w, const Prop& p, uintptr_t at, const DecodeOpts& o, int depth) {
    const std::string& t = p.type;
    if (p.dim > 1 && depth >= 0) {  // static array: decode each element
        w.beginArr();
        Prop one = p;
        one.dim = 1;
        for (int i = 0; i < p.dim && i < o.arrayLimit; i++) DecodeValue(w, one, at + (uintptr_t)i * p.size, o, depth);
        w.endArr();
        return;
    }
    if (t == "BoolProperty") w.boolean((Rd<uint8_t>(at + p.b[1]) & p.b[3]) != 0);
    else if (t == "Int8Property" || t == "Int16Property" || t == "IntProperty" || t == "Int64Property")
        w.num(ReadInt(at, p.size, true));
    else if (t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property")
        w.raw(std::to_string((uint64_t)ReadInt(at, p.size, false)));
    else if (t == "FloatProperty") w.dbl(Rd<float>(at));
    else if (t == "DoubleProperty") w.dbl(Rd<double>(at));
    else if (t == "ByteProperty") {
        int64_t v = Rd<uint8_t>(at);
        if (p.p70) {
            w.beginObj();
            w.knum("value", v);
            w.kstr("name", EnumNameOf(p.p70, v));
            w.endObj();
        } else w.num(v);
    } else if (t == "EnumProperty") {
        int64_t v = ReadInt(at, p.size, false);
        w.beginObj();
        w.knum("value", v);
        w.kstr("name", EnumNameOf(p.p78, v));
        w.endObj();
    } else if (t == "NameProperty") w.str(FNameAt(at));
    else if (t == "StrProperty") {
        bool ok;
        std::string s = ReadFStringAt(at, ok);
        if (ok) w.str(s);
        else w.str("<unreadable FString>");
    } else if (t == "ObjectProperty" || t == "ClassProperty" || t == "ObjectPtrProperty" || t == "ClassPtrProperty" ||
               t == "InterfaceProperty")
        WriteObjRef(w, Rd<uintptr_t>(at));
    else if (t == "WeakObjectProperty") {
        int32_t idx = Rd<int32_t>(at);
        uintptr_t obj = idx > 0 ? ObjectAtIndex(idx) : 0;
        WriteObjRef(w, obj);
    } else if (t == "TextProperty") {
        w.beginObj();
        w.kstr("textData", Hex(Rd<uintptr_t>(at)));
        w.kstr("hex", HexBytes(at, p.size));
        w.kstr("hint", "use /call KismetTextLibrary.Conv_TextToString with {\"InText\":{\"addr\":\"<this field addr>\"}}");
        w.kstr("fieldAddr", Hex(at));
        w.endObj();
    } else if (t == "StructProperty" && depth < o.depth) {
        w.beginObj();
        for (auto& f : PropsOf(p.p70, true)) {
            w.key(f.name.c_str());
            DecodeValue(w, f, at + (uintptr_t)f.offset, o, depth + 1);
        }
        w.endObj();
    } else if (t == "ArrayProperty") {
        uintptr_t data = Rd<uintptr_t>(at);
        int32_t num = Rd<int32_t>(at + 8);
        Prop in;
        w.beginObj();
        w.knum("num", num);
        if (data && num > 0 && ReadProp(InnerOf(p), in) && depth < o.depth) {
            w.key("items");
            w.beginArr();
            for (int32_t i = 0; i < num && i < o.arrayLimit; i++) DecodeValue(w, in, data + (uintptr_t)i * in.size, o, depth + 1);
            w.endArr();
        }
        w.endObj();
    } else if (t == "MapProperty" || t == "SetProperty") {
        w.beginObj();
        w.knum("sparseNum", Rd<int32_t>(at + 8));
        w.kstr("hex", HexBytes(at, p.size));
        w.endObj();
    } else {
        w.beginObj();
        w.kstr("type", t);
        w.kstr("hex", HexBytes(at, p.size));
        w.endObj();
    }
}

// ---------------------------------------------------------------- encode
namespace {
bool ParseHexBytes(const std::string& h, std::vector<uint8_t>& out) {
    if (h.size() % 2) return false;
    for (size_t i = 0; i < h.size(); i += 2) {
        unsigned v;
        if (sscanf(h.c_str() + i, "%2x", &v) != 1) return false;
        out.push_back((uint8_t)v);
    }
    return true;
}
std::u16string Utf8To16(const std::string& s) {
    std::u16string o;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n > 1) c &= (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            o += (char16_t)(0xD800 + (c >> 10));
            o += (char16_t)(0xDC00 + (c & 0x3FF));
        } else o += (char16_t)c;
    }
    return o;
}
int64_t EnumValueOf(uintptr_t en, const std::string& name, bool& ok) {
    for (auto& e : EnumEntries(en)) {
        const std::string& n = e.first;
        size_t c = n.rfind("::");
        std::string shortN = c == std::string::npos ? n : n.substr(c + 2);
        if (strcasecmp(n.c_str(), name.c_str()) == 0 || strcasecmp(shortN.c_str(), name.c_str()) == 0) {
            ok = true;
            return e.second;
        }
    }
    ok = false;
    return 0;
}
void PutInt(uint8_t* dst, int size, int64_t v) { memcpy(dst, &v, size > 8 ? 8 : (size_t)size); }
}  // namespace

bool EncodeValue(const Prop& p, const JsonValue& v, uint8_t* dst, Arena& arena, std::string& err) {
    const std::string& t = p.type;
    // Raw forms work for every type.
    if (v.type == JsonValue::Object) {
        if (const JsonValue* h = v.get("hex")) {
            std::vector<uint8_t> b;
            if (!h->isStr() || !ParseHexBytes(h->str, b) || (int)b.size() > p.size * p.dim) {
                err = p.name + ": bad hex";
                return false;
            }
            memcpy(dst, b.data(), b.size());
            return true;
        }
        if (const JsonValue* a = v.get("addr")) {
            uintptr_t src = a->isStr() ? ParseAddr(a->str) : 0;
            if (!src || !Mem::Read(src, dst, (size_t)p.size)) {
                err = p.name + ": cannot copy from addr";
                return false;
            }
            return true;
        }
    }
    if (v.type == JsonValue::Null) return true;  // stays zeroed
    if (t == "BoolProperty") {
        if (v.type != JsonValue::Bool && v.type != JsonValue::Number) {
            err = p.name + ": expected bool";
            return false;
        }
        bool b = v.type == JsonValue::Bool ? v.b : v.num != 0;
        uint8_t* byte = dst + p.b[1];
        *byte = (uint8_t)((*byte & ~p.b[3]) | (b ? p.b[2] : 0));  // UE: clear FieldMask, set ByteMask
        return true;
    }
    if (t == "Int8Property" || t == "Int16Property" || t == "IntProperty" || t == "Int64Property" ||
        t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property") {
        if (v.type == JsonValue::Number) PutInt(dst, p.size, (int64_t)strtoll(v.str.c_str(), nullptr, 10));
        else if (v.isStr()) PutInt(dst, p.size, (int64_t)strtoull(v.str.c_str(), nullptr, 0));
        else {
            err = p.name + ": expected integer";
            return false;
        }
        return true;
    }
    if (t == "FloatProperty" || t == "DoubleProperty") {
        if (v.type != JsonValue::Number) {
            err = p.name + ": expected number";
            return false;
        }
        if (t == "FloatProperty") {
            float f = (float)v.num;
            memcpy(dst, &f, 4);
        } else memcpy(dst, &v.num, 8);
        return true;
    }
    if (t == "ByteProperty" || t == "EnumProperty") {
        uintptr_t en = t == "ByteProperty" ? p.p70 : p.p78;
        int64_t val = 0;
        if (v.type == JsonValue::Number) val = (int64_t)v.num;
        else if (v.isStr() && en) {
            bool ok;
            val = EnumValueOf(en, v.str, ok);
            if (!ok) {
                err = p.name + ": unknown enum value " + v.str;
                return false;
            }
        } else {
            err = p.name + ": expected number or enum name";
            return false;
        }
        PutInt(dst, p.size, val);
        return true;
    }
    if (t == "NameProperty") {
        if (!v.isStr()) {
            err = p.name + ": expected string";
            return false;
        }
        std::string base = v.str;
        uint32_t number = 0;
        int64_t idx = FindName(base);
        if (idx < 0) {  // "Foo_3" -> ("Foo", 4)
            size_t us = base.rfind('_');
            if (us != std::string::npos && us + 1 < base.size() &&
                base.find_first_not_of("0123456789", us + 1) == std::string::npos) {
                idx = FindName(base.substr(0, us));
                number = (uint32_t)strtoul(base.c_str() + us + 1, nullptr, 10) + 1;
            }
        }
        if (idx < 0) {
            err = p.name + ": FName '" + v.str + "' does not exist in the name pool";
            return false;
        }
        uint32_t fn[2] = {(uint32_t)idx, number};
        memcpy(dst, fn, 8);
        return true;
    }
    if (t == "StrProperty") {
        if (!v.isStr()) {
            err = p.name + ": expected string";
            return false;
        }
        std::u16string w = Utf8To16(v.str);
        uint8_t* buf = arena.alloc((w.size() + 1) * 2);
        memcpy(buf, w.data(), w.size() * 2);
        int32_t n = (int32_t)w.size() + 1;
        uintptr_t d = (uintptr_t)buf;
        memcpy(dst, &d, 8);
        memcpy(dst + 8, &n, 4);
        memcpy(dst + 12, &n, 4);
        return true;
    }
    if (t == "ObjectProperty" || t == "ClassProperty" || t == "ObjectPtrProperty" || t == "ClassPtrProperty" ||
        t == "InterfaceProperty" || t == "WeakObjectProperty") {
        if (!v.isStr()) {
            err = p.name + ": expected object spec string";
            return false;
        }
        std::string e;
        uintptr_t o = ResolveObjectSpec(v.str, e);
        if (!o) {
            err = p.name + ": " + e;
            return false;
        }
        if (t == "WeakObjectProperty") {
            Obj ob;
            ReadObj(o, ob);
            int32_t idx = ob.index;
            uintptr_t item = 0;
            {
                using namespace detail;
                uintptr_t objects = Rd<uintptr_t>(kGObjObjects);
                uintptr_t chunk = Rd<uintptr_t>(objects + 8 * (uintptr_t)(idx / kChunkItems));
                item = chunk + (uintptr_t)(idx % kChunkItems) * kItemStride;
            }
            int32_t serial = Rd<int32_t>(item + 0x10);
            memcpy(dst, &idx, 4);
            memcpy(dst + 4, &serial, 4);
        } else {
            memcpy(dst, &o, 8);
            if (t == "InterfaceProperty") memset(dst + 8, 0, 8);  // interface pointer left null
        }
        return true;
    }
    if (t == "StructProperty") {
        if (v.type != JsonValue::Object) {
            err = p.name + ": expected object with struct fields (or {hex}/{addr})";
            return false;
        }
        auto fields = PropsOf(p.p70, true);
        for (auto& kv : v.obj) {
            bool found = false;
            for (auto& f : fields)
                if (strcasecmp(f.name.c_str(), kv.first.c_str()) == 0) {
                    found = true;
                    if (!EncodeValue(f, kv.second, dst + f.offset, arena, err)) return false;
                }
            if (!found) {
                err = p.name + ": struct " + ObjName(p.p70) + " has no field " + kv.first;
                return false;
            }
        }
        return true;
    }
    if (t == "ArrayProperty") {
        if (v.type != JsonValue::Array) {
            err = p.name + ": expected array";
            return false;
        }
        Prop in;
        if (!ReadProp(InnerOf(p), in)) {
            err = p.name + ": unreadable inner";
            return false;
        }
        uint8_t* data = arena.alloc((size_t)in.size * (v.arr.size() + 1));
        for (size_t i = 0; i < v.arr.size(); i++)
            if (!EncodeValue(in, v.arr[i], data + i * (size_t)in.size, arena, err)) return false;
        uintptr_t d = v.arr.empty() ? 0 : (uintptr_t)data;
        int32_t n = (int32_t)v.arr.size();
        memcpy(dst, &d, 8);
        memcpy(dst + 8, &n, 4);
        memcpy(dst + 12, &n, 4);
        return true;
    }
    err = p.name + ": type " + t + " needs {\"hex\":...} or {\"addr\":...}";
    return false;
}

uintptr_t ResolveObjectSpec(const std::string& spec, std::string& err) {
    if (spec.rfind("0x", 0) == 0) {
        uintptr_t a = ParseAddr(spec);
        if (!a || !Alive(a)) {
            err = "object " + spec + " is not a live object";
            return 0;
        }
        return a;
    }
    if (!EnsureCore(err)) return 0;
    auto colon = spec.find(':');
    if (colon == std::string::npos) {
        err = "object spec must be 0x<addr>, cdo:<Class>, first:<Class> or name:<Object>";
        return 0;
    }
    std::string kind = spec.substr(0, colon), arg = spec.substr(colon + 1);
    if (kind == "cdo") {
        uintptr_t cls = FindType(arg);
        uintptr_t cdo = cls ? FindCDO(cls) : 0;
        if (!cdo) err = "no CDO for class " + arg;
        return cdo;
    }
    if (kind == "first" || kind == "name") {
        uintptr_t cls = kind == "first" ? FindType(arg) : 0;
        if (kind == "first" && !cls) {
            err = "class " + arg + " not found";
            return 0;
        }
        uintptr_t found = 0;
        ForEachObject([&](uintptr_t o, uint32_t fl) {
            if (found || (fl & kInternalDead)) return;
            Obj ob;
            if (!ReadObj(o, ob) || (ob.flags & (0x10 | 0x20 | 0x8000 | 0x10000))) return;
            if (kind == "first" ? IsA(ob.cls, cls) : strcasecmp(FNameAt(o + kObjName).c_str(), arg.c_str()) == 0)
                found = o;
        });
        if (!found) err = "no live object for " + spec;
        return found;
    }
    err = "unknown object spec kind " + kind;
    return 0;
}

}  // namespace Probe
