#include "conan/reads.h"

#include "common.h"
#include "conan/text.h"
#include "gamethread.h"
#include "takaro/json_util.h"
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
    "BasePlayerChar_C", "BaseBPChar_C", "ItemInventory", "GameItem", "DataTable", "KismetTextLibrary",
    "Default__KismetTextLibrary", "ItemTable", "ItemNameToTemplateID", "SpawnDataTable", "MapMarkers_ConanSandbox",
};

ActionResult Fail(const std::string& error) {
    ActionResult r;
    r.error = error;
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
    return true;
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
        snprintf(buf, sizeof buf, "ping: ExactPing@824=%.3f GetPingInMilliseconds=%.3f -> %s; ", fast, slow,
                 CheckName(pingCheck_));
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
                 "location: K2_GetActorLocation=(%.2f, %.2f, %.2f) ComponentToWorld@0x210=(%.2f, %.2f, %.2f) "
                 "relative=(%.2f, %.2f, %.2f) attached=%d -> %s; ",
                 engine[0], engine[1], engine[2], loc.world[0], loc.world[1], loc.world[2], loc.rel[0], loc.rel[1],
                 loc.rel[2], loc.attached ? 1 : 0, CheckName(locationCheck_));
        checkDetail_ += buf;
        NativeLog("reads: self-check %s", buf);
    }
}

// The item stat arrays (GameItem+0x128 / +0x138) against GetIntStat / GetFloatStat.
void ReadService::RunStatsCheck(const InvItem& item) {
    if (statsCheck_ != Check::Pending) return;
    PlayerReader reader(*r_, layout_);
    std::vector<std::pair<int, int32_t>> ints;
    std::vector<std::pair<int, float>> floats;
    bool decoded = false;
    int32_t stack = 0;
    float dur = -1, maxDur = -1;
    bool ran = o_.game.run && o_.game.run(
                                  [&] {
                                      const uint64_t t0 = NowNs();
                                      if (!r_->Alive(item.item)) return;
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
    if (!ran || !decoded) return;
    gameThreadJobs_++;
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
    if (!cat->HaveItems()) changed |= cat->BuildItems(*r_, slow, itemsErr);
    if (!cat->HaveEntities()) changed |= cat->BuildEntities(*r_, slow, entErr);
    if (!cat->HaveLocations()) {
        changed |= cat->BuildLocations(*r_, slow, Region(), locErr);
        // MapMarkers_ConanSandbox is loaded only once a player has joined: look for it again
        // with a fresh index scan, at most every 30 s.
        if (!cat->HaveLocations() && NowMs() - lastScanMs_ >= 30000) {
            lastScanMs_ = NowMs();
            if (r_->Scan(kScanNames) > 0) changed |= cat->BuildLocations(*r_, slow, Region(), locErr);
        }
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
    bool done = cat->HaveItems() && cat->HaveEntities() && cat->HaveLocations() && textCheck_ != Check::Pending;
    status = std::string("items ") + (cat->HaveItems() ? "ok" : itemsErr) + ", entities " +
             (cat->HaveEntities() ? "ok" : entErr) + ", locations " + (cat->HaveLocations() ? "ok" : locErr) +
             ", text check " + CheckName(textCheck_);
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
            if (p.steam64 == id) return &p;
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
    if (action == "listItems" || action == "listEntities" || action == "listLocations") {
        {
            std::lock_guard<std::mutex> g(mu_);
            calls_++;
        }
        if (!o_.warmupThread) {
            std::string status;
            WarmupStep(status);
        }
        ActionResult r = ListResult(action);
        if (!r.ok) {
            std::lock_guard<std::mutex> g(mu_);
            errors_++;
        }
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
        return Fail(action + " needs a player (gameId, steamId, platformId or name)");
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
        errors_++;
        return Fail("getPlayerLocation: player " + ids.front() + (p ? " has no character in the world right now" : " is not online"));
    }

    if (action == "getPlayerInventory") {
        if (!p) {
            errors_++;
            return Fail("getPlayerInventory: player " + ids.front() + " is not online");
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
        .N("calls", (double)calls_)
        .N("errors", (double)errors_)
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
