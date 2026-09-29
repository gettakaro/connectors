#include "reflect.h"

#include "state.h"
#include "resolve.h"

#include <cstring>
#include <atomic>

using namespace UE;

namespace {

Reflect::Layout g_lay;
bool g_ready = false;
std::atomic<bool> g_validated{false};

// --- resolved game functions -------------------------------------------------------------------
using FnFNameCtor = void (*)(FName*, const char16_t*, int /*EFindName*/);
using FnFNameToString = void (*)(const FName*, FString*);
using FnFindFunction = void* (*)(void* obj, FName name);
using FnFindProperty = void* (*)(void* strct, FName name);
using FnGetPathName = void (*)(FString* ret, void* obj, void* stopOuter);
using FnStaticFindObjectPath = void* (*)(void* cls, FTopLevelAssetPath path, bool exactClass);
using FnGetObjectsOfClass = void (*)(void* cls, TArray<void*>* out, bool derived, uint32_t exclFlags,
                                     uint32_t internalExcl);
using FnGetObjectsWithOuter = void (*)(void* outer, TArray<void*>* out, bool nested, uint32_t exclFlags,
                                       uint32_t internalExcl);
using FnFree = void (*)(void*);
using FnStaticClass = void* (*)();

FnFNameCtor g_nameCtor = nullptr;
FnFNameToString g_nameToString = nullptr;
FnFindFunction g_findFunction = nullptr;
FnFindProperty g_findProperty = nullptr;
FnGetPathName g_getPathName = nullptr;
FnStaticFindObjectPath g_findObjectPath = nullptr;
FnGetObjectsOfClass g_getObjectsOfClass = nullptr;
FnGetObjectsWithOuter g_getObjectsWithOuter = nullptr;
FnFree g_free = nullptr;

template <typename T>
T As(const char* sym) {
    return (T)(uintptr_t)Resolve::Addr(sym);
}

inline void* ReadPtr(void* base, uint32_t off) {
    if (!base) return nullptr;
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 8)) return nullptr;
    void* v = *(void* const*)p;
    return v;
}
inline uint32_t ReadU32(void* base, uint32_t off, uint32_t def = 0) {
    if (!base) return def;
    const void* p = (const char*)base + off;
    if (!MemReadable(p, 4)) return def;
    return *(const uint32_t*)p;
}

void CacheVersionStrings();

}  // namespace

const Reflect::Layout& Reflect::Lay() { return g_lay; }

void Reflect::Init() {
    if (g_ready) return;
    g_nameCtor = As<FnFNameCtor>("FName::FName");
    g_nameToString = As<FnFNameToString>("FName::ToStringInto");
    g_findFunction = As<FnFindFunction>("UObject::FindFunction");
    g_findProperty = As<FnFindProperty>("UStruct::FindPropertyByName");
    g_getPathName = As<FnGetPathName>("UObjectBaseUtility::GetPathName");
    g_findObjectPath = As<FnStaticFindObjectPath>("StaticFindObjectPath");
    g_getObjectsOfClass = As<FnGetObjectsOfClass>("GetObjectsOfClass");
    g_getObjectsWithOuter = As<FnGetObjectsWithOuter>("GetObjectsWithOuter");
    g_free = As<FnFree>("FMemory::Free");
    g_ready = g_nameCtor && g_nameToString && g_findFunction && g_findProperty;
    PluginLog("reflect: init ready=%d nameCtor=%p toString=%p findFunction=%p findProperty=%p getObjectsOfClass=%p",
              (int)g_ready, (void*)g_nameCtor, (void*)g_nameToString, (void*)g_findFunction, (void*)g_findProperty,
              (void*)g_getObjectsOfClass);
}

bool Reflect::Ready() { return g_ready; }
bool Reflect::Validated() { return g_validated; }

// ---------------------------------------------------------------------------------------------
// strings

std::string Reflect::Utf16To8(const char16_t* s, int len) {
    std::string o;
    if (!s || len <= 0) return o;
    if (!MemReadable(s, (size_t)len * 2)) return o;
    o.reserve((size_t)len);
    for (int i = 0; i < len; i++) {
        unsigned cp = s[i];
        if (!cp) break;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    return o;
}

std::vector<char16_t> Reflect::Utf8To16(const std::string& s) {
    std::vector<char16_t> o;
    o.reserve(s.size() + 1);
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
        if (cp < 0x10000) o.push_back((char16_t)cp);
        else {
            cp -= 0x10000;
            o.push_back((char16_t)(0xD800 + (cp >> 10)));
            o.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        }
    }
    o.push_back(0);
    return o;
}

