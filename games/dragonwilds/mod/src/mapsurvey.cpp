// Map survey: see mapsurvey.h. Traces run in bounded game-thread jobs (one chunk of a row each);
// the survey thread owns the buffers and does every file write.
#include "mapsurvey.h"

#include "gamethread.h"
#include "reflect.h"
#include "sym.h"

#include <sys/stat.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>

namespace MapSurvey {

namespace {

using FnLineTrace = bool (*)(const void* world, void* hit, const double* start, const double* end, int channel,
                             const void* queryParams, const void* responseParams);
using FnQueryParamsCtor = void (*)(void* self, UE::FName tag, const void* statId, bool traceComplex,
                                   const void* ignoreActor);
using FnActorBounds = void (*)(const void* actor, bool onlyColliding, double* origin, double* extent,
                               bool includeChildActors);

constexpr int kMaxSide = 8192;     // Takaro's map upload accepts at most 8192 px per side
constexpr size_t kHitBuf = 1024;   // FHitResult is 256 bytes on UE 5.6; checked against reflection

struct Progress {
    Mutex lock;
    bool running = false;
    bool done = false;
    std::string error;
    Grid grid;
    double zTop = 0, zBottom = 0;
    int channel = 0, landscapes = 0, row = 0;
    uint64_t hits = 0, startedMs = 0, elapsedMs = 0, gameThreadUs = 0;
    std::string dir;
};
Progress& P() {
    static Progress* p = new Progress();
    return *p;
}

void* FindWorld() {
    void* cls = Reflect::StaticClass("UWorld::StaticClass");
    if (!cls) return nullptr;
    std::vector<void*> worlds;
    Reflect::GetObjectsOfClass(cls, worlds, true);
    void* fallback = nullptr;
    for (void* w : worlds) {
        void* gsProp = nullptr;
        for (void* c = Reflect::ObjClass(w); c && !gsProp; c = Reflect::SuperStruct(c))
            gsProp = Reflect::FindProperty(c, "GameState");
        if (!gsProp) continue;
        if (!fallback) fallback = w;
        int32_t off = Reflect::PropertyOffset(gsProp);
        const void* at = (const char*)w + off;
        if (off >= 0 && MemReadable(at, 8) && *(void* const*)at) return w;
    }
    return fallback;
}

int32_t StructOffset(void* st, const char* name) {
    void* p = st ? Reflect::FindProperty(st, name) : nullptr;
    return p ? Reflect::PropertyOffset(p) : -1;
}

double Num(const JsonValue& body, const char* key, double def) {
    const JsonValue* v = body.get(key);
    return v && v->isNum() ? v->num : def;
}

bool WriteFloats(const std::string& path, const std::vector<float>& v) {
    std::string bytes((const char*)v.data(), v.size() * sizeof(float));
    return WriteFileAtomic(path, bytes);
}

// The whole survey; runs on its own thread.
void Run(Grid grid, double zTop, double zBottom, int channel, int perJob) {
    auto trace = (FnLineTrace)(uintptr_t)Sym::Addr("UWorld::LineTraceSingleByChannel");
    auto ctor = (FnQueryParamsCtor)(uintptr_t)Sym::Addr("FCollisionQueryParams::FCollisionQueryParams");
    const size_t n = (size_t)grid.w * (size_t)grid.h;
    auto heights = std::make_shared<std::vector<float>>(n, std::numeric_limits<float>::quiet_NaN());
    auto normalz = std::make_shared<std::vector<float>>(n, std::numeric_limits<float>::quiet_NaN());

    // Query/response params and the hit layout are prepared once on the game thread.
    struct Shared {
        alignas(16) unsigned char query[1024];
        alignas(16) unsigned char response[64];
        alignas(16) unsigned char statId[32];
        void* world = nullptr;
        int32_t impactPoint = -1, impactNormal = -1, blocking = -1, size = 0;
    };
    auto sh = std::make_shared<Shared>();
    auto prep = std::make_shared<std::string>();
    bool ran = GameThread::Run(
        [sh, prep, ctor] {
            memset(sh->query, 0, sizeof sh->query);
            memset(sh->statId, 0, sizeof sh->statId);
            // ECR_Block for every channel: the same as FCollisionResponseParams::DefaultResponseParam.
            memset(sh->response, 2, 32);
            void* hit = Reflect::FindObjectByPath("/Script/Engine", "HitResult");
            sh->impactPoint = StructOffset(hit, "ImpactPoint");
            sh->impactNormal = StructOffset(hit, "ImpactNormal");
            sh->blocking = StructOffset(hit, "bBlockingHit");
            sh->size = hit ? *(int32_t*)((char*)hit + Reflect::Lay().structPropertiesSize) : 0;
            sh->world = FindWorld();
            if (!sh->world) { *prep = "no game world"; return; }
            if (sh->impactPoint < 0 || sh->impactNormal < 0 || sh->size <= 0 || (size_t)sh->size > kHitBuf) {
                *prep = "FHitResult layout not readable";
                return;
            }
            ctor(sh->query, UE::FName{}, sh->statId, false, nullptr);
        },
        10000);
    if (!ran && prep->empty()) *prep = "game thread unavailable";
    if (!prep->empty()) {
        Guard g(P().lock);
        P().running = false;
        P().error = *prep;
        return;
    }

    auto hits = std::make_shared<std::atomic<uint64_t>>(0);
    auto usSpent = std::make_shared<std::atomic<uint64_t>>(0);
    std::string error;
    for (int row = 0; row < grid.h && error.empty(); row++) {
        for (int c0 = 0; c0 < grid.w && error.empty(); c0 += perJob) {
            int c1 = std::min(grid.w, c0 + perJob);
            bool ok = GameThread::Run(
                [=] {
                    timespec a{}, b{};
                    clock_gettime(CLOCK_MONOTONIC, &a);
                    alignas(16) unsigned char hit[kHitBuf];
                    double y = grid.minY + (row + 0.5) * grid.step;
                    for (int col = c0; col < c1; col++) {
                        double x = grid.minX + (col + 0.5) * grid.step;
                        double s[3] = {x, y, zTop}, e[3] = {x, y, zBottom};
                        memset(hit, 0, (size_t)sh->size);
                        if (!trace(sh->world, hit, s, e, channel, sh->query, sh->response)) continue;
                        const double* ip = (const double*)(hit + sh->impactPoint);
                        const double* in = (const double*)(hit + sh->impactNormal);
                        size_t i = (size_t)row * grid.w + col;
                        (*heights)[i] = (float)ip[2];
                        (*normalz)[i] = (float)in[2];
                        ++*hits;
                    }
                    clock_gettime(CLOCK_MONOTONIC, &b);
                    *usSpent += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000 + (b.tv_nsec - a.tv_nsec) / 1000);
                },
                30000);
            if (!ok) error = "game thread unavailable during the survey";
        }
        Guard g(P().lock);
        P().row = row + 1;
        P().hits = *hits;
        P().gameThreadUs = *usSpent;
        P().elapsedMs = NowMs() - P().startedMs;
    }

