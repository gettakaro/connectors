// Lane L2a: the read actions of the native Conan connector.
//
//   getPlayers, getPlayer        PlayerArray -> PlayerState -> controller (Steam64, name, IP, ping)
//   getPlayerLocation            pawn -> RootComponent -> RelativeLocation (ComponentToWorld when attached)
//   getPlayerInventory           backpack, hotbar and equipment ItemInventory -> GameItem
//   listItems / listEntities / listLocations   the catalogues (conan/catalogue.h), built once
//
// Everything is read on worker threads through UE::Mem (process_vm_readv on our own pid), with
// torn-read checks. The game thread is used only for one-off startup self-checks of the
// non-reflected offsets (ExactPing, ComponentToWorld, the item stat arrays, the FText
// internals) against the engine's own getters, and as the fallback for a cell whose check
// failed. Method and offsets: El-Limon context/games/conan-exiles/evidence/spikes/S3-data.md.
#pragma once

#include "conan/catalogue.h"
#include "conan/player_snapshot.h"
#include "takaro/game.h"
#include "ue/reflect.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace conan {

struct GameCalls {
    // Runs `fn` on the game thread; false when it did not run within `timeoutMs`.
    std::function<bool(std::function<void()> fn, int timeoutMs)> run;
    // Game thread only: ProcessEvent(obj, fn, parms) without our detour.
    std::function<void(uintptr_t obj, uintptr_t fn, void* parms)> call;
};

struct ReadOptions {
    const UE::Mem* mem = nullptr;
    uintptr_t objObjects = 0, nameBlocks = 0;
    GameCalls game;
    bool warmupThread = true;  // build the catalogues in the background after start
    int warmupRetryMs = 3000;
    int listWaitMs = 8000;  // a list action waits this long for a catalogue still building
};

enum class Check { Pending, Passed, Failed };
const char* CheckName(Check c);

class ReadService {
public:
    explicit ReadService(ReadOptions o);
    ~ReadService();
    ReadService(const ReadService&) = delete;
    ReadService& operator=(const ReadService&) = delete;

    static bool Handles(const std::string& action);
    // Action worker thread. Always produces a result (ok payload or a structured error).
    takaro::ActionResult Execute(const std::string& action, const JsonValue& args);
    std::string HealthJson();

    // One warm-up step (index scan, layout, catalogues, text self-check). True once everything
    // is built. The background thread calls it until it returns true; tests call it directly.
    bool WarmupStep(std::string& status);

    // giveItem (lane L2b) can resolve item codes once the item catalogue is built.
    int32_t ResolveItemCode(const std::string& code);

private:
    bool EnsureIndex(std::string& error);
    bool EnsureGameStates(std::string& error);
    bool Snapshot(std::vector<PlayerRecord>& out, std::string& error);
    void RunPlayerChecks(const PlayerRecord& p);
    void RunStatsCheck(const InvItem& item);
    void RunTextCheck();
    bool CallOnGame(uintptr_t obj, uintptr_t fn, uint16_t parmsSize, const std::function<void(uint8_t*)>& before,
                    const std::function<void(uint8_t*)>& after);
    bool SlowText(uintptr_t ftext, std::string& out);
    bool SlowPing(uintptr_t ps, float& out);
    bool SlowLocation(uintptr_t pawn, double out[3]);
    bool SlowStats(uintptr_t item, InvItem& e);
    std::string PlayerJson(const PlayerRecord& p);
    std::string Region();
    takaro::ActionResult ListResult(const std::string& action);
    static std::vector<std::string> Candidates(const JsonValue& args);
    const PlayerRecord* Match(const std::vector<PlayerRecord>& players, const std::vector<std::string>& ids);

    ReadOptions o_;
    std::unique_ptr<UE::Reflection> r_;
    std::mutex mu_;  // serialises every read action and the warm-up
    bool indexed_ = false;
    uint64_t lastScanMs_ = 0, lastStateScanMs_ = 0;
    std::string indexError_;
    PlayerLayout layout_;
    std::vector<uintptr_t> gameStates_;
    // UFunctions for the self-checks and the fallbacks
    UE::FuncInfo fnPing_, fnLocation_, fnIntStat_, fnFloatStat_, fnTextToString_;
    uintptr_t textLibCdo_ = 0;
    int32_t textRetOffset_ = -1, intStatRet_ = -1, floatStatRet_ = -1, locationRet_ = -1, pingRet_ = -1;

    std::shared_ptr<const Catalogue> catalogue_;  // replaced as a whole, read without the lock
    std::string itemsJson_, entitiesJson_, locationsJson_;
    std::mutex catMu_;
    std::condition_variable catCv_;
    bool catalogueDone_ = false;
    std::string catalogueStatus_ = "not started";
    std::string lastHealth_;
    std::string lastStatus_;  // warm-up thread only

    Check pingCheck_ = Check::Pending, locationCheck_ = Check::Pending, statsCheck_ = Check::Pending,
          textCheck_ = Check::Pending;
    bool textRebuilt_ = false;
    std::string checkDetail_;
    struct Known {
        std::string json;
        bool haveLocation = false;
        double loc[3] = {0, 0, 0};
    };
    std::map<std::string, Known> known_;  // Steam64 -> last seen
    uint64_t calls_ = 0, errors_ = 0, gameThreadJobs_ = 0, gameThreadNs_ = 0, lastSnapshotNs_ = 0;
    uint64_t snapshotReads_ = 0;

    std::thread warm_;
    std::atomic<bool> stop_{false};
    std::mutex stopMu_;
    std::condition_variable stopCv_;
};

// The production service (SelfMem, the pinned globals, GameThread::Run, ProcessEvent). Created
// on first use once UE::SetGlobals has run; nullptr before that (refused build, host tests).
ReadService* ProductionReads();

}  // namespace conan