std::string Reflect::ToStd(FString& s, bool freeIt) {
    std::string out = Utf16To8(s.Data, s.Num);
    if (freeIt && s.Data && g_free) g_free(s.Data);
    if (freeIt) { s.Data = nullptr; s.Num = s.Max = 0; }
    return out;
}

std::string Reflect::NameToString(const FName& n) {
    if (!g_nameToString) return "";
    FString fs;
    g_nameToString(&n, &fs);
    return ToStd(fs, true);
}

FName Reflect::MakeName(const std::string& s, bool add) {
    FName n;
    if (!g_nameCtor) return n;
    auto w = Utf8To16(s);
    g_nameCtor(&n, w.data(), add ? 1 : 0);
    return n;
}

// ---------------------------------------------------------------------------------------------
// objects

void* Reflect::ObjClass(void* obj) { return ReadPtr(obj, g_lay.objClass); }
void* Reflect::ObjOuter(void* obj) { return ReadPtr(obj, g_lay.objOuter); }
void* Reflect::SuperStruct(void* s) { return ReadPtr(s, g_lay.structSuper); }
void* Reflect::ClassDefaultObject(void* cls) { return ReadPtr(cls, g_lay.classDefaultObject); }

std::string Reflect::ObjName(void* obj) {
    if (!obj || !MemReadable((const char*)obj + g_lay.objName, 8)) return "";
    FName n;
    memcpy(&n, (const char*)obj + g_lay.objName, 8);
    return NameToString(n);
}

std::string Reflect::ClassName(void* obj) { return ObjName(ObjClass(obj)); }

std::string Reflect::ObjPathName(void* obj) {
    if (!obj || !g_getPathName) return "";
    FString fs;
    g_getPathName(&fs, obj, nullptr);
    return ToStd(fs, true);
}

bool Reflect::IsA(void* obj, void* cls) {
    void* c = ObjClass(obj);
    for (int i = 0; c && i < 64; i++) {
        if (c == cls) return true;
        c = SuperStruct(c);
    }
    return false;
}

void* Reflect::FindFunction(void* obj, const std::string& name) {
    if (!obj || !g_findFunction) return nullptr;
    return g_findFunction(obj, MakeName(name));
}

void* Reflect::FunctionNative(void* func) { return ReadPtr(func, g_lay.funcFunc); }

void* Reflect::FindProperty(void* structPtr, const std::string& name) {
    if (!structPtr || !g_findProperty) return nullptr;
    return g_findProperty(structPtr, MakeName(name));
}

int32_t Reflect::PropertyOffset(void* prop) { return (int32_t)ReadU32(prop, g_lay.propOffsetInternal, 0xFFFFFFFF); }

std::string Reflect::PropertyTypeName(void* prop) {
    void* fc = ReadPtr(prop, g_lay.fieldClass);
    if (!fc || !MemReadable(fc, 8)) return "";
    FName n;
    memcpy(&n, fc, 8);
    return NameToString(n);
}

void* Reflect::StaticClass(const char* symName) {
    auto fn = (FnStaticClass)(uintptr_t)Resolve::Addr(symName);
    if (!fn) return nullptr;
    return fn();
}

void* Reflect::FindObjectByPath(const std::string& package, const std::string& name) {
    if (!g_findObjectPath) return nullptr;
    FTopLevelAssetPath p{MakeName(package), MakeName(name)};
    if (!p.PackageName.Comparison || !p.AssetName.Comparison) return nullptr;  // name not in the FName pool
    return g_findObjectPath(nullptr, p, false);
}

bool Reflect::GetObjectsOfClass(void* cls, std::vector<void*>& out, bool includeDerived) {
    out.clear();
    if (!cls || !g_getObjectsOfClass) return false;
    TArray<void*> arr;
    g_getObjectsOfClass(cls, &arr, includeDerived, 0x10 /*RF_ClassDefaultObject*/, 0);
    if (arr.Data && arr.Num > 0 && MemReadable(arr.Data, (size_t)arr.Num * 8))
        out.assign(arr.Data, arr.Data + arr.Num);
    if (arr.Data && g_free) g_free(arr.Data);
    return true;
}

bool Reflect::GetObjectsWithOuter(void* outer, std::vector<void*>& out, bool includeNested) {
    out.clear();
    if (!outer || !g_getObjectsWithOuter) return false;
    TArray<void*> arr;
    g_getObjectsWithOuter(outer, &arr, includeNested, 0, 0);
    if (arr.Data && arr.Num > 0 && MemReadable(arr.Data, (size_t)arr.Num * 8))
        out.assign(arr.Data, arr.Data + arr.Num);
    if (arr.Data && g_free) g_free(arr.Data);
    return true;
}