    std::string dir = PluginDataDir() + "/map";
    mkdir(dir.c_str(), 0755);
    if (error.empty() && !(WriteFloats(dir + "/heights.f32", *heights) && WriteFloats(dir + "/normalz.f32", *normalz)))
        error = "could not write " + dir;
    Guard g(P().lock);
    if (error.empty()) {
        std::string meta = "{\"minX\":" + JsonNum(grid.minX) + ",\"minY\":" + JsonNum(grid.minY) +
                           ",\"step\":" + JsonNum(grid.step) + ",\"w\":" + std::to_string(grid.w) +
                           ",\"h\":" + std::to_string(grid.h) + ",\"zTop\":" + JsonNum(zTop) +
                           ",\"zBottom\":" + JsonNum(zBottom) + ",\"channel\":" + std::to_string(channel) +
                           ",\"landscapes\":" + std::to_string(P().landscapes) +
                           ",\"hits\":" + std::to_string((uint64_t)*hits) +
                           ",\"ms\":" + std::to_string(NowMs() - P().startedMs) +
                           ",\"gameThreadUs\":" + std::to_string((uint64_t)*usSpent) +
                           ",\"gameBuild\":" + JsonStr(Reflect::GameBuild()) +
                           ",\"elfBuildId\":" + JsonStr(Elf().buildId) + "}";
        if (!WriteFileAtomic(dir + "/meta.json", meta)) error = "could not write meta.json";
    }
    P().running = false;
    P().done = error.empty();
    P().error = error;
    P().dir = dir;
    P().elapsedMs = NowMs() - P().startedMs;
    PluginLog("mapsurvey: %s (%dx%d, %llu hits, %llu ms game thread)", error.empty() ? "done" : error.c_str(),
              grid.w, grid.h, (unsigned long long)*hits, (unsigned long long)(*usSpent / 1000));
}

std::string StatusLocked() {
    Progress& p = P();
    return "{\"running\":" + std::string(p.running ? "true" : "false") + ",\"done\":" + (p.done ? "true" : "false") +
           ",\"error\":" + (p.error.empty() ? "null" : JsonStr(p.error)) + ",\"minX\":" + JsonNum(p.grid.minX) +
           ",\"minY\":" + JsonNum(p.grid.minY) + ",\"step\":" + JsonNum(p.grid.step) +
           ",\"w\":" + std::to_string(p.grid.w) + ",\"h\":" + std::to_string(p.grid.h) +
           ",\"zTop\":" + JsonNum(p.zTop) + ",\"zBottom\":" + JsonNum(p.zBottom) +
           ",\"channel\":" + std::to_string(p.channel) + ",\"landscapes\":" + std::to_string(p.landscapes) +
           ",\"row\":" + std::to_string(p.row) + ",\"hits\":" + std::to_string(p.hits) +
           ",\"elapsedMs\":" + std::to_string(p.elapsedMs) + ",\"gameThreadUs\":" + std::to_string(p.gameThreadUs) +
           ",\"dir\":" + JsonStr(p.dir) + "}";
}

}  // namespace

