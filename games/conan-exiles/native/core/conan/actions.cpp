#include "conan/actions.h"

#include "conan/shutdown.h"
#include "conan/text.h"
#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace conan {

using takaro::ActionResult;
using takaro::AsRecord;
using takaro::JStr;
using takaro::Num;
using takaro::Put;
using takaro::Str;

std::atomic<uint64_t> g_loginCount{0};
void OnLoginHook() { g_loginCount.fetch_add(1, std::memory_order_relaxed); }

namespace {

constexpr int kMaxGiveAmount = 100000;
constexpr int kMaxGiveSteps = 200;  // a stack is at most 1000 on this build, usually far less
constexpr double kTeleportToleranceXY = 300;  // Unreal units; SnapToGround only moves Z
constexpr size_t kMaxConsoleRead = 256 * 1024;
constexpr size_t kMaxConsoleLines = 60;
const char* const kDefaultKickReason = "Kicked by an admin";
const char* const kDefaultBanReason = "Banned by an admin";

ActionResult Fail(const std::string& e) {
    ActionResult r;
    r.error = e;
    return r;
}
ActionResult Ok(JsonValue payload = JsonValue()) {
    ActionResult r;
    r.ok = true;
    r.payload = std::move(payload);
    return r;
}

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool AllDigits(const std::string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Steam64 of the request's player; resolves a name against the online list as a fallback.
bool TargetPlayer(MutationGame& game, const JsonValue& args, std::string& steam64, std::string& name, bool needOnline,
                  std::string& error) {
    std::string id = takaro::StripSteamPrefix(takaro::PlayerId(args));
    if (id.empty()) id = Str(AsRecord(args.get("player")).get("name")).value_or("");
    if (id.empty()) {
        error = "missing player (gameId)";
        return false;
    }
    std::vector<OnlinePlayer> online;
    std::string err;
    const bool listed = game.OnlinePlayers(online, err);
    for (auto& p : online) {
        if (p.steam64 == id || (!IsSteam64(id) && EqualsIgnoreCase(p.name, id))) {
            steam64 = p.steam64;
            name = p.name;
            return true;
        }
    }
    if (needOnline) {
        error = listed ? "player " + id + " is not online" : "cannot read the online players: " + err;
        return false;
    }
    if (!IsSteam64(id)) {
        error = "player " + id + " is not a Steam64 id and not online";
        return false;
    }
    steam64 = id;
    name = Str(AsRecord(args.get("player")).get("name")).value_or("");
    return true;
}

}  // namespace

std::string GiveItemCode(const JsonValue& args) {
    for (auto* k : {"item", "itemCode", "code", "name"})
        if (auto v = Str(args.get(k))) return *v;
    const JsonValue& item = AsRecord(args.get("item"));
    for (auto* k : {"code", "itemCode", "templateId", "name"})
        if (auto v = Str(item.get(k))) return *v;
    return "";
}

std::vector<std::string> LinesOfFrame(const std::string& text, int64_t frame) {
    std::vector<std::string> out;
    if (frame < 0) return out;
    char tag[16];
    snprintf(tag, sizeof tag, "][%3lld]", (long long)(frame % 1000));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // "[2026.10.03-09.35.18:020][781]Category: text"
        if (line.size() < 30 || line[0] != '[') continue;
        size_t close = line.find(']');
        if (close == std::string::npos || line.compare(close, strlen(tag), tag) != 0) continue;
        out.push_back(line.substr(close + strlen(tag)));
    }
    return out;
}

std::string ConsoleRefusal(const std::string& command) {
    std::string c = takaro::Trim(command);
    if (c.empty()) return "executeConsoleCommand needs a command";
    for (unsigned char ch : c)
        if (ch < 0x20) return "executeConsoleCommand takes one line without control characters";
    std::string verb = LowerAscii(c.substr(0, c.find(' ')));
    if (verb == "exit" || verb == "quit")
        return "'" + verb + "' would stop the server at once; use Takaro's shutdown action (it warns players first)";
    static const char* const kRcon[][2] = {
        {"listplayers", "getPlayers"}, {"kickplayer", "kickPlayer"}, {"banplayer", "banPlayer"},
        {"unbanplayer", "unbanPlayer"}, {"listbans", "listBans"},     {"broadcast", "sendMessage"},
        {"shutdown", "shutdown"},
    };
    for (auto& r : kRcon)
        if (verb == r[0])
            return "'" + verb + "' is an RCON verb, not a game console command, and this connector uses no RCON; use "
                   "Takaro's " + r[1] + " action";
    return "";
}

