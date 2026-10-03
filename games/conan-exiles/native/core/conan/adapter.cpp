#include "conan/adapter.h"

#include "conan/actions_game.h"
#include "conan/coverage.h"
#include "conan/reads.h"
#include "conan/text.h"
#include "gtstats.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"

#include <algorithm>

namespace conan {

using takaro::ActionResult;
using takaro::AsRecord;
using takaro::GameEvent;
using takaro::ObjBuilder;
using takaro::Str;

namespace {

std::string First(const std::vector<const JsonValue*>& records, std::initializer_list<const char*> keys) {
    for (auto* r : records)
        for (auto* k : keys)
            if (auto v = Str(r->get(k))) return *v;
    return "";
}

}  // namespace

bool ParseSendMessage(const JsonValue& args, ChatRequest& out, std::string& error) {
    out = ChatRequest();
    auto msg = Str(args.get("message"));
    if (!msg) msg = Str(args.get("text"));
    if (!msg) {
        error = "sendMessage needs a non-empty message";
        return false;
    }
    out.message = *msg;
    const JsonValue& opts = AsRecord(args.get("opts"));
    std::vector<const JsonValue*> records = {&args, &AsRecord(args.get("player")), &opts,
                                             &AsRecord(opts.get("recipient")), &AsRecord(args.get("recipient"))};
    std::string recipient = First(records, {"platformId"});
    if (recipient.empty()) recipient = First(records, {"steamId"});
    if (recipient.empty()) recipient = First(records, {"gameId", "userId", "playerId"});
    if (recipient.empty()) recipient = First(records, {"name", "playerName"});
    if (recipient.empty()) {
        const JsonValue* r = args.get("recipient");
        if (r && r->type == JsonValue::String) recipient = r->str;
    }
    out.recipient = NormalizeRecipient(recipient);
    std::string sender = Str(opts.get("senderNameOverride")).value_or(Str(args.get("senderNameOverride")).value_or(""));
    out.sender = sender.empty() ? kDefaultSender : sender;
    return true;
}

Adapter::Adapter(AdapterOptions o) : o_(std::move(o)) {
    if (!o_.chat) o_.chat = SendChat;
    if (!o_.reads) {
        o_.reads = [](const std::string& action, const JsonValue& args, ActionResult& out) {
            if (!ReadService::Handles(action)) return false;
            ReadService* rs = ProductionReads();
            if (rs) out = rs->Execute(action, args);
            else out.error = action + ": the engine globals are not set (no verified server build)";
            return true;
        };
        o_.readsHealth = [] {
            ReadService* rs = ProductionReads();
            return rs ? rs->HealthJson() : std::string("null");
        };
        if (o_.ready) ProductionReads();  // starts the catalogue warm-up on its worker thread
    }
    if (o_.ready && (!o_.savedDir.empty() || o_.mutationGame)) {
        MutationOptions mo = o_.mutationOptions;
        mo.savedDir = o_.savedDir;
        mutations_.reset(new Mutations(mo, o_.mutationGame ? o_.mutationGame : MakeUeMutationGame()));
    }
    if (!o_.ready) {
        // The one critical notice of this process. It goes through the durable outbox like any event,
        // so it reaches Takaro after the first identify even when Takaro is down right now.
        GameEvent ev;
        ev.type = "log";
        ev.data = takaro::JObj();
        takaro::Put(ev.data, "msg",
                    takaro::JStr("[Takaro Conan native] CRITICAL: " + o_.refusal +
                                 ". The connector is connected but refuses every action and installed no hook; "
                                 "update the connector to a release pinned to this server build."));
        Emit(std::move(ev));
    }
}

ActionResult Adapter::Fail(const std::string& error) {
    ActionResult r;
    r.error = error;
    return r;
}

ActionResult Adapter::Execute(const std::string& action, const JsonValue& args) {
    const Coverage* cov = ActionCoverage(action);
    if (!cov) return Fail("unknown action '" + action + "'");
    if (!o_.ready) {
        if (action == "testReachability") {
            ActionResult r;
            r.ok = true;
            r.payload = takaro::JObj();
            takaro::Put(r.payload, "connectable", takaro::JBool(false));
            takaro::Put(r.payload, "reason", takaro::JStr("Conan native connector refused this server: " + o_.refusal));
            return r;
        }
        return Fail(action + " refused: " + o_.refusal + " (the native connector does not touch an unverified build)");
    }
    if (std::string(cov->implementation) != "native")
        return Fail(action + " is not implemented by the native Conan connector yet (" + cov->reason + ")");

    ActionResult r;
    if (action == "testReachability") {
        r.ok = true;
        r.payload = takaro::JObj();
        takaro::Put(r.payload, "connectable", takaro::JBool(true));
        takaro::Put(r.payload, "reason", takaro::JNull());
        return r;
    }
    if (action == "getMapInfo") {
        r.ok = true;
        r.payload = takaro::JObj();
        takaro::Put(r.payload, "enabled", takaro::JBool(false));
        for (auto* k : {"mapBlockSize", "maxZoom", "mapSizeX", "mapSizeY", "mapSizeZ"})
            takaro::Put(r.payload, k, takaro::JNum(0));
        return r;
    }
    if (mutations_ && Mutations::Handles(action)) return mutations_->Execute(action, args);
    if (action == "getMapTile") return Fail("getMapTile is not supported: " + std::string(cov->reason));
    if (action == "sendMessage") {
        ChatRequest req;
        std::string error;
        if (!ParseSendMessage(args, req, error)) return Fail(error);
        ChatOutcome out = o_.chat(req);
        {
            std::lock_guard<std::mutex> g(mu_);
            (out.success ? chatSent_ : chatFailed_)++;
            lastChatMs_ = out.gameThreadMs;
        }
        if (!out.success) return Fail(out.error);
        r.ok = true;  // payload Null answers {}
        return r;
    }
    if (o_.reads(action, args, r)) return r;
    return Fail(action + " has no native handler");
}

void Adapter::Emit(GameEvent ev) {
    std::lock_guard<std::mutex> g(mu_);
    while (events_.size() >= kMaxQueuedEvents) {
        auto victim = std::find_if(events_.begin(), events_.end(), [](const GameEvent& e) { return e.type == "log"; });
        if (victim == events_.end()) victim = events_.begin();
        events_.erase(victim);
        dropped_++;
    }
    events_.push_back(std::move(ev));
}

void Adapter::DrainEvents(std::vector<GameEvent>& out) {
    std::lock_guard<std::mutex> g(mu_);
    while (!events_.empty()) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
}

std::string Adapter::HealthJson() {
    std::lock_guard<std::mutex> g(mu_);
    return ObjBuilder()
        .S("version", o_.version)
        .B("ready", o_.ready)
        .S("refusal", o_.refusal)
        .S("buildId", o_.buildId)
        .S("build", o_.build)
        .S("pins", o_.pinsDetail)
        .N("pinsScanMs", o_.pinsScanMs)
        .N("queuedEvents", (double)events_.size())
        .N("droppedEvents", (double)dropped_)
        .N("chatSent", (double)chatSent_)
        .N("chatFailed", (double)chatFailed_)
        .N("lastChatGameThreadMs", lastChatMs_)
        .Raw("reads", o_.readsHealth ? o_.readsHealth() : std::string("null"))
        .Raw("mutations", mutations_ ? mutations_->HealthJson() : "null")
        .Raw("gameThread", GtStats::HealthJson())
        .Done();
}

}  // namespace conan
