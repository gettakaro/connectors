// Full reflection dump to JSON, on a worker thread with safe reads (no game-thread time).
#include "probe.h"

#include "proto.h"

#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Probe {
namespace Dump {
namespace {
std::mutex g_lock;
std::string g_state = "idle", g_path, g_error, g_summary;
bool g_running = false;

struct Out {
    FILE* f;
    JW w;
    size_t bytes = 0;
    void flush(bool force) {
        if (!force && w.s.size() < (1 << 20)) return;
        fwrite(w.s.data(), 1, w.s.size(), f);
        bytes += w.s.size();
        w.s.clear();
    }
};

std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void WriteObjList(JW& w, const std::vector<uintptr_t>& objs, size_t limit) {
    w.beginArr();
    for (size_t i = 0; i < objs.size() && i < limit; i++) {
        w.beginObj();
        w.kstr("addr", Hex(objs[i]));
        w.kstr("name", ObjName(objs[i]));
        w.kstr("class", ClassNameOf(objs[i]));
        w.kstr("path", ObjPath(objs[i]));
        w.endObj();
    }
    w.endArr();
}

void Run(std::string path) {
    const uint64_t t0 = NowMs();
    std::string err;
    if (!EnsureCore(err)) {
        std::lock_guard<std::mutex> g(g_lock);
        g_state = "error";
        g_error = err;
        g_running = false;
        return;
    }
    const Meta& m = Core();
    // Pass 1: classify every object.
    std::vector<uintptr_t> structs, enums;
    std::unordered_map<uintptr_t, long long> instances;  // class -> live non-CDO instances
    std::unordered_map<uintptr_t, uintptr_t> cdo;
    std::unordered_map<uintptr_t, std::vector<uintptr_t>> byClass;  // only for the singleton classes
    std::unordered_map<uintptr_t, int> kind;  // meta class -> 0 other, 1 struct, 2 function, 3 enum
    std::vector<uintptr_t> functions;
    long long objects = 0, dead = 0;
    ForEachObject([&](uintptr_t o, uint32_t fl) {
        objects++;
        if (fl & (0x10000000 | 0x20000000)) {
            dead++;
            return;
        }
        Obj ob;
        if (!ReadObj(o, ob) || !ob.cls) return;
        if (ob.flags & 0x10) cdo[ob.cls] = o;
        else if (!(ob.flags & 0x20)) instances[ob.cls]++;
        auto it = kind.find(ob.cls);
        if (it == kind.end()) {
            int k = IsA(ob.cls, m.Function) ? 2 : IsA(ob.cls, m.Struct) ? 1 : IsA(ob.cls, m.Enum) ? 3 : 0;
            it = kind.emplace(ob.cls, k).first;
        }
        if (it->second == 1) structs.push_back(o);
        else if (it->second == 2) functions.push_back(o);
        else if (it->second == 3) enums.push_back(o);
        if (!(ob.flags & 0x30)) byClass[ob.cls].push_back(o);
    });
    const uint64_t tWalk = NowMs();

    FILE* f = fopen((path + ".tmp").c_str(), "w");
    if (!f) {
        std::lock_guard<std::mutex> g(g_lock);
        g_state = "error";
        g_error = "cannot open " + path + ".tmp";
        g_running = false;
        return;
    }
    Out out{f, JW()};
    JW& w = out.w;
    w.beginObj();
    w.key("meta");
    w.beginObj();
    w.kstr("tool", std::string("takaro-conan-probe ") + kVersion);
    w.kstr("serverBuild", "25639945");
    w.kstr("buildId", ReadElfBuildId("/proc/self/exe"));
    {
        time_t now = time(nullptr);
        char b[32];
        strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
        w.kstr("generatedAt", b);
    }
    w.knum("objectSlots", objects);
    w.knum("deadSlots", dead);
    w.knum("structs", (long long)structs.size());
    w.knum("functions", (long long)functions.size());
    w.knum("enums", (long long)enums.size());
    w.knum("walkMs", (long long)(tWalk - t0));
    w.endObj();
    w.key("layout");
    w.beginObj();
    w.kstr("note", "byte offsets used by the probe; build 25639945, Linux x64");
    w.kstr("GObjObjects", "0xc35a580 (item stride 0x18: +4 flags, +8 UObject*)");
    w.kstr("FNamePoolBlocks", "0xc2a5d40");
    w.kstr("ProcessEvent", "0x3f12340 (vtable slot 78)");
    w.kstr("UObject", "flags +0x8, index +0xC, class +0x10, name +0x18, outer +0x20");
    w.kstr("UField", "next +0x28");
    w.kstr("UStruct", "super +0x40, children(UField) +0x48, childProperties(FField) +0x50, propertiesSize +0x58");
    w.kstr("UFunction", "flags +0xB0, numParms +0xB4 (u8), parmsSize +0xB6 (u16), returnValueOffset +0xB8 (u16), func +0xD8");
    w.kstr("UEnum", "cppType FString +0x30, names TArray<{FName,int64}> +0x40");
    w.kstr("FField", "class +0x8 (FFieldClass name +0x8), next +0x18, name +0x20");
    w.kstr("FProperty", "arrayDim +0x2C, elementSize +0x30, propertyFlags +0x38, offset +0x44, type pointers +0x70/+0x78, bool bytes +0x70..0x73");
    w.endObj();
    out.flush(true);

    // Structs (classes and script structs) with their properties and functions.
    {
        std::vector<std::pair<std::string, uintptr_t>> keyed;
        for (uintptr_t st : structs) keyed.push_back({ObjPath(st), st});
        std::sort(keyed.begin(), keyed.end());
        for (size_t i = 0; i < keyed.size(); i++) structs[i] = keyed[i].second;
    }
    w.key("structs");
    w.beginArr();
    long long funcListed = 0;
    for (uintptr_t st : structs) {
        auto ic = instances.find(st);
        auto cd = cdo.find(st);
        WriteStruct(w, st, ic == instances.end() ? 0 : ic->second, cd == cdo.end() ? 0 : cd->second);
        out.flush(false);
    }
    w.endArr();
    // Functions whose outer is not a struct (package-level delegate signatures).
    w.key("packageFunctions");
    w.beginArr();
    std::unordered_set<uintptr_t> structSet(structs.begin(), structs.end());
    for (uintptr_t fn : functions) {
        uintptr_t outer = Mem::Rd<uintptr_t>(fn + 0x20);
        if (structSet.count(outer)) {
            funcListed++;
            continue;
        }
        FuncInfo fi;
        if (!ReadFunc(fn, fi)) continue;
        w.beginObj();
        w.kstr("outer", ObjPath(outer));
        w.key("function");
        WriteFunc(w, fi);
        w.endObj();
        out.flush(false);
    }
    w.endArr();
    w.key("enums");
    w.beginArr();
    for (uintptr_t en : enums) {
        WriteEnum(w, en);
        out.flush(false);
    }
    w.endArr();

    // Singletons, CDOs and live instances the later spikes need.
    w.key("singletons");
    w.beginObj();
    const char* bases[] = {"GameModeBase", "GameSession", "GameStateBase", "CheatManager", "PlayerController",
                           "PlayerState", "WorldSettings", "GameInstance", "Engine", "GameEngine", "World"};
    for (const char* b : bases) {
        uintptr_t base = FindType(b);
        w.key(b);
        w.beginObj();
        w.kstr("class", base ? Hex(base) : "");
        std::vector<uintptr_t> live;
        std::vector<std::pair<std::string, long long>> subclasses;
        if (base) {
            for (auto& kv : instances)
                if (IsA(kv.first, base)) subclasses.push_back({ObjPath(kv.first), kv.second});
            for (auto& kv : byClass)
                if (IsA(kv.first, base))
                    for (auto o : kv.second) live.push_back(o);
        }
        std::sort(subclasses.begin(), subclasses.end());
        w.key("liveClasses");
        w.beginArr();
        for (auto& s : subclasses) {
            w.beginArr();
            w.str(s.first);
            w.num(s.second);
            w.endArr();
        }
        w.endArr();
        w.key("instances");
        WriteObjList(w, live, 40);
        w.endObj();
    }
    w.endObj();
    // Classes whose name contains a keyword, with CDO and live instances.
    w.key("keywordClasses");
    w.beginObj();
    const char* keys[] = {"GameMode", "GameSession", "GameState", "Blacklist", "Ban", "CheatManager", "Cheat",
                          "Admin", "Console", "ItemTable", "Inventory", "Shutdown", "Kick"};
    for (const char* k : keys) {
        const std::string lk = Lower(k);
        w.key(k);
        w.beginArr();
        int n = 0;
        for (uintptr_t st : structs) {
            if (n >= 200) break;
            if (Lower(ObjName(st)).find(lk) == std::string::npos) continue;
            n++;
            w.beginObj();
            w.kstr("name", ObjName(st));
            w.kstr("kind", ClassNameOf(st));
            w.kstr("path", ObjPath(st));
            auto ic = instances.find(st);
            w.knum("instances", ic == instances.end() ? 0 : ic->second);
            auto cd = cdo.find(st);
            if (cd != cdo.end()) w.kstr("cdo", Hex(cd->second));
            auto bc = byClass.find(st);
            if (bc != byClass.end()) {
                w.key("live");
                WriteObjList(w, bc->second, 10);
            }
            w.endObj();
        }
        w.endArr();
    }
    w.endObj();
    // Data tables (the item table among them) with their row struct.
    w.key("dataTables");
    w.beginArr();
    if (uintptr_t dt = FindType("DataTable")) {
        Prop rowStruct;
        bool haveRow = FindProp(dt, "RowStruct", rowStruct);
        for (auto& kv : byClass) {
            if (!IsA(kv.first, dt)) continue;
            for (uintptr_t o : kv.second) {
                w.beginObj();
                w.kstr("addr", Hex(o));
                w.kstr("path", ObjPath(o));
                w.kstr("class", ObjName(kv.first));
                if (haveRow) w.kstr("rowStruct", ObjName(Mem::Rd<uintptr_t>(o + (uintptr_t)rowStruct.offset)));
                w.endObj();
            }
            out.flush(false);
        }
    }
    w.endArr();
    // Live-instance histogram.
    w.key("instanceHistogram");
    w.beginArr();
    {
        std::vector<std::pair<long long, uintptr_t>> h;
        for (auto& kv : instances) h.push_back({kv.second, kv.first});
        std::sort(h.rbegin(), h.rend());
        for (size_t i = 0; i < h.size() && i < 400; i++) {
            w.beginArr();
            w.str(ObjPath(h[i].second));
            w.num(h[i].first);
            w.endArr();
        }
    }
    w.endArr();
    w.key("stats");
    w.beginObj();
    w.knum("functionsUnderStructs", funcListed);
    w.knum("totalMs", (long long)(NowMs() - t0));
    w.endObj();
    w.endObj();
    out.flush(true);
    fclose(f);
    rename((path + ".tmp").c_str(), path.c_str());
    std::lock_guard<std::mutex> g(g_lock);
    g_state = "done";
    char b[512];
    snprintf(b, sizeof b,
             "{\"path\":%s,\"bytes\":%zu,\"objects\":%lld,\"structs\":%zu,\"functions\":%zu,\"enums\":%zu,"
             "\"functionsUnderStructs\":%lld,\"ms\":%llu}",
             JsonStr(path).c_str(), out.bytes, objects, structs.size(), functions.size(), enums.size(), funcListed,
             (unsigned long long)(NowMs() - t0));
    g_summary = b;
    g_running = false;
    NativeLog("dump done: %s", b);
}
}  // namespace

std::string Start(const std::string& outPath, std::string& err) {
    std::lock_guard<std::mutex> g(g_lock);
    if (g_running) {
        err = "a dump is already running";
        return "";
    }
    g_running = true;
    g_state = "running";
    g_error.clear();
    g_summary.clear();
    g_path = outPath;
    std::thread(Run, outPath).detach();
    return outPath;
}

std::string Status() {
    std::lock_guard<std::mutex> g(g_lock);
    JW w;
    w.beginObj();
    w.kstr("state", g_state);
    w.kstr("path", g_path);
    if (!g_error.empty()) w.kstr("error", g_error);
    if (!g_summary.empty()) {
        w.key("summary");
        w.raw(g_summary);
    }
    w.endObj();
    return w.s;
}

}  // namespace Dump
}  // namespace Probe