// ---------------------------------------------------------------------------------------------
Mutations::Mutations(MutationOptions o, std::shared_ptr<MutationGame> game)
    : o_(std::move(o)),
      game_(std::move(game)),
      bans_(o_.banFile.empty() ? takaro::JoinPath(takaro::JoinPath(o_.savedDir, "Config/Takaro"), "bans.json")
                               : o_.banFile) {
    if (o_.serverLog.empty()) o_.serverLog = takaro::JoinPath(o_.savedDir, "Logs/ConanSandbox.log");
    bans_.Load();
    if (!bans_.Error().empty()) NativeLog("bans: %s", bans_.Error().c_str());
    else NativeLog("bans: %zu entries loaded from %s", bans_.Size(), bans_.path().c_str());
    if (o_.startThreads) sweeper_ = std::thread(&Mutations::SweepLoop, this);
}

Mutations::~Mutations() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (sweeper_.joinable()) sweeper_.join();
    if (shutdown_.joinable()) shutdown_.join();
}

bool Mutations::Handles(const std::string& a) {
    return a == "giveItem" || a == "teleportPlayer" || a == "executeConsoleCommand" || a == "kickPlayer" ||
           a == "banPlayer" || a == "unbanPlayer" || a == "listBans" || a == "shutdown";
}

ActionResult Mutations::Execute(const std::string& a, const JsonValue& args) {
    ActionResult r;
    if (a == "giveItem") r = GiveItem(args);
    else if (a == "teleportPlayer") r = TeleportPlayer(args);
    else if (a == "executeConsoleCommand") r = ExecuteConsoleCommand(args);
    else if (a == "kickPlayer") r = KickPlayer(args);
    else if (a == "banPlayer") r = BanPlayer(args);
    else if (a == "unbanPlayer") r = UnbanPlayer(args);
    else if (a == "listBans") r = ListBans();
    else if (a == "shutdown") r = Shutdown();
    else return Fail(a + " is not a mutation action");
    if (!r.ok) {
        failures_++;
        NativeLog("%s failed: %s", a.c_str(), r.error.c_str());
    }
    return r;
}

// ---- giveItem ----
ActionResult Mutations::GiveItem(const JsonValue& args) {
    const std::string code = GiveItemCode(args);
    if (code.empty()) return Fail("giveItem needs an item code");
    double amountD = Num(args.get("amount")).value_or(Num(args.get("quantity")).value_or(1));
    if (!std::isfinite(amountD) || amountD < 1 || amountD != std::floor(amountD) || amountD > kMaxGiveAmount)
        return Fail("giveItem amount must be a whole number from 1 to " + std::to_string(kMaxGiveAmount));
    const int32_t amount = (int32_t)amountD;
    // Conan items have no quality tier; Takaro's quality (string or null) is accepted and ignored.
    int32_t templateId = 0;
    std::string err;
    if (AllDigits(code)) {
        templateId = (int32_t)std::stol(code);
    } else if (!game_->ResolveItemCode(code, templateId, err)) {
        return Fail("giveItem: " + err);
    }
    std::string steam64, name;
    if (!TargetPlayer(*game_, args, steam64, name, true, err)) return Fail("giveItem: " + err);

    int32_t given = 0, steps = 0;
    double gt = 0;
    std::string why;
    while (given < amount && steps < kMaxGiveSteps) {
        steps++;
        auto st = game_->GiveChunk(steam64, templateId, amount - given);
        gt += st.gameThreadMs;
        if (!st.ok) {
            why = st.error;
            break;
        }
        if (st.added <= 0) {
            if (given == 0 && st.slot < 0)
                return Fail("giveItem: item " + code + " (template " + std::to_string(templateId) +
                            ") is not a Conan item on this server, or the backpack of " + name + " is full");
            why = "the backpack is full";
            break;
        }
        given += st.added;
    }
    NativeLog("giveItem %s (template %d) x%d to %s: given %d in %d step(s), game-thread %.3f ms", code.c_str(),
              templateId, amount, steam64.c_str(), given, steps, gt);
    if (given < amount)
        return Fail("giveItem: gave " + std::to_string(given) + " of " + std::to_string(amount) + " " + code +
                    " to " + name + (why.empty() ? "" : ": " + why));
    gives_++;
    return Ok();
}

