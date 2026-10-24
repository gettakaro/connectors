#include "conan/reads.h"

#include "common.h"
#include "conan/layout_probe.h"
#include "conan/text.h"
#include "gamethread.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"
#include "ue/ue.h"

#include <cmath>
#include <cstring>

namespace conan {

using takaro::ActionResult;
using takaro::ObjBuilder;

namespace {
constexpr int kJobTimeoutMs = 2000;
constexpr uint64_t kRescanMs = 10000;  // at most one object-array scan per 10 s

// Names the index scan collects (types, the text library CDO and the DataTables).
const std::vector<std::string> kScanNames = {
    "GameStateBase", "PlayerState", "Actor", "Controller", "SceneComponent", "ConanPlayerController",
    "BasePlayerChar_C", "BaseBPChar_C", "ItemInventory", "GameItem", "ShortcutRefItem", "DataTable", "KismetTextLibrary",
    "Default__KismetTextLibrary", "ItemTable", "ItemNameToTemplateID", "SpawnDataTable", "MapMarkers_ConanSandbox",
    "KismetSystemLibrary", "Default__KismetSystemLibrary",
};
const char* const kMarkerTablePath = "/Game/Systems/Map/MapMarkers_ConanSandbox.MapMarkers_ConanSandbox";
// Own loads of the marker table: at most 3 that the engine answered, 30 s apart; a load the game
// thread did not pick up in time (it can stall for seconds while the world finishes loading) is
// not counted and is tried again after 5 s.
constexpr int kMaxMarkerLoads = 3;
constexpr uint64_t kMarkerLoadGapMs = 30000, kMarkerRetryMs = 5000;
constexpr int kMarkerJobTimeoutMs = 10000;

ActionResult Fail(const std::string& error, const char* code = "") {
    ActionResult r;
    r.error = error;
    r.code = code;
    return r;
}

ActionResult Raw(const std::string& json) {
    ActionResult r;
    r.ok = takaro::ParseJson(json, r.payload);
    if (!r.ok) r.error = "internal error: could not encode the response";
    return r;
}

std::string Vec(double x, double y, double z) { return ObjBuilder().N("x", x).N("y", y).N("z", z).Done(); }

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
}  // namespace

const char* CheckName(Check c) {
    return c == Check::Passed ? "passed" : c == Check::Failed ? "failed (game-thread fallback)" : "pending";
}

ReadService::ReadService(ReadOptions o) : o_(std::move(o)) {
    r_.reset(new UE::Reflection(*o_.mem, o_.objObjects, o_.nameBlocks));
    if (o_.warmupThread) {
        warm_ = std::thread([this] {
            for (;;) {
                std::string status;
                bool done = WarmupStep(status);
                if (status != lastStatus_ || done) NativeLog("reads: warm-up %s: %s", done ? "done" : "waiting", status.c_str());
                lastStatus_ = status;
                if (done) return;
                std::unique_lock<std::mutex> l(stopMu_);
                if (stopCv_.wait_for(l, std::chrono::milliseconds(o_.warmupRetryMs), [this] { return stop_.load(); }))
                    return;
            }
        });
    }
}

ReadService::~ReadService() {
    {
        std::lock_guard<std::mutex> l(stopMu_);
        stop_ = true;
    }
    stopCv_.notify_all();
    if (warm_.joinable()) warm_.join();
}

bool ReadService::Handles(const std::string& a) {
    return a == "getPlayers" || a == "getPlayer" || a == "getPlayerLocation" || a == "getPlayerInventory" ||
           a == "listItems" || a == "listEntities" || a == "listLocations";
}

// ---------------------------------------------------------------- index and layout
bool ReadService::EnsureIndex(std::string& error) {
    if (indexed_) return true;
    uint64_t now = NowMs();
    if (lastScanMs_ && now - lastScanMs_ < kRescanMs) {
        error = indexError_;
        return false;
    }
    lastScanMs_ = now;
    const uint64_t t0 = NowNs();
    const uint64_t reads0 = o_.mem->ReadCount();
    int64_t n = r_->Scan(kScanNames);
    if (n < 0) {
        error = indexError_ = "the object array is not readable yet";
        return false;
    }
    PlayerLayout l;
    if (!l.Resolve(*r_, error)) {
        indexError_ = error;
        return false;
    }
    layout_ = l;
    UE::Reflection& r = *r_;
    uintptr_t ps = r.Type("PlayerState"), actor = r.Type("Actor"), item = r.Type("GameItem"),
              textLib = r.Type("KismetTextLibrary");
    fnPing_ = r.Function(ps, "GetPingInMilliseconds");
    fnLocation_ = r.Function(actor, "K2_GetActorLocation");
    fnIntStat_ = r.Function(item, "GetIntStat");
    fnFloatStat_ = r.Function(item, "GetFloatStat");
    fnTextToString_ = r.Function(textLib, "Conv_TextToString");
    for (uintptr_t cdo : r.Named("Default__KismetTextLibrary"))
        if (r.Alive(cdo) && r.mem().Rd<uintptr_t>(cdo + UE::Layout::kObjClass) == textLib) textLibCdo_ = cdo;
    pingRet_ = fnPing_.ok() ? r.Param(fnPing_.fn, "ReturnValue").offset : -1;
    locationRet_ = fnLocation_.ok() ? r.Param(fnLocation_.fn, "ReturnValue").offset : -1;
    intStatRet_ = fnIntStat_.ok() ? r.Param(fnIntStat_.fn, "ReturnValue").offset : -1;
    floatStatRet_ = fnFloatStat_.ok() ? r.Param(fnFloatStat_.fn, "ReturnValue").offset : -1;
    textRetOffset_ = fnTextToString_.ok() ? r.Param(fnTextToString_.fn, "ReturnValue").offset : -1;
    if (pingRet_ < 0 || locationRet_ < 0 || intStatRet_ < 0 || floatStatRet_ < 0 || textRetOffset_ < 0 || !textLibCdo_) {
        error = indexError_ = "the self-check getters were not found by reflection";
        return false;
    }
    indexed_ = true;
    indexError_.clear();
    NativeLog("reads: index of %lld objects in %.1f ms (%llu safe reads); layout: playerArray=%d owner=%d name=%d "
              "ip=%d userId=%d pawn=%d root=%d relLoc=%d backpack=%d hotbar=%d equipment=%d itemList=%d templateId=%d",
              (long long)n, (NowNs() - t0) / 1e6, (unsigned long long)(o_.mem->ReadCount() - reads0), l.playerArray,
              l.psOwner, l.psName, l.psAddress, l.pcUserId, l.pcPawn, l.rootComponent, l.relativeLocation, l.backpack,
              l.hotbar, l.equipment, l.itemList, l.templateId);
    return true;
}

bool ReadService::EnsureGameStates(std::string& error) {
    for (uintptr_t gs : gameStates_)
        if (r_->Alive(gs)) return true;
    uint64_t now = NowMs();
    if (lastStateScanMs_ && now - lastStateScanMs_ < 5000) {
        error = "no live GameState (world not loaded yet)";
        return false;
    }
    lastStateScanMs_ = now;
    gameStates_ = r_->Instances(layout_.gameStateBase, 8);
    if (gameStates_.empty()) {
        error = "no live GameState (world not loaded yet)";
        return false;
    }
    std::string region = Region();
    std::lock_guard<std::mutex> g(regionMu_);
    if (region != region_) NativeLog("reads: loaded map %s", region.empty() ? "unknown" : region.c_str());
    region_ = region;
    return true;
}

std::string ReadService::CurrentRegion() {
    std::lock_guard<std::mutex> g(regionMu_);
    return region_;
}

std::string ReadService::Region() {
    for (uintptr_t gs : gameStates_) {
        std::string p = r_->Path(gs);
        if (p.find("Siptah") != std::string::npos) return "IsleOfSiptah";
        if (p.find("/Game/Maps/ConanSandbox/ConanSandbox.") == 0) return "ExiledLands";
    }
    return "";
}

// ---------------------------------------------------------------- game-thread calls
bool ReadService::CallOnGame(uintptr_t obj, uintptr_t fn, uint16_t parmsSize,
                             const std::function<void(uint8_t*)>& before, const std::function<void(uint8_t*)>& after) {
    if (!o_.game.run || !o_.game.call || !fn || parmsSize > 512) return false;
    alignas(16) uint8_t parms[512];
    memset(parms, 0, sizeof parms);
    uint64_t ns = 0;
    bool ran = o_.game.run(
        [&] {
            const uint64_t t0 = NowNs();
            if (before) before(parms);
            o_.game.call(obj, fn, parms);
            if (after) after(parms);
            ns = NowNs() - t0;
        },
        kJobTimeoutMs);
    if (ran) {
        gameThreadJobs_++;
        gameThreadNs_ += ns;
    }
    return ran;
}

// Conv_TextToString(InText) on the text library CDO. The FText is copied bit for bit into the
// parameters (the native thunk takes it by const reference, so no refcount changes). The
// returned FString is allocated by the engine and deliberately leaked (a few per process).
bool ReadService::SlowText(uintptr_t ftext, std::string& out) {
    if (!fnTextToString_.ok() || !textLibCdo_ || textRetOffset_ < 0) return false;
    bool ok = false;
    std::string s;
    const UE::Mem& m = *o_.mem;
    bool ran = CallOnGame(
        textLibCdo_, fnTextToString_.fn, fnTextToString_.parmsSize,
        [&](uint8_t* p) {
            ok = m.Read(ftext, p, 16) && UE::Plausible(*(uintptr_t*)p);
            if (!ok) memset(p, 0, 16);
        },
        [&](uint8_t* p) {
            if (ok) ok = m.ReadFString((uintptr_t)(p + textRetOffset_), s);
        });
    if (!ran || !ok) return false;
    out = s;
    return true;
}

bool ReadService::SlowPing(uintptr_t ps, float& out) {
    float v = -1;
    bool ran = CallOnGame(ps, fnPing_.fn, fnPing_.parmsSize, nullptr,
                          [&](uint8_t* p) { memcpy(&v, p + pingRet_, 4); });
    out = v;
    return ran && v >= 0;
}

bool ReadService::SlowLocation(uintptr_t pawn, double out[3]) {
    return CallOnGame(pawn, fnLocation_.fn, fnLocation_.parmsSize, nullptr,
                      [&](uint8_t* p) { memcpy(out, p + locationRet_, 24); });
}

bool ReadService::SlowStats(uintptr_t item, InvItem& e) {
    int32_t stack = 0;
    float dur = -1, maxDur = -1;
    bool ok = o_.game.run && o_.game.run(
                                 [&] {
                                     const uint64_t t0 = NowNs();
                                     alignas(16) uint8_t p[16];
                                     memset(p, 0, sizeof p);
                                     p[0] = ItemStat::kStackSize;
                                     o_.game.call(item, fnIntStat_.fn, p);
                                     memcpy(&stack, p + intStatRet_, 4);
                                     memset(p, 0, sizeof p);
                                     p[0] = ItemStat::kDurability;
                                     o_.game.call(item, fnFloatStat_.fn, p);
                                     memcpy(&dur, p + floatStatRet_, 4);
                                     memset(p, 0, sizeof p);
                                     p[0] = ItemStat::kMaxDurability;
                                     o_.game.call(item, fnFloatStat_.fn, p);
                                     memcpy(&maxDur, p + floatStatRet_, 4);
                                     gameThreadNs_ += NowNs() - t0;
                                 },
                                 kJobTimeoutMs);
    if (!ok) return false;
    gameThreadJobs_++;
    e.stack = stack > 0 ? stack : 1;
    e.durability = maxDur > 0 ? dur : -1;
    e.maxDurability = maxDur > 0 ? maxDur : -1;
    return true;
}

bool ReadService::LoadMarkerTable(std::string& error, bool& pickedUp) {
    pickedUp = true;
    const UE::Reflection& r = *r_;
    uintptr_t sys = r.Type("KismetSystemLibrary"), cdo = 0;
    for (uintptr_t c : r.Named("Default__KismetSystemLibrary"))
        if (r.Alive(c) && r.mem().Rd<uintptr_t>(c + UE::Layout::kObjClass) == sys) cdo = c;
    UE::FuncInfo make = r.Function(sys, "MakeSoftObjectPath"), toRef = r.Function(sys, "Conv_SoftObjPathToSoftObjRef"),
                 load = r.Function(sys, "LoadAsset_Blocking");
    if (!sys || !cdo || !make.ok() || !toRef.ok() || !load.ok()) {
        pickedUp = false;  // not indexed yet: try again without using up an attempt
        error = "KismetSystemLibrary asset functions not found by reflection";
        return false;
    }
    UE::PropInfo pathIn = r.Param(make.fn, "PathString"), pathOut = r.Param(make.fn, "ReturnValue"),
                 refIn = r.Param(toRef.fn, "SoftObjectPath"), refOut = r.Param(toRef.fn, "ReturnValue"),
                 assetIn = r.Param(load.fn, "Asset"), objOut = r.Param(load.fn, "ReturnValue");
    // Types and sizes are checked here, so the bytes copied from one call into the next are
    // exactly one FSoftObjectPath and one FSoftObjectPtr.
    auto fits = [](const UE::PropInfo& p, const UE::FuncInfo& f, const char* type, int32_t size) {
        return p.ok() && p.type == type && p.size > 0 && (size < 0 || p.size == size) &&
               p.offset + p.size <= f.parmsSize && f.parmsSize <= 512;
    };
    if (!fits(pathIn, make, "StrProperty", 16) || !fits(pathOut, make, "StructProperty", -1) ||
        !fits(refIn, toRef, "StructProperty", pathOut.size) || !fits(refOut, toRef, "SoftObjectProperty", -1) ||
        !fits(assetIn, load, "SoftObjectProperty", refOut.size) || !fits(objOut, load, "ObjectProperty", 8)) {
        error = "KismetSystemLibrary asset function parameters do not match";
        return false;
    }
    std::u16string path(kMarkerTablePath, kMarkerTablePath + strlen(kMarkerTablePath));
    path.push_back(u'\0');
    uintptr_t table = 0;
    uint64_t ns = 0;
    bool ran = o_.game.run && o_.game.call && o_.game.run(
        [&] {
            const uint64_t t0 = NowNs();
            alignas(16) uint8_t a[512], b[512], c[512];
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            memset(c, 0, sizeof c);
            // FString {data, num incl. NUL, max}: the engine copies it and never frees our buffer.
            const char16_t* data = path.data();
            int32_t n = (int32_t)path.size();
            memcpy(a + pathIn.offset, &data, 8);
            memcpy(a + pathIn.offset + 8, &n, 4);
            memcpy(a + pathIn.offset + 12, &n, 4);
            o_.game.call(cdo, make.fn, a);
            memcpy(b + refIn.offset, a + pathOut.offset, (size_t)pathOut.size);
            o_.game.call(cdo, toRef.fn, b);
            memcpy(c + assetIn.offset, b + refOut.offset, (size_t)refOut.size);
            o_.game.call(cdo, load.fn, c);
            memcpy(&table, c + objOut.offset, 8);
            ns = NowNs() - t0;
        },
        kMarkerJobTimeoutMs);
    if (!ran) {
        pickedUp = false;
        error = "the game thread did not pick up the load within " + std::to_string(kMarkerJobTimeoutMs / 1000) + " s";
        return false;
    }
    gameThreadJobs_++;
    gameThreadNs_ += ns;
    NativeLog("reads: loaded %s on the game thread without a player (%.1f ms) -> %s", kMarkerTablePath, ns / 1e6,
              table ? "ok" : "null");
    if (!table) {
        error = "LoadAsset_Blocking returned no table";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- startup self-checks
// ExactPing (PlayerState+824) against GetPingInMilliseconds, and RelativeLocation /
// ComponentToWorld against K2_GetActorLocation, read inside one game-thread job so both sides see
// the same frame.
void ReadService::RunPlayerChecks(const PlayerRecord& p) {
    if (pingCheck_ != Check::Pending && (locationCheck_ != Check::Pending || !p.pawn)) return;
    const UE::Mem& m = *o_.mem;
    PlayerReader reader(*r_, layout_);
    float slow = -1, fast = -1;
    double engine[3] = {0, 0, 0};
    Location loc;
    std::string err;
    bool locOk = false, pingRan = false, locRan = false;
    bool ran = o_.game.run && o_.game.run(
                                  [&] {
                                      const uint64_t t0 = NowNs();
                                      alignas(16) uint8_t parms[64];
                                      if (pingCheck_ == Check::Pending && r_->Alive(p.ps)) {
                                          memset(parms, 0, sizeof parms);
                                          o_.game.call(p.ps, fnPing_.fn, parms);
                                          memcpy(&slow, parms + pingRet_, 4);
                                          m.Get(p.ps + PlayerLayout::kExactPing, fast);
                                          pingRan = true;
                                      }
                                      if (locationCheck_ == Check::Pending && p.pawn && r_->Alive(p.pawn)) {
                                          memset(parms, 0, sizeof parms);
                                          o_.game.call(p.pawn, fnLocation_.fn, parms);
                                          memcpy(engine, parms + locationRet_, 24);
                                          locOk = reader.Location(p.pawn, loc, err);
                                          locRan = true;
                                      }
                                      gameThreadNs_ += NowNs() - t0;
                                  },
                                  kJobTimeoutMs);
    if (!ran) return;
    gameThreadJobs_++;
    char buf[256];
    if (pingRan) {
        pingCheck_ = Near(slow, fast, 0.01) ? Check::Passed : Check::Failed;
        snprintf(buf, sizeof buf, "ping: ExactPing@%d=%.3f GetPingInMilliseconds=%.3f -> %s; ",
                 (int)PlayerLayout::kExactPing, fast, slow, CheckName(pingCheck_));
        checkDetail_ += buf;
        NativeLog("reads: self-check %s", buf);
    }
    if (locRan) {
        bool worldOk = locOk && Near(loc.world[0], engine[0], 1) && Near(loc.world[1], engine[1], 1) &&
                       Near(loc.world[2], engine[2], 1);
        bool relOk = locOk && (loc.attached || (Near(loc.rel[0], engine[0], 1) && Near(loc.rel[1], engine[1], 1) &&
                                                Near(loc.rel[2], engine[2], 1)));
        locationCheck_ = worldOk && relOk ? Check::Passed : Check::Failed;
        snprintf(buf, sizeof buf,
                 "location: K2_GetActorLocation=(%.2f, %.2f, %.2f) ComponentToWorld@0x%x=(%.2f, %.2f, %.2f) "
                 "relative=(%.2f, %.2f, %.2f) attached=%d -> %s; ",
                 engine[0], engine[1], engine[2], (unsigned)PlayerLayout::kComponentToWorldT, loc.world[0],
                 loc.world[1], loc.world[2], loc.rel[0], loc.rel[1], loc.rel[2], loc.attached ? 1 : 0,
                 CheckName(locationCheck_));
        checkDetail_ += buf;
        NativeLog("reads: self-check %s", buf);
        if (locationCheck_ == Check::Failed && locOk) {
            uintptr_t root = m.Rd<uintptr_t>(p.pawn + (uintptr_t)layout_.rootComponent);
            NativeLog("reads: layout probe: K2_GetActorLocation found at root %s (ComponentToWorld translation pinned "
                      "at +0x%x)",
                      ProbeVector(m, root, engine).c_str(), (unsigned)PlayerLayout::kComponentToWorldT);
        }
    }
}

// The item stat arrays (GameItem+kIntStats / +kFloatStats) against GetIntStat / GetFloatStat.
void ReadService::RunStatsCheck(const InvItem& item) {
    if (statsCheck_ != Check::Pending) return;
    PlayerReader reader(*r_, layout_);
    std::vector<std::pair<int, int32_t>> ints;
    std::vector<std::pair<int, float>> floats;
    bool decoded = false, alive = false;
    int32_t stack = 0;
    float dur = -1, maxDur = -1;
    bool ran = o_.game.run && o_.game.run(
                                  [&] {
                                      const uint64_t t0 = NowNs();
                                      if (!r_->Alive(item.item)) return;
                                      alive = true;
                                      decoded = reader.Stats(item.item, ints, floats);
                                      alignas(16) uint8_t p[16];
                                      memset(p, 0, sizeof p);
                                      p[0] = ItemStat::kStackSize;
                                      o_.game.call(item.item, fnIntStat_.fn, p);
                                      memcpy(&stack, p + intStatRet_, 4);
                                      memset(p, 0, sizeof p);
                                      p[0] = ItemStat::kDurability;
                                      o_.game.call(item.item, fnFloatStat_.fn, p);
                                      memcpy(&dur, p + floatStatRet_, 4);
                                      memset(p, 0, sizeof p);
                                      p[0] = ItemStat::kMaxDurability;
                                      o_.game.call(item.item, fnFloatStat_.fn, p);
                                      memcpy(&maxDur, p + floatStatRet_, 4);
                                      gameThreadNs_ += NowNs() - t0;
                                  },
                                  kJobTimeoutMs);
    if (!ran || !alive) return;
    gameThreadJobs_++;
    if (!decoded) {
        // The pinned stat arrays did not decode: say where they are, once, and use the getters.
        statsCheck_ = Check::Failed;
        NativeLog("reads: self-check stats: template %d: the pinned stat arrays (+0x%x/+0x%x) did not decode -> %s",
                  item.templateId, (unsigned)PlayerLayout::kIntStats, (unsigned)PlayerLayout::kFloatStats,
                  CheckName(statsCheck_));
        NativeLog("reads: layout probe: stats %s", ProbeStats(*o_.mem, item.item, stack, dur, maxDur).c_str());
        return;
    }
    int32_t wStack = 1;
    float wDur = 0, wMax = 0;  // a missing float stat reads as 0 through GetFloatStat
    for (auto& kv : ints)
        if (kv.first == ItemStat::kStackSize) wStack = kv.second;
    for (auto& kv : floats) {
        if (kv.first == ItemStat::kDurability) wDur = kv.second;
        if (kv.first == ItemStat::kMaxDurability) wMax = kv.second;
    }
    statsCheck_ = (wStack == stack && Near(wDur, dur, 0.001) && Near(wMax, maxDur, 0.001)) ? Check::Passed : Check::Failed;
    char buf[256];
    snprintf(buf, sizeof buf,
             "stats: template %d worker stack=%d dur=%.2f/%.2f, GetIntStat/GetFloatStat stack=%d dur=%.2f/%.2f -> %s; ",
             item.templateId, wStack, wDur, wMax, stack, dur, maxDur, CheckName(statsCheck_));
    checkDetail_ += buf;
    NativeLog("reads: self-check %s", buf);
    if (statsCheck_ == Check::Failed)
        NativeLog("reads: layout probe: stats %s", ProbeStats(*o_.mem, item.item, stack, dur, maxDur).c_str());
}

// The worker FText decode against Conv_TextToString for the sampled rows (both vtables).
void ReadService::RunTextCheck() {
    std::shared_ptr<const Catalogue> cat;
    {
        std::lock_guard<std::mutex> l(catMu_);
        cat = catalogue_;
    }
    if (!cat || textCheck_ != Check::Pending || cat->TextSamples().empty()) return;
    int ok = 0, bad = 0;
    std::string firstBad;
    for (auto& s : cat->TextSamples()) {
        std::string engine;
        if (!SlowText(s.first, engine)) return;  // game thread busy: try again next step
        if (engine == s.second) ok++;
        else if (bad++ == 0) firstBad = "'" + s.second + "' vs engine '" + engine + "'";
    }
    textCheck_ = bad == 0 ? Check::Passed : Check::Failed;
    std::string line = "text: " + std::to_string(ok) + "/" + std::to_string(ok + bad) +
                       " sampled rows equal Conv_TextToString" + (bad ? " (first mismatch " + firstBad + ")" : "") +
                       " -> " + CheckName(textCheck_) + "; ";
    checkDetail_ += line;
    NativeLog("reads: self-check %s", line.c_str());
}

// ---------------------------------------------------------------- warm-up
bool ReadService::WarmupStep(std::string& status) {
    std::lock_guard<std::mutex> g(mu_);
    std::string err;
    if (!EnsureIndex(err)) {
        status = "index: " + err;
        return false;
    }
    if (!EnsureGameStates(err)) {
        status = err;
        return false;
    }
    std::shared_ptr<const Catalogue> old;
    {
        std::lock_guard<std::mutex> l(catMu_);
        old = catalogue_;
    }
    std::shared_ptr<Catalogue> cat = old ? std::make_shared<Catalogue>(*old) : std::make_shared<Catalogue>();
    SlowTextFn slow = [this](uintptr_t a, std::string& o) { return SlowText(a, o); };
    const uint64_t reads0 = o_.mem->ReadCount();
    bool changed = false;
    std::string itemsErr, entErr, locErr;
    // Builds all three catalogues again, so texts that were not loaded during the early build
    // (start-up) are picked up; keeps the old catalogue when a table is gone.
    auto rebuild = [&] {
        auto fresh = std::make_shared<Catalogue>();
        std::string e1, e2, e3;
        if (!fresh->BuildItems(*r_, slow, e1) || !fresh->BuildEntities(*r_, slow, e2) ||
            !fresh->BuildLocations(*r_, slow, Region(), e3))
            return false;
        cat = fresh;
        changed = true;
        return true;
    };
    // Locations first: they are cheap, and Takaro asks for them right after connecting.
    bool locationsNew = false;
    if (!cat->HaveLocations()) {
        locationsNew = cat->BuildLocations(*r_, slow, Region(), locErr);
        // The game loads MapMarkers_ConanSandbox only once a player has joined: look for it again
        // with a fresh index scan, at most every 30 s.
        if (!locationsNew && NowMs() - lastScanMs_ >= 30000) {
            lastScanMs_ = NowMs();
            if (r_->Scan(kScanNames) > 0) locationsNew = cat->BuildLocations(*r_, slow, Region(), locErr);
        }
        // Without a player, load it ourselves once the world is up, so Takaro's location sync
        // right after connecting gets an answer.
        if (!locationsNew && markerLoads_ < kMaxMarkerLoads && NowMs() >= nextMarkerLoadMs_) {
            std::string loadErr;
            bool picked = true;
            bool loaded = LoadMarkerTable(loadErr, picked);
            if (picked) markerLoads_++;
            nextMarkerLoadMs_ = NowMs() + (picked ? kMarkerLoadGapMs : kMarkerRetryMs);
            if (!loaded) NativeLog("reads: marker table load %d failed: %s", markerLoads_ + (picked ? 0 : 1), loadErr.c_str());
            else if (r_->Scan(kScanNames) > 0 && cat->BuildLocations(*r_, slow, Region(), locErr)) {
                locationsNew = true;
                markersForced_ = true;
                refreshAfterJoin_ = true;  // nobody has joined yet: rebuild everything after the first join
            }
        }
        if (locationsNew) {
            changed = true;
            {
                std::lock_guard<std::mutex> l(catMu_);
                locationsJson_ = cat->LocationsJson();
            }
            catCv_.notify_all();
        }
    }
    if (!cat->HaveItems()) changed |= cat->BuildItems(*r_, slow, itemsErr);
    if (!cat->HaveEntities()) changed |= cat->BuildEntities(*r_, slow, entErr);
    // The game loaded the marker table (a player has joined) and the world is fully up.
    if (locationsNew && !markersForced_ && cat->HaveItems() && cat->HaveEntities()) rebuild();
    if (refreshAfterJoin_ && firstPlayerMs_ && NowMs() - firstPlayerMs_ >= (uint64_t)o_.refreshAfterJoinMs &&
        NowMs() >= nextRefreshMs_) {
        // A table that is gone (our own marker load can be collected) is tried again in 60 s, five times.
        if (rebuild() || ++refreshTries_ >= 5) refreshAfterJoin_ = false;
        else nextRefreshMs_ = NowMs() + 60000;
    }
    if (changed) {
        const CatalogueStats& s = cat->Stats();
        NativeLog("reads: catalogue items %zu listed of %zu rows (%zu codes, %.1f ms), entities %zu of %zu (%.1f ms), "
                  "locations %zu of %zu (%.1f ms, region %s); text plain=%zu stringTable=%zu slow=%zu missing=%zu; "
                  "%llu safe reads",
                  s.itemsListed, s.itemRows, s.itemCodes, s.itemsMs, s.entities, s.entityRows, s.entitiesMs, s.locations,
                  s.markerRows, s.locationsMs, Region().c_str(), s.textPlain, s.textStringTable, s.textSlow,
                  s.textMissing, (unsigned long long)(o_.mem->ReadCount() - reads0));
        std::lock_guard<std::mutex> l(catMu_);
        catalogue_ = cat;
        if (cat->HaveItems()) itemsJson_ = cat->ItemsJson();
        if (cat->HaveEntities()) entitiesJson_ = cat->EntitiesJson();
        if (cat->HaveLocations()) locationsJson_ = cat->LocationsJson();
    }
    if (cat->HaveItems()) RunTextCheck();
    if (textCheck_ == Check::Failed && !textRebuilt_) {
        textRebuilt_ = true;
        // The worker FText decode is wrong on this process: rebuild every name on the game thread.
        auto slowCat = std::make_shared<Catalogue>();
        slowCat->ForceSlowText(true);
        slowCat->BuildItems(*r_, slow, itemsErr);
        slowCat->BuildEntities(*r_, slow, entErr);
        slowCat->BuildLocations(*r_, slow, Region(), locErr);
        NativeLog("reads: catalogue rebuilt through Conv_TextToString (%zu items, %zu entities, %zu locations)",
                  slowCat->Stats().itemsListed, slowCat->Stats().entities, slowCat->Stats().locations);
        std::lock_guard<std::mutex> l(catMu_);
        catalogue_ = slowCat;
        itemsJson_ = slowCat->HaveItems() ? slowCat->ItemsJson() : "";
        entitiesJson_ = slowCat->HaveEntities() ? slowCat->EntitiesJson() : "";
        locationsJson_ = slowCat->HaveLocations() ? slowCat->LocationsJson() : "";
        cat = slowCat;
    }
    bool done = cat->HaveItems() && cat->HaveEntities() && cat->HaveLocations() && textCheck_ != Check::Pending &&
                !refreshAfterJoin_;
    status = std::string("items ") + (cat->HaveItems() ? "ok" : itemsErr) + ", entities " +
             (cat->HaveEntities() ? "ok" : entErr) + ", locations " +
             (cat->HaveLocations() ? (markersForced_ ? "ok (table loaded without a player)" : "ok") : locErr) +
             ", text check " + CheckName(textCheck_) + (refreshAfterJoin_ ? ", rebuild after the first join pending" : "");
    {
        std::lock_guard<std::mutex> l(catMu_);
        catalogueStatus_ = status;
        catalogueDone_ = done;
    }
    catCv_.notify_all();
    return done;
}

int32_t ReadService::ResolveItemCode(const std::string& code) {
    std::lock_guard<std::mutex> l(catMu_);
    return catalogue_ && catalogue_->HaveItems() ? catalogue_->ResolveCode(code) : 0;
}

std::string ReadService::KnownName(const std::string& steam64) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = known_.find(steam64);
    if (it == known_.end()) return "";
    JsonValue v;
    if (!takaro::ParseJson(it->second.json, v) || v.type != JsonValue::Object) return "";
    const JsonValue* n = v.get("name");
    return n && n->type == JsonValue::String ? n->str : "";
}

std::string ReadService::EntityNameForClass(const std::string& npcClass) {
    std::lock_guard<std::mutex> l(catMu_);
    if (!catalogue_ || !catalogue_->HaveEntities() || npcClass.empty()) return "";
    return catalogue_->EntityNameForClass(npcClass);
}

// ---------------------------------------------------------------- players
bool ReadService::Snapshot(std::vector<PlayerRecord>& out, std::string& error) {
    if (!EnsureIndex(error) || !EnsureGameStates(error)) return false;
    const uint64_t t0 = NowNs();
    const uint64_t reads0 = o_.mem->ReadCount();
    PlayerReader reader(*r_, layout_);
    bool ok = reader.Players(gameStates_, out, error);
    if (!ok && error == "no live GameState") {
        gameStates_.clear();
        lastStateScanMs_ = 0;
        ok = EnsureGameStates(error) && reader.Players(gameStates_, out, error);
    }
    lastSnapshotNs_ = NowNs() - t0;
    snapshotReads_ = o_.mem->ReadCount() - reads0;
    if (!ok) return false;
    if (!out.empty() && !firstPlayerMs_) firstPlayerMs_ = NowMs();
    for (auto& p : out) {
        RunPlayerChecks(p);
        if (pingCheck_ == Check::Failed) SlowPing(p.ps, p.ping);
    }
    return true;
}

std::string ReadService::PlayerJson(const PlayerRecord& p) {
    ObjBuilder b;
    std::string name = !p.name.empty() ? p.name : !p.characterName.empty() ? p.characterName : p.steam64;
    b.S("gameId", p.steam64).S("name", name);
    if (IsSteam64(p.steam64)) b.S("steamId", p.steam64).S("platformId", "steam:" + p.steam64);
    if (!p.ip.empty()) b.S("ip", p.ip);
    // Only once the ExactPing offset is proven (or through the engine getter after a failure).
    if (p.ping >= 0 && pingCheck_ != Check::Pending) b.N("ping", (double)std::lround(p.ping));
    return b.Done();
}

std::vector<std::string> ReadService::Candidates(const JsonValue& args) {
    std::vector<std::string> out;
    const JsonValue& player = takaro::AsRecord(args.get("player"));
    for (const JsonValue* rec : {&args, &player})
        for (const char* k : {"gameId", "steamId", "platformId", "playerId", "name"})
            if (auto v = takaro::Str(rec->get(k))) out.push_back(NormalizeRecipient(*v));
    return out;
}

const PlayerRecord* ReadService::Match(const std::vector<PlayerRecord>& players, const std::vector<std::string>& ids) {
    for (auto& id : ids)
        for (auto& p : players)
            if (p.steam64 == id || (!p.urlId.empty() && p.urlId == id) || (!p.uniqueId.empty() && p.uniqueId == id)) return &p;
    for (auto& id : ids)
        for (auto& p : players)
            if (EqualsIgnoreCase(p.name, id) || EqualsIgnoreCase(p.characterName, id)) return &p;
    return nullptr;
}

// ---------------------------------------------------------------- lists
ActionResult ReadService::ListResult(const std::string& action) {
    std::unique_lock<std::mutex> l(catMu_);
    auto have = [&] {
        if (action == "listItems") return !itemsJson_.empty();
        if (action == "listEntities") return !entitiesJson_.empty();
        return !locationsJson_.empty();
    };
    if (!have()) catCv_.wait_for(l, std::chrono::milliseconds(o_.listWaitMs), have);
    if (!have()) return Fail(action + ": the Conan catalogue is still being built (" + catalogueStatus_ + "); try again shortly");
    return Raw(action == "listItems" ? itemsJson_ : action == "listEntities" ? entitiesJson_ : locationsJson_);
}

// ---------------------------------------------------------------- dispatch
ActionResult ReadService::Execute(const std::string& action, const JsonValue& args) {
    const uint64_t t0 = NowNs();
    ActionResult r = ExecuteImpl(action, args);
    // One line per read request: the evidence trail of what Takaro was answered.
    std::string body = r.ok ? takaro::JsonDump(r.payload) : r.error;
    std::string rows = r.ok && r.payload.type == JsonValue::Array ? " " + std::to_string(r.payload.arr.size()) + " rows" : "";
    NativeLog("reads: %s %s -> %s%s (%.2f ms): %.400s", action.c_str(), takaro::JsonDump(args).substr(0, 120).c_str(),
              r.ok ? "ok" : "ERROR", rows.c_str(), (NowNs() - t0) / 1e6, body.c_str());
    return r;
}

ActionResult ReadService::ExecuteImpl(const std::string& action, const JsonValue& args) {
    if (action == "listItems" || action == "listEntities" || action == "listLocations") {
        // No mu_ here: a list action must not wait behind a catalogue build in progress.
        calls_++;
        if (!o_.warmupThread) {
            std::string status;
            WarmupStep(status);
        }
        ActionResult r = ListResult(action);
        if (!r.ok) errors_++;
        return r;
    }
    std::lock_guard<std::mutex> g(mu_);
    calls_++;
    std::vector<PlayerRecord> players;
    std::string error;
    if (!Snapshot(players, error)) {
        errors_++;
        if (action == "getPlayer") {
            // Takaro rejects every "no player" answer to getPlayer; fall through to the records below.
            players.clear();
        } else {
            return Fail(action + ": " + error);
        }
    }
    for (auto& p : players) known_[p.steam64].json = PlayerJson(p);

    if (action == "getPlayers") {
        std::string o = "[";
        for (size_t i = 0; i < players.size(); i++) o += (i ? "," : "") + PlayerJson(players[i]);
        return Raw(o + "]");
    }

    std::vector<std::string> ids = Candidates(args);
    if (ids.empty()) {
        errors_++;
        return Fail(action + " needs a player (gameId, steamId, platformId or name)", takaro::kInvalidArgs);
    }
    const PlayerRecord* p = Match(players, ids);

    if (action == "getPlayer") {
        if (p) return Raw(PlayerJson(*p));
        for (auto& id : ids) {
            auto it = known_.find(id);
            if (it != known_.end()) return Raw(it->second.json);
        }
        // Never seen: a minimal record from the requested id (Takaro needs a valid IGamePlayer).
        const std::string& id = ids.front();
        ObjBuilder b;
        b.S("gameId", id).S("name", id);
        if (IsSteam64(id)) b.S("steamId", id).S("platformId", "steam:" + id);
        return Raw(b.Done());
    }

    if (action == "getPlayerLocation") {
        if (p && p->pawn) {
            double v[3];
            if (locationCheck_ == Check::Failed) {
                if (!SlowLocation(p->pawn, v)) return Fail("getPlayerLocation: the game thread did not respond");
            } else {
                Location loc;
                PlayerReader reader(*r_, layout_);
                if (!reader.Location(p->pawn, loc, error)) {
                    errors_++;
                    return Fail("getPlayerLocation: " + error);
                }
                v[0] = loc.x;
                v[1] = loc.y;
                v[2] = loc.z;
            }
            Known& k = known_[p->steam64];
            k.haveLocation = true;
            memcpy(k.loc, v, sizeof v);
            return Raw(Vec(v[0], v[1], v[2]));
        }
        // Offline or without a character (dead, loading): the last position we saw. Takaro asks
        // for it right after a connect/disconnect and drops the event if this fails.
        for (auto& id : ids) {
            auto it = known_.find(id);
            if (it != known_.end() && it->second.haveLocation)
                return Raw(Vec(it->second.loc[0], it->second.loc[1], it->second.loc[2]));
        }
        // Online without a character and never seen before (a new character's intro cinematic:
        // the controller has no pawn yet): the controller's own root follows the view. Takaro
        // asks for the location right after player-connected and drops the event on an error.
        if (p && p->pc) {
            Location loc;
            PlayerReader reader(*r_, layout_);
            if (reader.Location(p->pc, loc, error)) return Raw(Vec(loc.x, loc.y, loc.z));
        }
        errors_++;
        return Fail("getPlayerLocation: player " + ids.front() + (p ? " has no character in the world right now" : " is not online"),
                    takaro::kNotFound);
    }

    if (action == "getPlayerInventory") {
        if (!p) {
            errors_++;
            return Fail("getPlayerInventory: player " + ids.front() + " is not online", takaro::kNotFound);
        }
        if (!p->pawn) {
            errors_++;
            return Fail("getPlayerInventory: player " + ids.front() + " has no character in the world right now");
        }
        PlayerReader reader(*r_, layout_);
        std::vector<InvItem> items;
        if (!reader.Inventory(p->pawn, items, error)) {
            // Torn on the worker in every attempt: copy the lists inside one game-thread job, where
            // nothing can change while we read.
            bool gtOk = false;
            bool ran = o_.game.run && o_.game.run(
                                          [&] {
                                              const uint64_t t0 = NowNs();
                                              gtOk = reader.Inventory(p->pawn, items, error);
                                              gameThreadNs_ += NowNs() - t0;
                                          },
                                          kJobTimeoutMs);
            if (ran) gameThreadJobs_++;
            if (!ran || !gtOk) {
                errors_++;
                return Fail("getPlayerInventory: " + (ran ? error : std::string("the game thread did not respond")));
            }
        }
        std::shared_ptr<const Catalogue> cat;
        {
            std::lock_guard<std::mutex> l(catMu_);
            cat = catalogue_;
        }
        if (!cat || !cat->HaveItems()) {
            errors_++;
            return Fail("getPlayerInventory: the item catalogue is still being built; try again shortly");
        }
        // One row per item code and durability, amounts summed (internal fist templates dropped).
        struct Row {
            std::string code, name, quality;
            double amount;
        };
        std::vector<Row> rows;
        for (auto& e : items) {
            if (IsInternalTemplate(e.templateId)) continue;
            if (statsCheck_ == Check::Pending) RunStatsCheck(e);
            if (statsCheck_ == Check::Failed) SlowStats(e.item, e);
            std::string quality;
            if (e.maxDurability > 0 && e.durability >= 0)
                quality = std::to_string((int)std::lround(100.0 * e.durability / e.maxDurability));
            std::string code = cat->CodeFor(e.templateId);
            bool merged = false;
            for (auto& r : rows)
                if (r.code == code && r.quality == quality) {
                    r.amount += e.stack;
                    merged = true;
                }
            if (!merged) rows.push_back({code, cat->DisplayName(e.templateId), quality, (double)e.stack});
        }
        std::string o = "[";
        for (size_t i = 0; i < rows.size(); i++) {
            ObjBuilder b;
            b.S("code", rows[i].code).S("name", rows[i].name).N("amount", rows[i].amount);
            if (!rows[i].quality.empty()) b.S("quality", rows[i].quality);
            o += (i ? "," : "") + b.Done();
        }
        return Raw(o + "]");
    }
    return Fail(action + " is not a read action");
}

std::string ReadService::HealthJson() {
    // Never wait on a read in progress (it may be waiting for the game thread): reuse the last one.
    std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
    if (!g.owns_lock()) {
        std::lock_guard<std::mutex> l(catMu_);
        return lastHealth_.empty() ? std::string("null") : lastHealth_;
    }
    std::string cat;
    {
        std::lock_guard<std::mutex> l(catMu_);
        if (catalogue_) {
            const CatalogueStats& s = catalogue_->Stats();
            cat = ObjBuilder()
                      .N("items", (double)s.itemsListed)
                      .N("itemRows", (double)s.itemRows)
                      .N("entities", (double)s.entities)
                      .N("locations", (double)s.locations)
                      .N("textSlow", (double)s.textSlow)
                      .N("textMissing", (double)s.textMissing)
                      .N("itemsMs", s.itemsMs)
                      .Done();
        }
        cat = ObjBuilder().S("status", catalogueStatus_).B("done", catalogueDone_).Raw("stats", cat.empty() ? "null" : cat).Done();
    }
    std::string h = ObjBuilder()
        .B("indexed", indexed_)
        .S("indexError", indexError_)
        .Raw("catalogue", cat)
        .S("pingCheck", CheckName(pingCheck_))
        .S("locationCheck", CheckName(locationCheck_))
        .S("statsCheck", CheckName(statsCheck_))
        .S("textCheck", CheckName(textCheck_))
        .S("checks", checkDetail_)
        .N("calls", (double)calls_.load())
        .N("errors", (double)errors_.load())
        .N("gameThreadJobs", (double)gameThreadJobs_)
        .N("gameThreadMs", gameThreadNs_ / 1e6)
        .N("lastSnapshotUs", lastSnapshotNs_ / 1e3)
        .N("lastSnapshotReads", (double)snapshotReads_)
        .N("safeReads", (double)o_.mem->ReadCount())
        .Done();
    std::lock_guard<std::mutex> l(catMu_);
    lastHealth_ = h;
    return h;
}

ReadService* ProductionReads() {
    static std::mutex m;
    static ReadService* s = nullptr;  // leaked: action workers may still run at exit
    std::lock_guard<std::mutex> l(m);
    if (s) return s;
    if (!UE::HaveGlobals()) return nullptr;
    ReadOptions o;
    o.mem = &UE::SelfMem();
    o.objObjects = UE::ObjObjectsAddr();
    o.nameBlocks = UE::NameBlocksAddr();
    o.game.run = [](std::function<void()> fn, int timeoutMs) { return GameThread::Run(std::move(fn), timeoutMs); };
    o.game.call = [](uintptr_t obj, uintptr_t fn, void* parms) { UE::CallProcessEvent((void*)obj, (void*)fn, parms); };
    s = new ReadService(o);
    return s;
}

}  // namespace conan
