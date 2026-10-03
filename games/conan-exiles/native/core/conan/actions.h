// The mutation actions of the native Conan connector (lane L2b): giveItem, teleportPlayer,
// executeConsoleCommand, kickPlayer, banPlayer, unbanPlayer, listBans and shutdown.
//
// Mutations holds the action logic (argument parsing, chunked give with read-back, verified
// teleport and kick, the ban list and its enforcement, the console log bracket, the shutdown
// countdown). Everything that touches the game sits behind MutationGame, so host tests drive the
// logic with a fake; UeMutationGame (actions_game.cpp) is the real one. Each game call is one
// queued game-thread job (spike S1/S2 costs: 0.06 to 5 ms; the teleport and admin-console jobs
// exceed the 500 us per-tick budget and therefore run alone in their tick).
#pragma once

#include "conan/bans.h"
#include "takaro/game.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace conan {

struct OnlinePlayer {
    std::string steam64, name;
};
struct Vec3 {
    double x = 0, y = 0, z = 0;
};

class MutationGame {
public:
    virtual ~MutationGame() = default;
    virtual bool OnlinePlayers(std::vector<OnlinePlayer>& out, std::string& error) = 0;
    // A Takaro item code -> template id through the ItemNameToTemplateID tables.
    virtual bool ResolveItemCode(const std::string& code, int32_t& templateId, std::string& error) = 0;
    struct GiveStep {
        bool ok = false;  // the call ran (false: error says why)
        std::string error;
        int32_t slot = -1;  // AddItemTemplate's return: -1 = unknown template or no room
        int32_t added = 0;  // backpack count after - before (the read-back)
        double gameThreadMs = 0;
    };
    // One AddItemTemplate call (one stack at most) into the player's backpack.
    virtual GiveStep GiveChunk(const std::string& steam64, int32_t templateId, int32_t quantity) = 0;
    virtual bool Teleport(const std::string& steam64, const Vec3& to, std::string& error) = 0;
    virtual bool Location(const std::string& steam64, Vec3& out, std::string& error) = 0;
    virtual bool Kick(const std::string& steam64, const std::string& reason, std::string& error) = 0;
    struct ConsoleRun {
        bool ok = false;
        std::string error;
        int64_t frame = -1;        // GFrameCounter during the call (tags its log lines)
        std::string context;       // whose console ran it ("player <steam64>" / "engine")
        double gameThreadMs = 0;
    };
    virtual ConsoleRun Console(const std::string& command) = 0;
    virtual bool Broadcast(const std::string& message, std::string& error) = 0;
    virtual bool Exit(std::string& error) = 0;  // the engine's graceful exit
    // Installs the K2_PostLogin hook once the world is up; `onLogin` runs on the game thread
    // (keep it trivial). True once installed.
    virtual bool ArmLoginHook(void (*onLogin)()) = 0;
    virtual int64_t NowMs() = 0;  // wall clock, Unix ms
    virtual void SleepMs(int ms) = 0;
};

struct MutationOptions {
    std::string savedDir;  // <server>/ConanSandbox/Saved (bans file, console log)
    std::string banFile;   // default <savedDir>/Config/Takaro/bans.json
    std::string serverLog; // default <savedDir>/Logs/ConanSandbox.log
    int shutdownSeconds = 60;   // countdown before the exit (TAKARO_CONAN_SHUTDOWN_SECONDS)
    int verifyTimeoutMs = 6000; // kick / teleport read-back (inside Takaro's 10 s request timeout)
    int consoleSettleMs = 450;  // wait for the server log to flush the command's lines (p90 201 ms)
    int sweepIdleMs = 2000;     // ban sweep cadence while bans exist
    bool startThreads = true;   // host tests drive SweepOnce() themselves
};

class Mutations {
public:
    Mutations(MutationOptions o, std::shared_ptr<MutationGame> game);
    ~Mutations();
    static bool Handles(const std::string& action);
    takaro::ActionResult Execute(const std::string& action, const JsonValue& args);
    std::string HealthJson();

    // One enforcement pass: lift expired bans, kick banned online players. Returns kicks sent.
    int SweepOnce();
    // Blocks until a scheduled shutdown finished its countdown (tests).
    void JoinShutdown();
    BanList& bans() { return bans_; }

private:
    takaro::ActionResult GiveItem(const JsonValue& args);
    takaro::ActionResult TeleportPlayer(const JsonValue& args);
    takaro::ActionResult ExecuteConsoleCommand(const JsonValue& args);
    takaro::ActionResult KickPlayer(const JsonValue& args);
    takaro::ActionResult BanPlayer(const JsonValue& args);
    takaro::ActionResult UnbanPlayer(const JsonValue& args);
    takaro::ActionResult ListBans();
    takaro::ActionResult Shutdown();
    bool WaitOffline(const std::string& steam64);
    void SweepLoop();
    void RunShutdown(int seconds);

    MutationOptions o_;
    std::shared_ptr<MutationGame> game_;
    BanList bans_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread sweeper_, shutdown_;
    std::atomic<bool> shutdownScheduled_{false};
    // counters for health
    std::atomic<uint64_t> gives_{0}, teleports_{0}, consoles_{0}, kicks_{0}, banKicks_{0}, failures_{0};
    // Last sweep kick per Steam64: a client needs a second or two to leave, and every extra kick
    // stacks one more "Kicked from Server" dialog in its main menu.
    std::map<std::string, int64_t> sweepKickedAt_;
    std::atomic<bool> loginHookArmed_{false};
};

// The login hook body (game thread): bumps a counter the sweeper watches.
extern std::atomic<uint64_t> g_loginCount;
void OnLoginHook();

// Pure helpers, exposed for the host tests.
// The item code of a giveItem request: item / itemCode / code / name, or item.{code,...}.
std::string GiveItemCode(const JsonValue& args);
// Lines of `text` (a ConanSandbox.log slice) logged in frame `frame` (the "[nnn]" tag is the frame
// counter modulo 1000), with the "[date][frame]" prefix removed.
std::vector<std::string> LinesOfFrame(const std::string& text, int64_t frame);
// executeConsoleCommand input check: empty string when allowed, else the refusal text.
std::string ConsoleRefusal(const std::string& command);

}  // namespace conan