// ---- teleportPlayer ----
ActionResult Mutations::TeleportPlayer(const JsonValue& args) {
    auto x = Num(args.get("x")), y = Num(args.get("y")), z = Num(args.get("z"));
    if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z))
        return Fail("teleportPlayer needs numeric x, y and z");
    std::string steam64, name, err;
    if (!TargetPlayer(*game_, args, steam64, name, true, err)) return Fail("teleportPlayer: " + err);
    Vec3 to{*x, *y, *z};
    if (!game_->Teleport(steam64, to, err)) return Fail("teleportPlayer: " + err);
    // Read back: the server moves the pawn at once (then streams the area for the client).
    Vec3 at;
    double d = -1;
    for (int waited = 0; waited <= o_.verifyTimeoutMs; waited += 250) {
        if (game_->Location(steam64, at, err)) {
            d = std::hypot(at.x - to.x, at.y - to.y);
            if (d <= kTeleportToleranceXY) break;
        }
        game_->SleepMs(250);
    }
    NativeLog("teleportPlayer %s to (%.1f, %.1f, %.1f): now at (%.1f, %.1f, %.1f), xy distance %.1f", steam64.c_str(),
              to.x, to.y, to.z, at.x, at.y, at.z, d);
    if (d < 0 || d > kTeleportToleranceXY)
        return Fail("teleportPlayer: the teleport ran but " + name + " is not at the target (xy distance " +
                    (d < 0 ? std::string("unknown") : std::to_string((int)d)) + ")");
    teleports_++;
    return Ok();
}

// ---- executeConsoleCommand ----
ActionResult Mutations::ExecuteConsoleCommand(const JsonValue& args) {
    std::string cmd = Str(args.get("command")).value_or(Str(args.get("rawCommand")).value_or(""));
    std::string refusal = ConsoleRefusal(cmd);
    if (!refusal.empty()) return Fail(refusal);
    cmd = takaro::Trim(cmd);
    const takaro::FileStat before = takaro::StatFile(o_.serverLog);
    auto run = game_->Console(cmd);
    if (!run.ok) return Fail("executeConsoleCommand: " + run.error);
    // Conan's console commands answer through UE_LOG, not through a return value: collect the
    // server log lines of the call's frame once the log has flushed them.
    game_->SleepMs(o_.consoleSettleMs);
    std::string text;
    takaro::FileStat after = takaro::StatFile(o_.serverLog);
    if (after.exists) {
        uint64_t from = before.exists && before.identity == after.identity && before.size <= after.size ? before.size : 0;
        uint64_t len = std::min<uint64_t>(after.size - from, kMaxConsoleRead);
        takaro::ReadFileRange(o_.serverLog, from, len, text);
    }
    auto lines = LinesOfFrame(text, run.frame);
    if (lines.size() > kMaxConsoleLines) lines.resize(kMaxConsoleLines);
    std::string raw;
    for (auto& l : lines) raw += (raw.empty() ? "" : "\n") + l;
    NativeLog("executeConsoleCommand '%s' as %s: frame %lld, %zu log line(s), game-thread %.3f ms", cmd.c_str(),
              run.context.c_str(), (long long)run.frame, lines.size(), run.gameThreadMs);
    consoles_++;
    JsonValue p = takaro::JObj();
    Put(p, "success", takaro::JBool(true));
    Put(p, "rawResult", JStr(raw.empty() ? "(the command ran in the " + run.context +
                                               " console; the server logged no output for it)"
                                         : raw));
    return Ok(p);
}

// ---- kick ----
bool Mutations::WaitOffline(const std::string& steam64) {
    for (int waited = 0; waited <= o_.verifyTimeoutMs; waited += 250) {
        std::vector<OnlinePlayer> online;
        std::string err;
        if (game_->OnlinePlayers(online, err) &&
            std::none_of(online.begin(), online.end(), [&](const OnlinePlayer& p) { return p.steam64 == steam64; }))
            return true;
        game_->SleepMs(250);
    }
    return false;
}