// ---------------------------------------------------------------------------------------------
// boot validations

namespace {

struct Check {
    std::string name;
    bool ok = false;
    std::string detail;
};
Mutex g_validationLock;
std::vector<Check> g_validationChecks;

// Walks the FField chain of one UStruct; returns the properties in declaration order.
void WalkProps(void* strct, std::vector<void*>& out, size_t max) {
    void* f = ReadPtr(strct, g_lay.structChildProperties);
    for (size_t i = 0; f && i < max; i++) {
        out.push_back(f);
        f = ReadPtr(f, g_lay.fieldNext);
    }
}

// Tries candidate (childProperties, fieldName) offsets until FindPropertyByName agrees with our walk.
//
// IMPORTANT: nothing here converts an unverified FName to a string. FName::ToString() indexes the
// engine's name pool and segfaults on a bogus index, so candidate field values are only ever
// compared *as raw FName values* against what FindPropertyByName returns. (Learned the hard way:
// the first version stringified candidates and crashed the server inside FName::ToString.)
bool DiscoverPropertyLayout(void* cls, std::string& detail) {
    if (!cls) return false;
    for (uint32_t childOff = 0x40; childOff <= 0x70; childOff += 8) {
        void* first = ReadPtr(cls, childOff);
        if (!first || !MemReadable(first, 0x40)) continue;
        for (uint32_t nameOff = 0x18; nameOff <= 0x38; nameOff += 8) {
            FName n;
            memcpy(&n, (const char*)first + nameOff, 8);
            if (!n.Comparison || n.Number > 0xFFFF) continue;
            // Value-only round trip: ask the engine for the property with exactly this FName.
            if (!g_findProperty || g_findProperty(cls, n) != first) continue;
            g_lay.structChildProperties = childOff;
            g_lay.fieldName = nameOff;
            // FField::Next is discovered the same way: the candidate offset whose chain yields the
            // longest run of nodes that each round-trip through FindPropertyByName and terminates.
            uint32_t bestNext = 0;
            size_t bestLen = 0;
            for (uint32_t nextOff = 0x10; nextOff <= 0x30; nextOff += 8) {
                if (nextOff == nameOff) continue;
                size_t len = 0;
                void* f = first;
                bool clean = false;
                for (; f && len < 512; len++) {
                    FName fn;
                    if (!MemReadable((const char*)f + nameOff, 8)) break;
                    memcpy(&fn, (const char*)f + nameOff, 8);
                    if (!fn.Comparison || g_findProperty(cls, fn) != f) break;
                    void* nxt = ReadPtr(f, nextOff);
                    if (!nxt) { len++; clean = true; break; }
                    if (nxt == f) break;
                    f = nxt;
                }
                if (clean && len > bestLen) { bestLen = len; bestNext = nextOff; }
            }
            if (bestNext) g_lay.fieldNext = bestNext;
            // Only now are the FNames known-good, so they are safe to print.
            char b[224];
            snprintf(b, sizeof b, "childProperties=0x%x fieldName=0x%x fieldNext=0x%x (%zu properties, first '%s')",
                     childOff, nameOff, g_lay.fieldNext, bestLen, Reflect::NameToString(n).c_str());
            detail = b;
            return bestNext != 0;
        }
    }
    return false;
}

}  // namespace

