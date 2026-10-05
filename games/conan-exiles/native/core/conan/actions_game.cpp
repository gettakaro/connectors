#include "conan/actions_game.h"

#include "common.h"
#include "conan/actions_ue.h"
#include "conan/chat.h"
#include "conan/hook_dispatch.h"
#include "conan/text.h"
#include "gamethread.h"

#include <atomic>
#include <chrono>
#include <map>
#include <thread>

namespace conan {
namespace {

using rx::Expect;
using rx::Frame;
using rx::Func;

constexpr int kJobTimeoutMs = 3000;

std::atomic<void (*)()> g_onLogin{nullptr};
// K2_PostLogin, After: the ban sweep checks the new player (a counter bump, nothing else here).
bool OnPostLogin(const HookDispatch::Call&, void*) {
    if (auto f = g_onLogin.load(std::memory_order_acquire)) f();
    return true;
}

class UeGame : public MutationGame {
public:
    bool OnlinePlayers(std::vector<OnlinePlayer>& out, std::string& error) override {
        out.clear();
        if (!rx::EnsureWorld(error)) return false;
        bool ran = GameThread::Run(
            [&] {
                // Players without a resolvable Steam64 cannot be targeted or banned; they are skipped.
                for (auto& c : rx::OnlinePcs())
                    if (!c.steam64.empty()) out.push_back({c.steam64, c.name});
            },
            kJobTimeoutMs);
        if (!ran) error = "game thread did not respond";
        return ran;
    }

    bool ResolveItemCode(const std::string& code, int32_t& id, std::string& error) override {
        const std::unordered_map<std::string, int32_t>* map = nullptr;
        if (!rx::EnsureWorld(error) || !rx::ItemNameMap(map, error)) return false;
        std::string key = code;
        for (auto& c : key)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        auto it = map->find(key);
        if (it == map->end()) {
            error = "unknown item code '" + code + "' (not in Conan's ItemNameToTemplateID tables; a numeric "
                    "template id works too)";
            return false;
        }
        id = it->second;
        return true;
    }

    GiveStep GiveChunk(const std::string& steam64, int32_t templateId, int32_t quantity) override {
        GiveStep st;
        if (!rx::EnsureWorld(st.error)) return st;
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                uintptr_t pawn = PawnOf(steam64, st.error);
                if (!pawn) return;
                const Func* getInv = Fn(rx::ClassOf(pawn), "GetBackpackInventory", {{"ReturnValue", "ObjectProperty", 8}},
                                        st.error);
                if (!getInv) return;
                Frame f1(*getInv);
                if (!rx::Call(pawn, *getInv, f1, &st.error)) return;
                const uintptr_t inv = f1.GetObj("ReturnValue");
                if (!rx::Alive(inv)) {
                    st.error = "the character has no backpack inventory";
                    return;
                }
                const Func* count = Fn(rx::ClassOf(inv), "GetNumberOfItemsByTemplate",
                                       {{"TemplateId", "IntProperty", 4}, {"ReturnValue", "IntProperty", 4}}, st.error);
                const Func* add = Fn(rx::ClassOf(inv), "AddItemTemplate",
                                     {{"TemplateId", "IntProperty", 4},
                                      {"Index", "IntProperty", 4},
                                      {"Context", "NameProperty", 8},
                                      {"quantity", "IntProperty", 4},
                                      {"loot", "BoolProperty", 1},
                                      {"durabilityPercentage", "FloatProperty", 4},
                                      {"durability", "FloatProperty", 4},
                                      {"ReturnValue", "IntProperty", 4}},
                                     st.error);
                if (!count || !add) return;
                Frame c1(*count);
                c1.Int("TemplateId", templateId);
                if (!rx::Call(inv, *count, c1, &st.error)) return;
                Frame a(*add);
                a.Int("TemplateId", templateId)
                    .Int("Index", -1)
                    .NameNone("Context")
                    .Int("quantity", quantity)
                    .Bool("loot", false)
                    .Float("durabilityPercentage", 1.0f)
                    // A durability >= 0 is an absolute value: 0 spawned weapons and tools broken
                    // (live 2026-10-03, Stone Sword 0/180). -1 = use durabilityPercentage (full).
                    .Float("durability", -1.0f);
                if (!rx::Call(inv, *add, a, &st.error)) return;
                Frame c2(*count);
                c2.Int("TemplateId", templateId);
                if (!rx::Call(inv, *count, c2, &st.error)) return;
                st.slot = a.GetInt("ReturnValue");
                st.added = c2.GetInt("ReturnValue") - c1.GetInt("ReturnValue");
                st.ok = true;
                st.gameThreadMs = (NowNs() - t0) / 1e6;
            },
            kJobTimeoutMs);
        if (!ran) st.error = "game thread did not respond";
        return st;
    }