std::string Status() {
    Guard g(P().lock);
    return StatusLocked();
}

int Start(const JsonValue& body, std::string& outJson) {
    if (!Sym::Addr("UWorld::LineTraceSingleByChannel") || !Sym::Addr("FCollisionQueryParams::FCollisionQueryParams")) {
        outJson = "{\"error\":\"line trace symbols not resolved on this build\"}";
        return 501;
    }
    {
        Guard g(P().lock);
        if (P().running) {
            outJson = StatusLocked();
            return 409;
        }
    }
    double step = Num(body, "step", 400.0);
    int perJob = (int)Num(body, "perJob", 1024);
    if (perJob < 16) perJob = 16;
    if (perJob > 8192) perJob = 8192;
    int channel = (int)Num(body, "channel", 0);  // ECC_WorldStatic

    // Bounds: the request's box, else the union of every loaded landscape proxy.
    struct Box {
        double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        int count = 0;
    };
    auto box = std::make_shared<Box>();
    auto bounds = (FnActorBounds)(uintptr_t)Sym::Addr("AActor::GetActorBounds");
    bool ran = GameThread::Run(
        [box, bounds] {
            void* cls = Reflect::StaticClass("ALandscapeProxy::StaticClass");
            if (!cls || !bounds) return;
            std::vector<void*> proxies;
            Reflect::GetObjectsOfClass(cls, proxies, true);
            for (void* a : proxies) {
                if (Reflect::ObjName(a).rfind("Default__", 0) == 0) continue;
                double o[3] = {0, 0, 0}, e[3] = {0, 0, 0};
                bounds(a, false, o, e, false);
                if (!(e[0] > 0 && e[1] > 0) || !std::isfinite(o[0]) || !std::isfinite(o[1])) continue;
                double b[6] = {o[0] - e[0], o[0] + e[0], o[1] - e[1], o[1] + e[1], o[2] - e[2], o[2] + e[2]};
                if (box->count == 0) {
                    box->minX = b[0]; box->maxX = b[1]; box->minY = b[2]; box->maxY = b[3];
                    box->minZ = b[4]; box->maxZ = b[5];
                } else {
                    box->minX = std::min(box->minX, b[0]); box->maxX = std::max(box->maxX, b[1]);
                    box->minY = std::min(box->minY, b[2]); box->maxY = std::max(box->maxY, b[3]);
                    box->minZ = std::min(box->minZ, b[4]); box->maxZ = std::max(box->maxZ, b[5]);
                }
                box->count++;
            }
        },
        10000);
    if (!ran) {
        outJson = "{\"error\":\"game thread unavailable\"}";
        return 503;
    }
    double minX = Num(body, "minX", box->minX), maxX = Num(body, "maxX", box->maxX);
    double minY = Num(body, "minY", box->minY), maxY = Num(body, "maxY", box->maxY);
    double zTop = Num(body, "zTop", box->count ? box->maxZ + 10000 : 200000);
    double zBottom = Num(body, "zBottom", box->count ? box->minZ - 10000 : -200000);
    Grid grid;
    if (!PlanGrid(minX, maxX, minY, maxY, step, kMaxSide, grid)) {
        outJson = "{\"error\":\"no usable bounds (no landscape loaded and no minX/maxX/minY/maxY given), or more than "
                  "8192 samples per side\",\"landscapes\":" + std::to_string(box->count) + "}";
        return 400;
    }
    {
        Guard g(P().lock);
        Progress& p = P();
        p.running = true;
        p.done = false;
        p.error.clear();
        p.grid = grid;
        p.zTop = zTop;
        p.zBottom = zBottom;
        p.channel = channel;
        p.landscapes = box->count;
        p.row = 0;
        p.hits = 0;
        p.gameThreadUs = 0;
        p.startedMs = NowMs();
        p.elapsedMs = 0;
        outJson = StatusLocked();
    }
    PluginLog("mapsurvey: start %dx%d step %.0f from (%.0f, %.0f), %d landscape proxies", grid.w, grid.h, grid.step,
              grid.minX, grid.minY, box->count);
    std::thread(Run, grid, zTop, zBottom, channel, perJob).detach();
    return 202;
}

}  // namespace MapSurvey