void Reflect::Validate() {
    std::vector<Check> checks;
    auto add = [&](const char* n, bool ok, const std::string& d) { checks.push_back({n, ok, d}); };
    char b[512];

    // Anchors are stock-engine classes on purpose: AActor exists on every UE build, so a failure
    // here means our layout is wrong, never that Vein renamed something.
    void* actorCls = StaticClass("AActor::StaticClass");
    void* objectCls = StaticClass("UObject::StaticClass");

    // 1. StaticClass() gives a UClass whose NamePrivate holds the expected FName.
    // The comparison is by FName *value* (MakeName does a pool lookup, never an insert), so a wrong
    // offset yields a mismatch instead of a call into FName::ToString on garbage.
    FName want = MakeName("Actor");
    auto nameAt = [&](void* obj, uint32_t off) {
        FName n;
        if (!obj || !MemReadable((const char*)obj + off, 8)) return n;
        memcpy(&n, (const char*)obj + off, 8);
        return n;
    };
    bool ok = actorCls && want.Comparison && nameAt(actorCls, g_lay.objName) == want;
    if (!ok && actorCls && want.Comparison) {
        for (uint32_t off = 0x10; off <= 0x30; off += 4) {
            if (nameAt(actorCls, off) == want) {
                g_lay.objName = off;
                ok = true;
                break;
            }
        }
    }
    snprintf(b, sizeof b, "AActor::StaticClass()=%p NamePrivate@0x%x matches FName('Actor')", actorCls,
             g_lay.objName);
    add("objectName", ok, b);
    if (!ok) {
        // Without a confirmed name offset nothing below may stringify anything.
        add("superStruct", false, "skipped: UObject::NamePrivate offset unknown");
        add("classDefaultObject", false, "skipped: UObject::NamePrivate offset unknown");
        add("findFunctionNative", false, "skipped: UObject::NamePrivate offset unknown");
        add("propertyLayout", false, "skipped: UObject::NamePrivate offset unknown");
        add("propertyOffsets", false, "skipped: UObject::NamePrivate offset unknown");
        add("getObjectsOfClass", false, "skipped: UObject::NamePrivate offset unknown");
    }
    if (ok) {

    // 2. UStruct::SuperStruct: walking up from AActor must reach the UObject class.
    {
        void* c = actorCls;
        size_t steps = 0;
        bool reached = false;
        for (; c && steps < 32; steps++) {
            if (objectCls && c == objectCls) { reached = true; break; }
            c = SuperStruct(c);
        }
        if (!reached && objectCls) {
            for (uint32_t off = 0x28; off <= 0x60; off += 8) {
                void* cand = actorCls;
                size_t s2 = 0;
                bool hit = false;
                for (; cand && s2 < 32; s2++) {
                    if (cand == objectCls) { hit = true; break; }
                    cand = ReadPtr(cand, off);
                }
                if (hit && s2 > 0) { g_lay.structSuper = off; reached = true; steps = s2; break; }
            }
        }
        snprintf(b, sizeof b, "SuperStruct@0x%x: AActor -> UObject in %zu steps", g_lay.structSuper, steps);
        add("superStruct", reached, b);
    }

    // 3. CDO: ClassDefaultObject offset, cross-checked through the CDO's own Class pointer.
    FName wantCdo = MakeName("Default__Actor");
    void* cdo = ClassDefaultObject(actorCls);
    ok = cdo && ObjClass(cdo) == actorCls && (!wantCdo.Comparison || nameAt(cdo, g_lay.objName) == wantCdo);
    if (!ok) {
        for (uint32_t off = 0x60; off <= 0x200; off += 8) {
            void* cand = ReadPtr(actorCls, off);
            if (!cand || !MemReadable(cand, 0x30)) continue;
            if (ObjClass(cand) != actorCls) continue;
            if (wantCdo.Comparison && nameAt(cand, g_lay.objName) != wantCdo) continue;
            g_lay.classDefaultObject = off;
            cdo = cand;
            ok = true;
            break;
        }
    }
    snprintf(b, sizeof b, "CDO=%p name='%s' offset=0x%x", cdo, cdo ? ObjName(cdo).c_str() : "",
             g_lay.classDefaultObject);
    add("classDefaultObject", ok, b);

    // 4. FindFunction + UFunction::Func == the resolved address of the exec thunk.
    // AActor::K2_TeleportTo is a stock BlueprintCallable UFUNCTION, so this pins UFunction::Func
    // without depending on a single Vein symbol.
    uint64_t execAddr = Resolve::Addr("AActor::execK2_TeleportTo");
    void* fn = cdo ? FindFunction(cdo, "K2_TeleportTo") : nullptr;
    void* native = FunctionNative(fn);
    ok = fn && execAddr && (uint64_t)(uintptr_t)native == execAddr;
    if (!ok && fn && execAddr) {
        for (uint32_t off = 0x80; off <= 0x180; off += 8) {
            if ((uint64_t)(uintptr_t)ReadPtr(fn, off) == execAddr) {
                g_lay.funcFunc = off;
                native = (void*)(uintptr_t)execAddr;
                ok = true;
                break;
            }
        }
    }
    if (!fn) {
        // Still worth reporting the FunctionFlags sanity: a UFunction we cannot find means either
        // FindFunction or the CDO is wrong, and both are already covered above.
        snprintf(b, sizeof b, "FindFunction(CDO,'K2_TeleportTo') returned null (execK2_TeleportTo=0x%llx)",
                 (unsigned long long)execAddr);
    } else {
        snprintf(b, sizeof b,
                 "FindFunction(CDO,'K2_TeleportTo')=%p Func=%p resolved(execK2_TeleportTo)=0x%llx "
                 "Func@0x%x FunctionFlags@0x%x=0x%x",
                 fn, native, (unsigned long long)execAddr, g_lay.funcFunc, g_lay.funcFunctionFlags,
                 ReadU32(fn, g_lay.funcFunctionFlags, 0));
    }
    add("findFunctionNative", ok, b);

    // 5. Property layout (ChildProperties / FField::NamePrivate / FField::Next), self-checked
    // against FindPropertyByName. Nothing here is assumed: the UE4SS-Vein layout ini says
    // 0x18/0x20, but the discovered value is what the plugin uses.
    std::string detail;
    void* propCls = actorCls;
    ok = DiscoverPropertyLayout(propCls, detail);
    if (!ok) detail = "no (childProperties, fieldName) pair agreed with FindPropertyByName on " + ObjName(propCls);
    add("propertyLayout", ok, detail);

    // 6. Property offsets look sane for that class (Offset_Internal < PropertiesSize).
    if (ok) {
        std::vector<void*> props;
        WalkProps(propCls, props, 256);
        uint32_t size = ReadU32(propCls, g_lay.structPropertiesSize, 0);
        size_t bad = 0;
        for (void* p : props) {
            int32_t o = PropertyOffset(p);
            if (o < 0 || (size && (uint32_t)o >= size)) bad++;
        }
        bool sane = !props.empty() && bad == 0;
        snprintf(b, sizeof b, "%s: %zu properties, propertiesSize=%u, %zu with an implausible Offset_Internal(0x%x)",
                 ObjName(propCls).c_str(), props.size(), size, bad, g_lay.propOffsetInternal);
        add("propertyOffsets", sane, b);
    } else {
        add("propertyOffsets", false, "skipped: property layout unknown");
    }

    // 7. GetObjectsOfClass works (uses the object array; world-independent).
    std::vector<void*> objs;
    bool got = GetObjectsOfClass(objectCls, objs, true);
    snprintf(b, sizeof b, "GetObjectsOfClass(UObject, derived) -> %zu objects", objs.size());
    add("getObjectsOfClass", got && objs.size() > 1000, b);

    }  // end of the checks that need a confirmed NamePrivate offset

    // Publish.
    size_t okCount = 0;
    for (size_t i = 0; i < checks.size(); i++) {
        if (checks[i].ok) okCount++;
        PluginLog("reflect-validate %-24s %s  %s", checks[i].name.c_str(), checks[i].ok ? "OK  " : "FAIL",
                  checks[i].detail.c_str());
    }
    { Guard guard(g_validationLock); g_validationChecks = checks; }
    g_validated = true;

    try { CacheVersionStrings(); } catch (...) {}

    auto& st = PluginState::Get();
    bool core = checks.size() >= 4 && checks[0].ok && checks[1].ok && checks[2].ok && checks[3].ok;
    st.SetCapability("reflection", core ? "ok" : "degraded",
                     core ? "" : "boot validation failed; see /health diagnostics.reflect");
    bool propsOk = false;
    for (auto& c : checks)
        if (c.name == "propertyLayout") propsOk = c.ok;
    st.SetCapability("reflectProperties", propsOk ? "ok" : "degraded",
                     propsOk ? "" : "UStruct::ChildProperties / FField::NamePrivate not confirmed");
    PluginLog("reflect: %zu/%zu boot validations passed", okCount, checks.size());
}