    bool Teleport(const std::string& steam64, const Vec3& to, std::string& error) override {
        if (!rx::EnsureWorld(error)) return false;
        bool ok = false;
        double ms = 0;
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                uintptr_t pc = rx::ControllerFor(steam64);
                if (!pc) {
                    error = "player " + steam64 + " is not online";
                    return;
                }
                const Func* tp = Fn(rx::ClassOf(pc), "TeleportPlayerServer",
                                    {{"TargetLocation", "StructProperty", 24},
                                     {"TargetRotation", "StructProperty", 24},
                                     {"RunCheatCheck", "BoolProperty", 1},
                                     {"SnapToGround", "BoolProperty", 1},
                                     {"ForceTeleportClients", "BoolProperty", 1},
                                     {"UsePawnRotationInstead", "BoolProperty", 1}},
                                    error);
                if (!tp) return;
                Frame f(*tp);
                f.Vec("TargetLocation", to.x, to.y, to.z)
                    .Vec("TargetRotation", 0, 0, 0)
                    .Bool("RunCheatCheck", false)
                    .Bool("SnapToGround", true)
                    .Bool("ForceTeleportClients", true)
                    .Bool("UsePawnRotationInstead", true);
                ok = rx::Call(pc, *tp, f, &error);
                ms = (NowNs() - t0) / 1e6;
            },
            kJobTimeoutMs);
        if (!ran) error = "game thread did not respond";
        if (ok) NativeLog("teleport job: game-thread %.3f ms", ms);
        return ran && ok;
    }

    bool Location(const std::string& steam64, Vec3& out, std::string& error) override {
        if (!rx::EnsureWorld(error)) return false;
        bool ok = false;
        bool ran = GameThread::Run(
            [&] {
                uintptr_t pawn = PawnOf(steam64, error);
                if (!pawn) return;
                const Func* loc = Fn(rx::ClassOf(pawn), "K2_GetActorLocation", {{"ReturnValue", "StructProperty", 24}}, error);
                if (!loc) return;
                Frame f(*loc);
                if (!rx::Call(pawn, *loc, f, &error)) return;
                f.GetVec("ReturnValue", out.x, out.y, out.z);
                ok = true;
            },
            kJobTimeoutMs);
        if (!ran) error = "game thread did not respond";
        return ran && ok;
    }

    bool Kick(const std::string& steam64, const std::string& reason, std::string& error) override {
        rx::Lookups l;
        if (!rx::EnsureWorld(error) || !rx::EnsureLookups(l, error)) return false;
        std::u16string text = ChatText(reason, 512);
        bool ok = false;
        double ms = 0;
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                uintptr_t pc = rx::ControllerFor(steam64);
                if (!pc) {
                    error = "player " + steam64 + " is not online";
                    return;
                }
                const Func* conv = Fn(rx::ClassOf(l.textLibrary), "Conv_StringToText",
                                      {{"InString", "StrProperty", 16}, {"ReturnValue", "TextProperty", 16}}, error);
                // ClientWasKicked makes the Conan client remember the reason; its main menu then
                // shows "Kicked from Server / <reason>" (live 2026-10-03). The return alone shows
                // nothing.
                const Func* told = Fn(rx::ClassOf(pc), "ClientWasKicked", {{"KickReason", "TextProperty", 16}}, error);
                const Func* kick = Fn(rx::ClassOf(pc), "ClientReturnToMainMenuWithTextReason",
                                      {{"ReturnReason", "TextProperty", 16}}, error);
                if (!conv || !told || !kick) return;
                Frame c(*conv);
                c.Str("InString", text);
                if (!rx::Call(l.textLibrary, *conv, c, &error)) return;
                // The FText keeps one reference to its text data; the RPC serializes a copy. That
                // one reference (a few bytes per kick) is intentionally never released.
                Frame w(*told);
                w.Raw("KickReason", c.At("ReturnValue"), 16);
                if (!rx::Call(pc, *told, w, &error)) return;
                Frame k(*kick);
                k.Raw("ReturnReason", c.At("ReturnValue"), 16);
                ok = rx::Call(pc, *kick, k, &error);
                ms = (NowNs() - t0) / 1e6;
            },
            kJobTimeoutMs);
        if (!ran) error = "game thread did not respond";
        if (ok) NativeLog("kick job %s: game-thread %.3f ms", steam64.c_str(), ms);
        return ran && ok;
    }

    ConsoleRun Console(const std::string& command) override {
        ConsoleRun r;
        rx::Lookups l;
        if (!rx::EnsureWorld(r.error) || !rx::EnsureLookups(l, r.error)) return r;
        std::u16string cmd = ChatText(command, 1024);
        bool ran = GameThread::Run(
            [&] {
                const uint64_t t0 = NowNs();
                const uintptr_t lib = l.systemLibrary;
                const Func* frameFn = Fn(rx::ClassOf(lib), "GetFrameCount", {{"ReturnValue", "Int64Property", 8}}, r.error);
                const Func* exec = Fn(rx::ClassOf(lib), "ExecuteConsoleCommand",
                                      {{"WorldContextObject", "ObjectProperty", 8},
                                       {"Command", "StrProperty", 16},
                                       {"SpecificPlayer", "ObjectProperty", 8}},
                                      r.error);
                if (!frameFn || !exec) return;
                Frame fc(*frameFn);
                if (!rx::Call(lib, *frameFn, fc, &r.error)) return;
                r.frame = fc.GetInt64("ReturnValue");
                auto pcs = rx::OnlinePcs();
                const uintptr_t pc = pcs.empty() ? 0 : pcs.front().pc;
                const uintptr_t world = pc ? pc : UE::LiveGameState();
                if (!world) {
                    r.error = "no live world";
                    return;
                }
                // Admin context for admin-only cheat commands, set and restored inside this one job.
                rx::Param admin;
                uintptr_t adminByte = 0;
                uint8_t saved = 0;
                if (pc && rx::FindProperty(rx::ClassOf(pc), "m_IsAdmin", "BoolProperty", 1, admin)) {
                    adminByte = pc + (uintptr_t)admin.offset + admin.boolByteOffset;
                    saved = rx::Rd<uint8_t>(adminByte);
                    rx::Wr<uint8_t>(adminByte, (uint8_t)(saved | (admin.boolByteMask ? admin.boolByteMask : 1)));
                }
                Frame f(*exec);
                f.Obj("WorldContextObject", world).Str("Command", cmd).Obj("SpecificPlayer", pc);
                r.ok = rx::Call(lib, *exec, f, &r.error);
                if (adminByte && rx::Alive(pc)) rx::Wr<uint8_t>(adminByte, saved);
                r.context = pc ? "player " + pcs.front().steam64 : std::string("engine");
                r.gameThreadMs = (NowNs() - t0) / 1e6;
            },
            kJobTimeoutMs);
        if (!ran) r.error = "game thread did not respond";
        return r;
    }

    bool Broadcast(const std::string& message, std::string& error) override {
        ChatRequest req;
        req.message = message;
        req.sender = "Server";
        ChatOutcome o = SendChat(req);
        if (!o.success && o.online > 0) error = o.error;
        return o.success || o.online == 0;  // nobody online: nothing to announce
    }

    bool Exit(std::string& error) override {
        // Same path as executeConsoleCommand: with a player online the command runs in that
        // player's console, which only accepts engine commands while the admin flag is set
        // (live 2026-10-03: "exit" without it was ignored); with nobody online it is the engine's.
        ConsoleRun r = Console("exit");
        if (!r.ok) error = r.error;
        else NativeLog("shutdown: exit ran in the %s console (frame %lld)", r.context.c_str(), (long long)r.frame);
        return r.ok;
    }

    bool ArmLoginHook(void (*onLogin)()) override {
        // One shared hook registry (conan/hook_dispatch.h): matched by name on GameModeBase and every
        // subclass, so the BaseGameMode_C Blueprint override is hooked too.
        static std::atomic<bool> subscribed{false};
        g_onLogin.store(onLogin, std::memory_order_release);
        if (subscribed.exchange(true)) return true;
        HookDispatch::Subscription sub;
        sub.owner = "bans";
        sub.baseClass = "GameModeBase";
        sub.function = "K2_PostLogin";
        sub.phase = HookDispatch::Phase::After;
        sub.fn = &OnPostLogin;
        if (HookDispatch::Subscribe(sub) >= 0) return true;
        subscribed = false;
        return false;
    }

    int64_t NowMs() override {
        int64_t s;
        long ns;
        WallClock(s, ns);
        return s * 1000 + ns / 1000000;
    }
    void SleepMs(int ms) override {
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

private:
    // Game thread only. Reflected functions per class, resolved and type-checked once.
    const Func* Fn(uintptr_t cls, const char* name, const std::vector<Expect>& expect, std::string& error) {
        auto key = std::make_pair(cls, std::string(name));
        auto it = cache_.find(key);
        if (it != cache_.end()) return &it->second;
        Func f;
        if (!rx::ResolveFunc(cls, name, expect, f, error)) return nullptr;
        return &cache_.emplace(key, std::move(f)).first->second;
    }

    // Game thread only: the possessed character of an online player.
    uintptr_t PawnOf(const std::string& steam64, std::string& error) {
        uintptr_t pc = rx::ControllerFor(steam64);
        if (!pc) {
            error = "player " + steam64 + " is not online";
            return 0;
        }
        rx::Param pawnProp;
        if (!rx::FindProperty(rx::ClassOf(pc), "Pawn", "ObjectProperty", 8, pawnProp)) {
            error = "Controller.Pawn not found on this build";
            return 0;
        }
        uintptr_t pawn = rx::Rd<uintptr_t>(pc + (uintptr_t)pawnProp.offset);
        if (!rx::Alive(pawn) || !rx::IsA(pawn, "ConanCharacter")) {
            error = "player " + steam64 + " has no character right now (loading, dead or in the menu)";
            return 0;
        }
        return pawn;
    }

    std::map<std::pair<uintptr_t, std::string>, Func> cache_;
};

}  // namespace

std::shared_ptr<MutationGame> MakeUeMutationGame() { return std::make_shared<UeGame>(); }

}  // namespace conan
