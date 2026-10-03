#include "conan/bans.h"

#include "takaro/fileio.h"
#include "takaro/json_util.h"
#include "takaro/protocol.h"

#include <algorithm>

namespace conan {

using takaro::JStr;
using takaro::Put;

BanList::BanList(std::string path) : path_(std::move(path)) {}

void BanList::Load() {
    std::lock_guard<std::mutex> g(mu_);
    bans_.clear();
    error_.clear();
    std::string text, err;
    bool exists = false;
    if (!takaro::ReadWholeFile(path_, text, exists, err)) {
        error_ = "cannot read " + path_ + ": " + err;
        return;
    }
    if (!exists) return;
    std::vector<Ban> parsed;
    if (!Parse(text, parsed, err)) {
        error_ = path_ + " is not a valid ban list (" + err + "); fix or delete it, bans are refused until then";
        return;
    }
    bans_ = std::move(parsed);
}

std::string BanList::Error() const {
    std::lock_guard<std::mutex> g(mu_);
    return error_;
}

size_t BanList::Size() const {
    std::lock_guard<std::mutex> g(mu_);
    return bans_.size();
}

bool BanList::SaveLocked(const std::vector<Ban>& next, std::string& error) {
    if (!error_.empty()) {
        error = error_;
        return false;
    }
    std::string dirErr;
    takaro::EnsureDirectory(takaro::DirName(path_), dirErr);
    if (!takaro::AtomicWriteFile(path_, Serialize(next), error)) {
        error = "cannot write " + path_ + ": " + error;
        return false;
    }
    bans_ = next;
    return true;
}

bool BanList::Upsert(const Ban& b, std::string& error) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Ban> next;
    for (auto& x : bans_)
        if (x.gameId != b.gameId) next.push_back(x);
    next.push_back(b);
    return SaveLocked(next, error);
}

bool BanList::Remove(const std::string& gameId, bool& removed, std::string& error) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Ban> next;
    removed = false;
    for (auto& x : bans_) {
        if (x.gameId == gameId) removed = true;
        else next.push_back(x);
    }
    if (!removed) return error_.empty() ? true : (error = error_, false);
    return SaveLocked(next, error);
}

std::vector<Ban> BanList::Expire(int64_t nowMs, std::string& error) {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Ban> next, gone;
    for (auto& x : bans_) (x.expiresAtMs && x.expiresAtMs <= nowMs ? gone : next).push_back(x);
    if (gone.empty()) return gone;
    if (!SaveLocked(next, error)) return {};
    return gone;
}

std::vector<Ban> BanList::Active(int64_t nowMs) const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<Ban> out;
    for (auto& x : bans_)
        if (!x.expiresAtMs || x.expiresAtMs > nowMs) out.push_back(x);
    return out;
}

bool BanList::Find(const std::string& gameId, int64_t nowMs, Ban& out) const {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& x : bans_)
        if (x.gameId == gameId && (!x.expiresAtMs || x.expiresAtMs > nowMs)) {
            out = x;
            return true;
        }
    return false;
}

std::string BanList::Serialize(const std::vector<Ban>& bans) {
    std::string o = "{\"version\":1,\"bans\":[";
    for (size_t i = 0; i < bans.size(); i++) {
        const Ban& b = bans[i];
        o += std::string(i ? "," : "") + "\n  " +
             takaro::ObjBuilder()
                 .S("gameId", b.gameId)
                 .S("name", b.name)
                 .S("reason", b.reason)
                 .Raw("expiresAt", b.expiresAtMs ? JsonStr(takaro::FormatIsoMs(b.expiresAtMs)) : "null")
                 .S("createdAt", takaro::FormatIsoMs(b.createdAtMs))
                 .Done();
    }
    return o + (bans.empty() ? "]}\n" : "\n]}\n");
}

bool BanList::Parse(const std::string& text, std::vector<Ban>& out, std::string& error) {
    out.clear();
    JsonValue root;
    if (!takaro::ParseJson(text, root) || root.type != JsonValue::Object) {
        error = "not a JSON object";
        return false;
    }
    const JsonValue* list = root.get("bans");
    if (!list || list->type != JsonValue::Array) {
        error = "no bans array";
        return false;
    }
    for (auto& e : list->arr) {
        if (e.type != JsonValue::Object) {
            error = "a ban entry is not an object";
            return false;
        }
        Ban b;
        b.gameId = takaro::Str(e.get("gameId")).value_or("");
        if (b.gameId.empty()) {
            error = "a ban entry has no gameId";
            return false;
        }
        b.name = takaro::Str(e.get("name")).value_or(b.gameId);
        b.reason = takaro::Str(e.get("reason")).value_or("");
        if (auto exp = takaro::Str(e.get("expiresAt"))) {
            if (!takaro::ParseIsoMs(*exp, b.expiresAtMs)) {
                error = "ban of " + b.gameId + " has an invalid expiresAt";
                return false;
            }
        }
        if (auto c = takaro::Str(e.get("createdAt"))) takaro::ParseIsoMs(*c, b.createdAtMs);
        out.push_back(b);
    }
    return true;
}

JsonValue BanToJson(const Ban& b) {
    JsonValue player = takaro::JObj();
    Put(player, "gameId", JStr(b.gameId));
    Put(player, "name", JStr(b.name.empty() ? b.gameId : b.name));
    Put(player, "steamId", JStr(b.gameId));
    Put(player, "platformId", JStr("steam:" + b.gameId));
    JsonValue ban = takaro::JObj();
    Put(ban, "player", player);
    Put(ban, "reason", JStr(b.reason));
    Put(ban, "expiresAt", b.expiresAtMs ? JStr(takaro::FormatIsoMs(b.expiresAtMs)) : takaro::JNull());
    return ban;
}

}  // namespace conan