namespace {
std::string g_gameBuild, g_engineVersion;
Mutex g_versionLock;

void CacheVersionStrings() {
    std::string gameBuild, engineVersion;
    using FnBuildVersion = const char16_t* (*)();
    using FnCurrent = const void* (*)();
    using FnVerToString = void (*)(FString* ret, const void* self, int component);  // FString return -> sret first
    auto bv = (FnBuildVersion)(uintptr_t)Resolve::Addr("FApp::GetBuildVersion");
    if (bv) {
        const char16_t* s = bv();
        if (MemReadable(s, 2)) {
            int len = 0;
            while (len < 256 && MemReadable(s + len, 2) && s[len]) len++;
            gameBuild = Reflect::Utf16To8(s, len);
        }
    }
    auto cur = (FnCurrent)(uintptr_t)Resolve::Addr("FEngineVersion::Current");
    auto ts = (FnVerToString)(uintptr_t)Resolve::Addr("FEngineVersion::ToString");
    if (cur && ts) {
        const void* v = cur();
        if (v && MemReadable(v, 8)) {
            FString out;
            ts(&out, v, 3 /*EVersionComponent::Changelist*/);
            engineVersion = Reflect::ToStd(out, true);
        }
    }
    PluginLog("reflect: gameBuild='%s' engineVersion='%s'", gameBuild.c_str(), engineVersion.c_str());
    Guard guard(g_versionLock);
    g_gameBuild = std::move(gameBuild);
    g_engineVersion = std::move(engineVersion);
}
}  // namespace