ActionResult Mutations::KickPlayer(const JsonValue& args) {
    std::string steam64, name, err;
    if (!TargetPlayer(*game_, args, steam64, name, true, err)) return Fail("kickPlayer: " + err);
    std::string reason = Str(args.get("reason")).value_or(kDefaultKickReason);
    if (!game_->Kick(steam64, reason, err)) return Fail("kickPlayer: " + err);
    if (!WaitOffline(steam64))
        return Fail("kickPlayer: the kick was sent but " + name + " is still connected");
    kicks_++;
    NativeLog("kickPlayer %s: disconnected (%s)", steam64.c_str(), reason.c_str());
    return Ok();
}

// ---- bans ----
ActionResult Mutations::BanPlayer(const JsonValue& args) {
    std::string steam64, name, err;
    if (!TargetPlayer(*game_, args, steam64, name, false, err)) return Fail("banPlayer: " + err);
    Ban b;
    b.gameId = steam64;
    b.reason = Str(args.get("reason")).value_or(kDefaultBanReason);
    b.createdAtMs = game_->NowMs();
    if (auto exp = Str(args.get("expiresAt"))) {
        if (!takaro::ParseIsoMs(*exp, b.expiresAtMs))
            return Fail("banPlayer: expiresAt '" + *exp + "' is not an ISO-8601 date with a time zone");
        if (b.expiresAtMs <= b.createdAtMs) return Fail("banPlayer: expiresAt " + *exp + " is in the past");
    }
    Ban old;
    if (name.empty() && bans_.Find(steam64, 0, old)) name = old.name;
    b.name = name.empty() ? steam64 : name;
    if (!bans_.Upsert(b, err)) return Fail("banPlayer: " + err);
    NativeLog("banPlayer %s (%s) until %s: %s", steam64.c_str(), b.name.c_str(),
              b.expiresAtMs ? takaro::FormatIsoMs(b.expiresAtMs).c_str() : "forever", b.reason.c_str());
    cv_.notify_all();  // the sweeper arms the login hook and kicks the player if online
    std::vector<OnlinePlayer> online;
    if (game_->OnlinePlayers(online, err) &&
        std::any_of(online.begin(), online.end(), [&](const OnlinePlayer& p) { return p.steam64 == steam64; })) {
        std::string kickErr;
        if (game_->Kick(steam64, "You are banned from this server: " + b.reason, kickErr) && WaitOffline(steam64)) {
            banKicks_++;
        } else {
            // The ban is stored and enforced by the sweep; report it but do not fail the ban.
            NativeLog("banPlayer %s: online kick not confirmed yet (%s); the ban sweep keeps enforcing it",
                      steam64.c_str(), kickErr.c_str());
        }
    }
    return Ok();
}

ActionResult Mutations::UnbanPlayer(const JsonValue& args) {
    std::string id = takaro::StripSteamPrefix(takaro::PlayerId(args));
    if (id.empty()) return Fail("unbanPlayer: missing player (gameId)");
    bool removed = false;
    std::string err;
    if (!bans_.Remove(id, removed, err)) return Fail("unbanPlayer: " + err);
    NativeLog("unbanPlayer %s: %s", id.c_str(), removed ? "removed" : "was not banned");
    return Ok();
}

ActionResult Mutations::ListBans() {
    if (!bans_.Error().empty()) return Fail("listBans: " + bans_.Error());
    JsonValue list = takaro::JArr();
    for (auto& b : bans_.Active(game_->NowMs())) list.arr.push_back(BanToJson(b));
    return Ok(list);
}

constexpr int64_t kSweepKickGapMs = 5000;