std::string Reflect::GameBuild() { Guard guard(g_versionLock); return g_gameBuild; }
std::string Reflect::EngineVersion() { Guard guard(g_versionLock); return g_engineVersion; }

// Reference offsets for this exact build. Two independent sources agree on every value below: the
// depot's own DWARF (`VeinServer-Linux-Test.debug`, read by `tools/dwarfoffsets.py`) and UE4SS-Vein's
// `MemberVariableLayout.ini`. They are a *cross-check only*: the plugin always uses the
// value it discovered at boot, and /health reports where the two disagree so a silent drift after a
// game update is visible instead of assumed away.
namespace {
struct RefOffset {
    const char* key;
    uint32_t value;
};
const RefOffset kReferenceLayout[] = {
    {"objClass", 0x10},         {"objName", 0x18},          {"objOuter", 0x20},
    {"structSuper", 0x40},      {"structChildProperties", 0x50}, {"structPropertiesSize", 0x58},
    {"classDefaultObject", 0x110}, {"funcFunctionFlags", 0xB0}, {"funcFunc", 0xD8},
    {"fieldNext", 0x18},        {"fieldName", 0x20},        {"propOffsetInternal", 0x44},
};
const size_t kReferenceCount = sizeof(kReferenceLayout) / sizeof(kReferenceLayout[0]);

uint32_t LiveOffset(const std::string& key) {
    const Reflect::Layout& l = Reflect::Lay();
    if (key == "objClass") return l.objClass;
    if (key == "objName") return l.objName;
    if (key == "objOuter") return l.objOuter;
    if (key == "structSuper") return l.structSuper;
    if (key == "structChildProperties") return l.structChildProperties;
    if (key == "structPropertiesSize") return l.structPropertiesSize;
    if (key == "classDefaultObject") return l.classDefaultObject;
    if (key == "funcFunctionFlags") return l.funcFunctionFlags;
    if (key == "funcFunc") return l.funcFunc;
    if (key == "fieldNext") return l.fieldNext;
    if (key == "fieldName") return l.fieldName;
    if (key == "propOffsetInternal") return l.propOffsetInternal;
    return 0xFFFFFFFF;
}
}  // namespace

std::string Reflect::LayoutJson() {
    std::vector<Check> checks;
    { Guard guard(g_validationLock); checks = g_validationChecks; }
    std::string validations = "[";
    for (const auto& check : checks) {
        if (validations.size() > 1) validations += ",";
        validations += "{\"check\":" + JsonStr(check.name) + ",\"ok\":" + (check.ok ? "true" : "false") +
                       ",\"detail\":" + JsonStr(check.detail) + "}";
    }
    validations += "]";
    auto n = [](const char* k, uint32_t v) { return "\"" + std::string(k) + "\":" + std::to_string(v) + ","; };
    std::string ref = "{\"source\":\"depot DWARF (VeinServer-Linux-Test.debug) and UE4SS-Vein "
                      "MemberVariableLayout.ini, which agree\",\"mismatches\":[";
    bool firstRef = true;
    for (size_t i = 0; i < kReferenceCount; i++) {
        uint32_t live = LiveOffset(kReferenceLayout[i].key);
        if (live == kReferenceLayout[i].value) continue;
        if (!firstRef) ref += ",";
        firstRef = false;
        ref += "{\"name\":\"" + std::string(kReferenceLayout[i].key) + "\",\"reference\":" +
               std::to_string(kReferenceLayout[i].value) + ",\"live\":" + std::to_string(live) + "}";
    }
    ref += "]}";
    return "{" + n("objClass", g_lay.objClass) + n("objName", g_lay.objName) + n("objOuter", g_lay.objOuter) +
           n("structSuper", g_lay.structSuper) + n("structChildProperties", g_lay.structChildProperties) +
           n("structPropertiesSize", g_lay.structPropertiesSize) + n("classDefaultObject", g_lay.classDefaultObject) +
           n("funcFunc", g_lay.funcFunc) + n("fieldNext", g_lay.fieldNext) + n("fieldName", g_lay.fieldName) +
           n("propOffsetInternal", g_lay.propOffsetInternal) + n("funcFunctionFlags", g_lay.funcFunctionFlags) +
           "\"reference\":" + ref + ",\"validations\":" + validations + "}";
}

// ---------------------------------------------------------------------------------------------
// /debug/object and /debug/structs

namespace {

using DebugSnapshot = Reflect::DebugSnapshot;
DebugSnapshot Literal(std::string text) { return [text = std::move(text)] { return text; }; }
template<class T> DebugSnapshot Numeric(T value) { return [value] { return std::to_string(value); }; }
DebugSnapshot Text(std::string value) { return [value = std::move(value)] { return JsonStr(value); }; }
DebugSnapshot DumpValue(void* obj, void* prop, const std::string& type, const std::string& propName) {
    int32_t off = Reflect::PropertyOffset(prop);
    if (off < 0 || off > 0x100000) return Literal("null");
    const void* p = (const char*)obj + off;
    auto rd = [&](size_t n) { return MemReadable(p, n); };
    if (!rd(1)) return Literal("null");
    if (type == "BoolProperty") return Literal(*(const uint8_t*)p ? "true" : "false");
    if (type == "ByteProperty" || type == "EnumProperty") return Numeric(*(const uint8_t*)p);
    if (type == "Int8Property") return Numeric(*(const int8_t*)p);
    if (type == "Int16Property" && rd(2)) return Numeric(*(const int16_t*)p);
    if (type == "UInt16Property" && rd(2)) return Numeric(*(const uint16_t*)p);
    if (type == "IntProperty" && rd(4)) return Numeric(*(const int32_t*)p);
    if (type == "UInt32Property" && rd(4)) return Numeric(*(const uint32_t*)p);
    if (type == "Int64Property" && rd(8)) return Numeric(*(const int64_t*)p);
    if (type == "UInt64Property" && rd(8)) return Numeric(*(const uint64_t*)p);
    if (type == "FloatProperty" && rd(4)) { const float v = *(const float*)p; return [v] { return JsonNum(v); }; }
    if (type == "DoubleProperty" && rd(8)) { const double v = *(const double*)p; return [v] { return JsonNum(v); }; }
    if (type == "NameProperty" && rd(8)) {
        FName n; memcpy(&n, p, 8);
        return Text(Reflect::NameToString(n));
    }
    if (type == "StrProperty" && rd(16)) {
        FString value; memcpy(&value, p, 16);
        if (value.Num < 0 || value.Num > 4096 || (value.Num && !MemReadable(value.Data, value.Num * 2)))
            return Literal("null");
        return [name = propName, text = Reflect::Utf16To8(value.Data, value.Num)] {
            return JsonStr(Redact(name + "=" + text).substr(name.size() + 1));
        };
    }
    if ((type == "ObjectProperty" || type == "ClassProperty" || type == "WeakObjectProperty" ||
         type == "ObjectPtrProperty" || type == "SoftObjectProperty") && rd(8)) {
        void* o = *(void* const*)p;
        if (!o || !MemReadable(o, 0x30)) return Literal("null");
        char address[32]; snprintf(address, sizeof address, "%p", o);
        return [address = std::string(address), cls = Reflect::ClassName(o), name = Reflect::ObjName(o)] {
            return "{\"ptr\":" + JsonStr(address) + ",\"class\":" + JsonStr(cls) + ",\"name\":" + JsonStr(name) + "}";
        };
    }
    if (type == "ArrayProperty" && rd(16)) {
        TArray<void*> array; memcpy(&array, p, 16);
        return [count = array.Num] { return "{\"arrayNum\":" + std::to_string(count) + "}"; };
    }
    return Literal("null");
}
struct PropertyView {
    std::string name, type;
    int32_t offset;
    DebugSnapshot value;
};
std::string PropertiesJson(const std::vector<PropertyView>& properties) {
    std::string out = "[";
    for (const auto& p : properties) {
        if (out.size() > 1) out += ",";
        out += "{\"name\":" + JsonStr(p.name) + ",\"type\":" + JsonStr(p.type) +
               ",\"offset\":" + std::to_string(p.offset);
        if (p.value) out += ",\"value\":" + p.value();
        out += "}";
    }
    return out + "]";
}
std::vector<PropertyView> CaptureProperties(void* structure, void* object, int limit) {
    std::vector<void*> props; WalkProps(structure, props, 512);
    std::vector<PropertyView> out;
    for (void* prop : props) {
        if ((int)out.size() >= limit) break;
        FName n;
        if (!MemReadable((const char*)prop + g_lay.fieldName, 8)) continue;
        memcpy(&n, (const char*)prop + g_lay.fieldName, 8);
        PropertyView row{Reflect::NameToString(n), Reflect::PropertyTypeName(prop), Reflect::PropertyOffset(prop), {}};
        if (object) row.value = DumpValue(object, prop, row.type, row.name);
        out.push_back(std::move(row));
    }
    return out;
}
} // namespace