int Mutations::SweepOnce() {
    std::string err;
    for (auto& b : bans_.Expire(game_->NowMs(), err))
        NativeLog("ban of %s expired and was lifted", b.gameId.c_str());
    auto active = bans_.Active(game_->NowMs());
    if (active.empty()) return 0;
    if (!loginHookArmed_ && game_->ArmLoginHook(&OnLoginHook)) {
        loginHookArmed_ = true;
        NativeLog("bans: K2_PostLogin hook installed");
    }
    std::vector<OnlinePlayer> online;
    if (!game_->OnlinePlayers(online, err)) return 0;
    int kicked = 0;
    for (auto& p : online) {
        for (auto& b : active) {
            if (b.gameId != p.steam64) continue;
            auto last = sweepKickedAt_.find(p.steam64);
            if (last != sweepKickedAt_.end() && game_->NowMs() - last->second < kSweepKickGapMs) continue;
            std::string reason = "You are banned from this server: " + b.reason +
                                 (b.expiresAtMs ? " (until " + takaro::FormatIsoMs(b.expiresAtMs) + ")" : "");
            if (game_->Kick(p.steam64, reason, err)) {
                sweepKickedAt_[p.steam64] = game_->NowMs();
                kicked++;
                banKicks_++;
                NativeLog("bans: kicked banned player %s (%s)", p.steam64.c_str(), p.name.c_str());
            } else {
                NativeLog("bans: kick of banned player %s failed: %s", p.steam64.c_str(), err.c_str());
            }
        }
    }
    return kicked;
}

void Mutations::SweepLoop() {
    uint64_t seenLogins = g_loginCount.load();
    int64_t fastUntil = 0, lastSweep = 0;
    std::unique_lock<std::mutex> l(mu_);
    while (!stopping_) {
        // No bans: one wake per sweepIdleMs that touches no game state. With bans: a 250 ms check
        // of the login counter (K2_PostLogin hook), a sweep every 500 ms for 15 s after a login,
        // otherwise one every sweepIdleMs (also lifts expired bans).
        const bool any = bans_.Size() > 0;
        cv_.wait_for(l, std::chrono::milliseconds(any ? 250 : o_.sweepIdleMs));
        if (stopping_) break;
        if (bans_.Size() == 0) continue;
        const int64_t t = game_->NowMs();
        const uint64_t logins = g_loginCount.load();
        if (logins != seenLogins) {
            seenLogins = logins;
            fastUntil = t + 15000;
        }
        const bool due = t - lastSweep >= (t < fastUntil ? 500 : o_.sweepIdleMs) || !any;
        if (!due) continue;
        lastSweep = t;
        l.unlock();
        SweepOnce();
        l.lock();
    }
}

// ---- shutdown ----
ActionResult Mutations::Shutdown() {
    bool expected = false;
    if (!shutdownScheduled_.compare_exchange_strong(expected, true)) {
        NativeLog("shutdown: already counting down");
        return Ok();
    }
    const int seconds = std::max(0, o_.shutdownSeconds);
    NativeLog("shutdown: requested by Takaro, countdown %d s", seconds);
    // Respond first, exit later (the response must reach Takaro).
    shutdown_ = std::thread(&Mutations::RunShutdown, this, seconds);
    return Ok();
}

void Mutations::RunShutdown(int seconds) {
    auto marks = CountdownMarks(seconds);
    int left = seconds;
    std::string err;
    for (int m : marks) {
        game_->SleepMs((left - m) * 1000);
        left = m;
        if (!game_->Broadcast(CountdownText(m), err)) NativeLog("shutdown: countdown message failed: %s", err.c_str());
    }
    game_->SleepMs(left * 1000);
    game_->Broadcast("The server is shutting down now.", err);
    game_->SleepMs(500);
    if (game_->Exit(err)) NativeLog("shutdown: engine exit requested");
    else NativeLog("shutdown: engine exit FAILED: %s", err.c_str());
}

void Mutations::JoinShutdown() {
    if (shutdown_.joinable()) shutdown_.join();
}

std::string Mutations::HealthJson() {
    return takaro::ObjBuilder()
        .N("giveItem", (double)gives_)
        .N("teleport", (double)teleports_)
        .N("console", (double)consoles_)
        .N("kick", (double)kicks_)
        .N("banKicks", (double)banKicks_)
        .N("failures", (double)failures_)
        .N("bans", (double)bans_.Size())
        .S("banStoreError", bans_.Error())
        .B("loginHook", loginHookArmed_)
        .B("shutdownScheduled", shutdownScheduled_)
        .Done();
}

}  // namespace conan