Reflect::DebugSnapshot Reflect::DumpObject(void* obj, int maxProps) {
    if (!obj || !MemReadable(obj, 0x40)) return Literal("{\"error\":\"object pointer is not readable\"}");
    struct ClassView { std::string name; std::vector<PropertyView> properties; };
    std::vector<ClassView> classes;
    int emitted = 0;
    bool isStruct = false;
    for (void* cls = ObjClass(obj); cls && classes.size() < 32; cls = SuperStruct(cls)) {
        std::string name = ObjName(cls);
        if (name == "Struct" || name == "Class" || name == "ScriptStruct") isStruct = true;
        auto props = CaptureProperties(cls, obj, maxProps - emitted);
        emitted += props.size();
        classes.push_back({std::move(name), std::move(props)});
    }
    auto declared = isStruct ? CaptureProperties(obj, nullptr, 512) : std::vector<PropertyView>();
    char address[32]; snprintf(address, sizeof address, "%p", obj);
    return [address = std::string(address), name = ObjName(obj), cls = ClassName(obj), path = ObjPathName(obj),
            classes = std::move(classes), declared = std::move(declared), isStruct, truncated = emitted >= maxProps] {
        std::string out = "{\"ptr\":" + JsonStr(address) + ",\"name\":" + JsonStr(name) +
                          ",\"class\":" + JsonStr(cls) + ",\"path\":" + JsonStr(path) + ",\"classes\":[";
        bool first = true;
        for (const auto& c : classes) {
            if (!first) out += ",";
            first = false;
            out += "{\"class\":" + JsonStr(c.name) + ",\"properties\":" + PropertiesJson(c.properties) + "}";
        }
        out += "]";
        if (isStruct) out += ",\"declaredProperties\":" + PropertiesJson(declared);
        return out + ",\"truncated\":" + (truncated ? "true" : "false") + "}";
    };
}

Reflect::DebugSnapshot Reflect::DumpStruct(const std::string& name) {
    static const char* kPackages[] = {"/Script/Vein", "/Script/Engine", "/Script/CoreUObject", "/Script/VeinRuntime"};
    void* s = nullptr;
    std::string pkg;
    size_t dot = name.find('.');
    if (name.size() > 1 && name[0] == '/' && dot != std::string::npos) {
        s = FindObjectByPath(name.substr(0, dot), name.substr(dot + 1));
        pkg = name.substr(0, dot);
    } else {
        for (auto* p : kPackages) {
            s = FindObjectByPath(p, name);
            if (s) { pkg = p; break; }
        }
    }
    if (!s) {
        // Fallback: the package of a UClass/UScriptStruct is not always guessable, so scan the live
        // object array for a UStruct with this name. This is the discovery path L2/L3 use to learn
        // the layout of game structs like FChatMessageData.
        std::string want = name;
        size_t d2 = want.rfind('.');
        if (d2 != std::string::npos) want = want.substr(d2 + 1);
        if (!want.empty() && want[0] == 'F') {
            // Both "FChatMessageData" and "ChatMessageData" are accepted.
        }
        std::vector<void*> all;
        GetObjectsOfClass(StaticClass("UObject::StaticClass"), all, true);
        for (void* o : all) {
            std::string cn = ClassName(o);
            if (cn != "Class" && cn != "ScriptStruct") continue;
            std::string on = ObjName(o);
            if (on == want || ("F" + on) == want) {
                s = o;
                pkg = ObjName(ObjOuter(o));
                break;
            }
        }
    }
    if (!s) return Literal("{\"error\":\"struct not found; pass the full path (/Script/Pkg.Name) or a plain class/struct name\"}");
    auto properties = CaptureProperties(s, nullptr, 512);
    return [pkg, name = ObjName(s), cls = ClassName(s), size = ReadU32(s, g_lay.structPropertiesSize, 0),
            super = ObjName(SuperStruct(s)), properties = std::move(properties)] {
        return "{\"package\":" + JsonStr(pkg) + ",\"name\":" + JsonStr(name) + ",\"class\":" + JsonStr(cls) +
               ",\"propertiesSize\":" + std::to_string(size) + ",\"super\":" + JsonStr(super) +
               ",\"properties\":" + PropertiesJson(properties) + "}";
    };
}
