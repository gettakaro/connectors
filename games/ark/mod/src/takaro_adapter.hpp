#pragma once
// Generic action handling (port of the 0.1 sidecar adapter, request handler and
// ban manager). Runs on bridge worker threads only. A failure throws Withhold:
// Takaro's Generic void actions treat any response frame as success, so a failed
// action leaves its request unanswered and Takaro's bounded timeout rejects it.
#include "game_profile.hpp"
#include "gate_state.hpp"
#include "json.hpp"
#include "takaro_store.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace takaro {

// A failed request. On Generic protocol version 0 no frame is sent for it (Takaro's
// v0 void actions would resolve any frame as success); on version 1 it becomes a
// structured error response with one of Takaro's codes.
struct Withhold : std::runtime_error {
  std::string code;
  explicit Withhold(const std::string& why, std::string error_code = {})
      : std::runtime_error(why), code(error_code.empty() ? classify(why) : std::move(error_code)) {}

  static std::string classify(const std::string& why) {
    if (why.find("is unavailable on this build") != std::string::npos ||
        why.find("lookup is unavailable") != std::string::npos ||
        why.find("dimension is unavailable") != std::string::npos)
      return "unsupported";
    for (const char* marker : {"must be", "requires", "is required", "accepts no", "contains controls", "exceeds 1024",
                               "invalid JSON", "invalid console command", "identities conflict"})
      if (why.find(marker) != std::string::npos) return "invalid_args";
    return "game_error";
  }
};

[[noreturn]] inline void withhold(const std::string& why) { throw Withhold(why); }

constexpr const char* kPermanentSentinel = "3021-01-01T00:00:00.000Z";

// ---- argument helpers (absent and explicit JSON null are the same) ----

inline const json::Value& empty_object() { static const json::Value v = json::Value::object(); return v; }

inline const json::Value& as_record(const json::Value* v) {
  return v && v->is_object() ? *v : empty_object();
}

// Takaro sends args as [], {}, a JSON string ("{}", "[]", "{...}"), "" or null.
inline json::Value normalize_args(const json::Value* raw) {
  if (!raw || raw->is_null()) return json::Value::object();
  json::Value parsed = *raw;
  if (raw->is_string()) {
    std::string text = raw->as_string();
    const size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return json::Value::object();
    auto value = json::parse(text);
    if (!value) withhold("args contain invalid JSON");
    parsed = *value;
  }
  if (parsed.is_array()) {
    if (parsed.as_array().empty()) return json::Value::object();
    withhold("args must be an object or empty array");
  }
  if (parsed.is_object()) return parsed;
  withhold("args must be an object or empty array");
}

inline std::optional<std::string> steam64(const json::Value* v) {
  if (!v || !v->is_string()) return std::nullopt;
  std::string id = v->as_string();
  if (id.size() > 6) {
    std::string prefix = id.substr(0, 6);
    for (auto& c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (prefix == "steam:") id = id.substr(6);
  }
  if (!valid_steam64(id)) return std::nullopt;
  return id;
}

inline std::string player_id(const json::Value& args) {
  for (const json::Value* object : {&args, &as_record(args.get("player")), &as_record(args.get("playerRef"))}) {
    for (const char* key : {"gameId", "steamId", "platformId", "playerId"})
      if (auto id = steam64(object->get(key))) return *id;
  }
  withhold("a Steam64 player identifier is required");
}

// Decodes UTF-8 (already validated by the JSON parser) into code points.
inline std::vector<uint32_t> code_points(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t code; size_t count;
    if (c < 0x80) { code = c; count = 0; }
    else if (c < 0xe0) { code = c & 31; count = 1; }
    else if (c < 0xf0) { code = c & 15; count = 2; }
    else { code = c & 7; count = 3; }
    ++i;
    for (size_t k = 0; k < count && i < s.size(); ++k, ++i) code = (code << 6) | (static_cast<unsigned char>(s[i]) & 63);
    out.push_back(code);
  }
  return out;
}

inline bool js_space(uint32_t c) {
  return c == 0x20 || (c >= 0x09 && c <= 0x0d) || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

// JavaScript String.prototype.trim() semantics.
inline std::string js_trim(const std::string& s) {
  const auto cps = code_points(s);
  size_t b = 0, e = cps.size();
  while (b < e && js_space(cps[b])) ++b;
  while (e > b && js_space(cps[e - 1])) --e;
  std::string out;
  for (size_t i = b; i < e; ++i) json::append_utf8(out, cps[i]);
  return out;
}

inline bool safe_integer(const json::Value* v) {
  return v && v->is_number() && std::floor(v->as_number()) == v->as_number() &&
         std::fabs(v->as_number()) <= 9007199254740991.0;
}

inline double finite_number(const json::Value* v, const char* label, double limit) {
  if (!v || !v->is_number() || !std::isfinite(v->as_number()) || std::fabs(v->as_number()) > limit)
    withhold(std::string(label) + " requires a finite number");
  return v->as_number();
}

// ^(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$
inline bool unsigned_decimal(const std::string& t) {
  size_t i = 0;
  auto digits = [&] { const size_t s = i; while (i < t.size() && t[i] >= '0' && t[i] <= '9') ++i; return i - s; };
  if (i < t.size() && t[i] == '0') ++i;
  else if (i < t.size() && t[i] >= '1' && t[i] <= '9') digits();
  else return false;
  if (i < t.size() && t[i] == '.') { ++i; if (!digits()) return false; }
  if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {
    ++i;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
    if (!digits()) return false;
  }
  return i == t.size();
}

// TheIsland's PlayerStarts carry no PlayerStartTag, so the native name is the object
// path. Players know these as numbered spawn points: ".PlayerStart_10" -> "Spawn point 10".
inline std::string location_name(const std::string& code, const std::string& name) {
  if (name != code) return name;
  const size_t dot = code.rfind('.');
  const std::string actor = dot == std::string::npos ? code : code.substr(dot + 1);
  constexpr std::string_view prefix = "PlayerStart_";
  if (actor.rfind(prefix, 0) == 0 && actor.size() > prefix.size() &&
      actor.find_first_not_of("0123456789", prefix.size()) == std::string::npos)
    return "Spawn point " + actor.substr(prefix.size());
  if (actor == "PlayerStart") return "Spawn point";
  return actor.empty() ? name : actor;
}

inline bool no_options(const json::Value& args) {
  for (const auto& [key, value] : args.as_object()) if (!value.is_null()) return false;
  return true;
}

inline json::Value player_json(const std::string& id, const std::string& name,
                               const std::string& ip = {}, std::optional<int32_t> ping = std::nullopt) {
  json::Value out = json::obj({{"gameId", json::str(id)}, {"name", json::str(name.empty() ? id : name)},
                               {"steamId", json::str(id)}, {"platformId", json::str("steam:" + id)}});
  // Only a live connection's verified values; never a placeholder.
  if (!ip.empty()) out.set("ip", json::str(ip));
  if (ping && *ping >= 0) out.set("ping", json::num(*ping));
  return out;
}

// ---- bans ----

class BanManager {
 public:
  BanManager(gate::Server& gate, BanFile file, KnownPlayers& known)
      : gate_(gate), file_(std::move(file)), known_(known) {
    state_ = file_.load();  // throws on corrupt metadata: the bridge refuses to start blind
  }

  void ban(const std::string& id, const json::Value* reason_raw, const json::Value* expires_raw) {
    const std::string reason = checked_reason(reason_raw);
    (void)checked_expiry(expires_raw);
    std::lock_guard<std::mutex> lock(mutex_);
    BanMeta meta;
    meta.reason = reason;
    meta.expires_at = checked_expiry(expires_raw);
    meta.pending = true;
    meta.pending_since = now_ms();
    const auto previous = state_.find(id) == state_.end() ? std::nullopt : std::optional<BanMeta>(state_[id]);
    set(id, meta);  // intent is durable before the native mutation can take effect
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::ban;
    action->player_id = id;
    const auto outcome = gate_.submit(action, std::chrono::milliseconds(3000), std::chrono::milliseconds(4500));
    if (outcome.status != gate::Server::Submit::done || !outcome.success) {
      // A prior managed ban stays valid when a metadata update fails. For a new
      // ban keep the pending intent in case a timed-out native call took effect.
      if (previous) set(id, *previous);
      withhold("native ban effect was not verified");
    }
    meta.pending = false;
    meta.pending_since = 0;
    set(id, meta);
  }

  void unban(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!native_unban(id)) withhold("native unban effect was not verified");
    erase(id);
  }

  struct Row { std::string id, reason; std::optional<std::string> expires_at; };

  std::vector<Row> list() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto ids = reconcile_locked();
    std::vector<Row> rows;
    for (const auto& id : ids) {
      const auto it = state_.find(id);
      rows.push_back({id, it == state_.end() ? std::string() : it->second.reason,
                      it == state_.end() ? std::nullopt : it->second.expires_at});
    }
    return rows;
  }

  // Periodic: lifts expired timed bans, promotes confirmed pending ones.
  bool reconcile() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      (void)reconcile_locked();
      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool ready() const { return ready_.load(); }

 private:
  gate::Server& gate_;
  BanFile file_;
  KnownPlayers& known_;
  std::mutex mutex_;
  std::map<std::string, BanMeta> state_;
  std::atomic<bool> ready_{false};

  void set(const std::string& id, const BanMeta& meta) {
    auto next = state_;
    next[id] = meta;
    if (!file_.save(next)) withhold("ban metadata could not be persisted");
    state_ = std::move(next);
  }
  void erase(const std::string& id) {
    auto next = state_;
    next.erase(id);
    if (!file_.save(next)) withhold("ban metadata could not be persisted");
    state_ = std::move(next);
  }

  bool native_unban(const std::string& id) {
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::unban;
    action->player_id = id;
    const auto outcome = gate_.submit(action, std::chrono::milliseconds(3000), std::chrono::milliseconds(4500));
    return outcome.status == gate::Server::Submit::done && outcome.success;
  }

  static std::string checked_reason(const json::Value* raw) {
    if (!raw) return {};
    if (!raw->is_string() || codepoints(raw->as_string()) > 10000 || raw->as_string().size() > 40000)
      withhold("banPlayer reason must be text within 10000 characters");
    return raw->as_string();
  }

  static std::optional<std::string> checked_expiry(const json::Value* raw) {
    if (!raw) return std::nullopt;
    if (raw->is_string() && raw->as_string() == kPermanentSentinel) return std::nullopt;
    if (!raw->is_string()) withhold("banPlayer expiresAt must be an ISO 8601 date with a timezone");
    const auto until = parse_iso_ms(raw->as_string());
    if (!until) withhold("banPlayer expiresAt must be an ISO 8601 date with a timezone");
    if (*until <= now_ms()) withhold("banPlayer expiresAt must be in the future");
    return iso_ms(*until);
  }

  std::set<std::string> native_bans() {
    const auto ids = gate_.bans();
    if (!ids || ids->size() > 4096) withhold("native ban collection is unavailable");
    std::set<std::string> out;
    for (const auto& id : *ids) {
      if (!valid_steam64(id) || !out.insert(id).second) withhold("native ban collection is invalid");
    }
    return out;
  }

  std::set<std::string> reconcile_locked() {
    ready_ = false;
    auto ids = native_bans();
    const auto snapshot = state_;
    for (const auto& [id, meta] : snapshot) {
      if (!ids.count(id)) {
        // A timed-out native action can finish after an arbitrarily long game
        // thread stall. Keep pending intent until its effect or an explicit
        // unban is observed.
        if (!meta.pending) erase(id);
        continue;
      }
      if (meta.expires_at) {
        const auto until = parse_iso_ms(*meta.expires_at);
        if (until && *until <= now_ms()) {
          if (!native_unban(id)) withhold("expired native ban could not be removed");
          ids.erase(id);
          erase(id);
          continue;
        }
      }
      if (meta.pending) {
        BanMeta active = meta;
        active.pending = false;
        active.pending_since = 0;
        set(id, active);
      }
    }
    for (const auto& id : ids) {
      // Takaro's ban sync merges listBans reason/expiry into managed rows. An
      // unknown native ID cannot safely be projected as blank/permanent.
      if (!state_.count(id)) withhold("native ban metadata is unknown; listBans requires migration");
    }
    ready_ = true;
    return ids;
  }
};

// ---- the Generic action surface ----

class Adapter {
 public:
  Adapter(gate::Server& gate, BanManager& bans, KnownPlayers& known) : gate_(gate), bans_(bans), known_(known) {}

  // Blocks until the event stream has delivered up to `seq` or the timeout passes.
  using DeliveryWaiter = std::function<bool(uint64_t, std::chrono::milliseconds)>;
  void set_delivery_waiter(DeliveryWaiter waiter) { wait_delivered_ = std::move(waiter); }

  // Called after the response for `shutdown` is written to the socket.
  using AfterWrite = std::function<void()>;

  // The icon declared in game.profile (null = none declared, getGameIcon is refused).
  void set_game_icon(std::shared_ptr<const GameIcon> icon) { icon_ = std::move(icon); }

  json::Value handle(const std::string& action, const json::Value& args, AfterWrite* after_write = nullptr) {
    if (action == "testReachability") return test_reachability();
    if (action == "getPlayers") {
      json::Value out = json::Value::array();
      for (const auto& p : gate_.players()) {
        known_.remember(p.id, p.name);
        out.push(player_json(p.id, p.name, p.ip, p.ping));
      }
      return out;
    }
    if (action == "getPlayer") {
      // Takaro calls getPlayer for offline players too and rejects null, {}
      // and error frames alike: always answer with a real IGamePlayer.
      const std::string id = player_id(args);
      if (auto online = gate_.player(id)) return player_json(id, online->name, online->ip, online->ping);
      return player_json(id, known_.name(id));
    }
    if (action == "getPlayerLocation") {
      const auto position = gate_.location(player_id(args));
      if (!position || !std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z))
        withhold("native player location is unavailable");
      return json::obj({{"x", json::num(position->x)}, {"y", json::num(position->y)}, {"z", json::num(position->z)}});
    }
    if (action == "getPlayerInventory") {
      const auto items = gate_.inventory(player_id(args));
      if (!items) withhold("native inventory is unavailable");
      json::Value out = json::Value::array();
      for (const auto& item : *items) {
        if (item.code.empty() || item.name.empty() || item.amount < 1)
          withhold("native inventory item has no verified code, name or quantity");
        out.push(json::obj({{"code", json::str(item.code)}, {"name", json::str(item.name)},
                            {"amount", json::num(item.amount)}}));
      }
      return out;
    }
    if (action == "giveItem") return give_item(args);
    if (action == "teleportPlayer") return teleport(args);
    if (action == "listItems" || action == "listEntities") return catalog(action == "listEntities", args);
    if (action == "listLocations") return list_locations(args);
    if (action == "kickPlayer" || action == "banPlayer" || action == "unbanPlayer") return moderate(action, args);
    if (action == "listBans") {
      if (!no_options(args)) withhold("listBans accepts no options");
      json::Value out = json::Value::array();
      for (const auto& row : bans_.list()) {
        const std::string name = known_.name(row.id);
        out.push(json::obj({{"player", player_json(row.id, name)}, {"reason", json::str(row.reason)},
                            {"expiresAt", row.expires_at ? json::str(*row.expires_at) : json::Value::null()}}));
      }
      return out;
    }
    if (action == "executeConsoleCommand") return console(args);
    if (action == "sendMessage") return send_message(args);
    if (action == "shutdown") return shutdown(args, after_write);
    if (action == "getGameIcon") {
      if (!icon_) withhold("getGameIcon is unavailable on this build (no icon declared)");
      return game_icon_response(*icon_);
    }
    withhold("action '" + action.substr(0, 64) + "' is unavailable on this build");
  }

 private:
  gate::Server& gate_;
  BanManager& bans_;
  KnownPlayers& known_;
  DeliveryWaiter wait_delivered_;
  std::shared_ptr<const GameIcon> icon_;

  json::Value test_reachability() {
    const auto h = gate_.health();
    const bool ok = h.ready && !h.boot_id.empty();
    // Chat hooks install on the first player's chat call, so "chat starting" before
    // anyone joined is the normal state, not a degradation worth reporting.
    std::vector<std::string> degraded;
    if (!h.items) degraded.push_back("items=unavailable");
    if (!h.entities) degraded.push_back("entities=unavailable");
    std::string reason;
    for (const auto& d : degraded) reason += (reason.empty() ? "" : ", ") + d;
    json::Value out = json::Value::object();
    out.set("connectable", json::Value::boolean(ok));
    if (!ok) out.set("reason", json::str("Native status starting"));
    else out.set("reason", degraded.empty() ? json::Value::null() : json::str("Native capabilities: " + reason));
    return out;
  }

  static gate::Server::Outcome run(gate::Server& gate, std::shared_ptr<gate::Action> action) {
    return gate.submit(std::move(action), std::chrono::milliseconds(3000), std::chrono::milliseconds(4500));
  }

  json::Value give_item(const json::Value& args) {
    const json::Value& item = as_record(args.get("item"));
    std::string code;
    for (const json::Value* v : {args.get("item"), args.get("itemCode"), args.get("code"), args.get("name"), item.get("code")}) {
      if (v && v->is_string() && !js_trim(v->as_string()).empty()) { code = v->as_string(); break; }
    }
    const bool quoted_start = code.rfind("Blueprint'", 0) == 0;
    const bool quoted_end = !code.empty() && code.back() == '\'';
    if (code.empty() || !gate::valid_item_code(code) || quoted_start != quoted_end)
      withhold("giveItem requires a native /Game item class path");
    const json::Value* amount_raw = args.get("amount");
    if (!amount_raw) amount_raw = args.get("quantity");
    double amount = 1;
    if (amount_raw) {
      if (!safe_integer(amount_raw)) withhold("giveItem amount must be an integer");
      amount = amount_raw->as_number();
    }
    if (amount < 1 || amount > 10000) withhold("giveItem amount must be an integer from 1 to 10000");
    double quality = 0;
    if (const json::Value* q = args.get("quality")) {
      if (q->is_string()) {
        const std::string t = js_trim(q->as_string());
        if (!unsigned_decimal(t)) withhold("giveItem quality must be a finite nonnegative number");
        char* end = nullptr;
        quality = std::strtod(t.c_str(), &end);
        if (!end || *end) withhold("giveItem quality must be a finite nonnegative number");
      } else {
        quality = finite_number(q, "giveItem quality", 100000);
      }
      if (!std::isfinite(quality) || quality < 0 || quality > 100000)
        withhold("giveItem quality must be a finite nonnegative number");
    }
    bool blueprint = false;
    if (const json::Value* b = args.get("blueprint")) {
      if (!b->is_bool()) withhold("giveItem blueprint must be boolean");
      blueprint = b->as_bool();
    }
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::give_item;
    action->player_id = player_id(args);
    action->item_code = code;
    action->amount = static_cast<int32_t>(amount);
    action->quality = static_cast<float>(quality);
    action->blueprint = blueprint;
    const auto outcome = run(gate_, action);
    if (outcome.status != gate::Server::Submit::done || !outcome.success) withhold("native item grant was not verified");
    return json::Value::object();
  }

  json::Value teleport(const json::Value& args) {
    if (const json::Value* target = args.get("target")) {
      if (!(target->is_string() && target->as_string().empty()))
        withhold("teleportPlayer target lookup is unavailable");
    }
    if (args.get("dimension")) withhold("teleportPlayer dimension is unavailable");
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::teleport;
    action->x = static_cast<float>(finite_number(args.get("x"), "teleportPlayer x", 1e8));
    action->y = static_cast<float>(finite_number(args.get("y"), "teleportPlayer y", 1e8));
    action->z = static_cast<float>(finite_number(args.get("z"), "teleportPlayer z", 1e8));
    action->player_id = player_id(args);
    const auto outcome = run(gate_, action);
    if (outcome.status != gate::Server::Submit::done || !outcome.success) withhold("native teleport was not verified");
    return json::Value::object();
  }

  json::Value catalog(bool entities, const json::Value& args) {
    const json::Value* search = args.get("search");
    if (search && !search->is_string()) withhold("catalog search must be text");
    const auto rows = entities ? gate_.entities() : gate_.items();
    if (!rows || rows->empty() || rows->size() > 65536) withhold("native catalog is unavailable");
    std::string term = search ? js_trim(search->as_string()) : std::string();
    std::transform(term.begin(), term.end(), term.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::set<std::string> seen;
    json::Value out = json::Value::array();
    for (const auto& row : *rows) {
      const std::string code = js_trim(row.code), name = js_trim(row.name);
      if (code.empty() || name.empty()) withhold("native catalog entry has no verified code and name");
      if (!seen.insert(code).second) withhold("native catalog contains duplicate codes");
      if (!term.empty()) {
        std::string lc = code, ln = name;
        std::transform(lc.begin(), lc.end(), lc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(ln.begin(), ln.end(), ln.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lc.find(term) == std::string::npos && ln.find(term) == std::string::npos) continue;
      }
      out.push(json::obj({{"code", json::str(code)}, {"name", json::str(name)}}));
    }
    return out;
  }

  json::Value list_locations(const json::Value& args) {
    if (!no_options(args)) withhold("listLocations accepts no options");
    const auto rows = gate_.locations();
    if (!rows || rows->empty() || rows->size() > 1024) withhold("native spawn-point snapshot is unavailable");
    std::set<std::string> seen;
    json::Value out = json::Value::array();
    for (const auto& row : *rows) {
      const bool ok = !js_trim(row.code).empty() && row.code.size() <= 512 && !js_trim(row.name).empty() &&
          row.name.size() <= 512 && std::isfinite(row.x) && std::isfinite(row.y) && std::isfinite(row.z) &&
          std::fabs(row.x) <= 1e9f && std::fabs(row.y) <= 1e9f && std::fabs(row.z) <= 1e9f &&
          std::isfinite(row.size_x) && std::isfinite(row.size_y) && std::isfinite(row.size_z) &&
          row.size_x > 0 && row.size_y > 0 && row.size_z > 0 &&
          row.size_x <= 1e9f && row.size_y <= 1e9f && row.size_z <= 1e9f;
      if (!ok) withhold("native spawn-point bounds are invalid");
      if (!seen.insert(row.code).second) withhold("native spawn-point snapshot has duplicate codes");
      out.push(json::obj({{"code", json::str(row.code)}, {"name", json::str(location_name(row.code, row.name))},
                          {"position", json::obj({{"x", json::num(row.x)}, {"y", json::num(row.y)}, {"z", json::num(row.z)}})},
                          {"sizeX", json::num(row.size_x)}, {"sizeY", json::num(row.size_y)}, {"sizeZ", json::num(row.size_z)}}));
    }
    return out;
  }

  bool send(const std::string& recipient, const std::string& text) {
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::message;
    action->player_id = recipient;
    if (!gate::utf8_to_utf32(text, action->text)) return false;
    const auto outcome = run(gate_, action);
    return outcome.status == gate::Server::Submit::done && outcome.success;
  }

  json::Value moderate(const std::string& action_name, const json::Value& args) {
    const json::Value* reason_raw = args.get("reason");
    if (reason_raw && !reason_raw->is_string()) withhold(action_name + " reason must be text");
    const std::string reason = reason_raw ? reason_raw->as_string() : std::string();
    if (action_name == "kickPlayer") {
      const auto cps = code_points(reason);
      bool bad = reason.size() > 1000 || cps.size() > 480;
      for (uint32_t c : cps)
        if (c <= 0x1f || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029) bad = true;
      if (bad) withhold("kickPlayer reason must be a single line of at most 480 characters");
    } else if (action_name == "unbanPlayer" && !js_trim(reason).empty()) {
      withhold("unbanPlayer accepts no reason");
    }
    const std::string id = player_id(args);
    if (action_name == "banPlayer") {
      bans_.ban(id, reason_raw, args.get("expiresAt"));
      return json::Value::object();
    }
    if (args.get("expiresAt")) withhold(action_name + " accepts no expiresAt");
    if (action_name == "unbanPlayer") {
      bans_.unban(id);
      return json::Value::object();
    }
    if (!js_trim(reason).empty() && !send(id, "Kick reason: " + reason))
      withhold("native kick reason notice was not delivered");
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::kick;
    action->player_id = id;
    const auto outcome = run(gate_, action);
    if (outcome.status != gate::Server::Submit::done || !outcome.success) withhold("native kick effect was not verified");
    return json::Value::object();
  }

  json::Value console(const json::Value& args) {
    const json::Value* raw = args.get("command");
    if (!raw || !raw->is_string() || js_trim(raw->as_string()).empty() || raw->as_string().size() > 4096)
      withhold("executeConsoleCommand requires nonempty text within 4096 UTF-8 bytes");
    const std::string& command = raw->as_string();
    const auto cps = code_points(command);
    if (cps.size() > 1024) withhold("executeConsoleCommand exceeds 1024 code points");
    for (uint32_t c : cps)
      if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029)
        withhold("executeConsoleCommand contains controls");
    std::wstring decoded;
    if (!ark_engine_exec::decode_command(command, decoded)) withhold("invalid console command");
    auto failure = [](const char* message) {
      return json::obj({{"success", json::Value::boolean(false)}, {"rawResult", json::str("")},
                        {"errorMessage", json::str(message)}});
    };
    if (ark_shutdown_request::is_termination_command(command)) return failure("Use the dedicated shutdown action");
    if (ark_shutdown_request::contains_unicode_space(command)) return failure("Unsupported command whitespace");
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::console;
    action->command = command;
    const auto outcome = run(gate_, action);
    if (outcome.status == gate::Server::Submit::not_ready) return failure("game thread not ready");
    if (outcome.status == gate::Server::Submit::unavailable) return failure("queue full");
    if (outcome.status == gate::Server::Submit::timeout) return failure("game thread timeout");
    if (outcome.output.size() > 32768) withhold("native console output exceeds the transport bound");
    if (!outcome.success) {
      return json::obj({{"success", json::Value::boolean(false)}, {"rawResult", json::str(outcome.output)},
                        {"errorMessage", json::str("native console rejected or unhandled")}});
    }
    return json::obj({{"success", json::Value::boolean(true)}, {"rawResult", json::str(outcome.output)},
                      {"errorMessage", json::Value::null()}});
  }

  std::string message_recipient(const json::Value& args) {
    const json::Value& opts = as_record(args.get("opts"));
    const json::Value* raw = opts.get("recipient");
    if (raw && !raw->is_object()) withhold("sendMessage recipient must be a player reference");
    const json::Value& recipient = as_record(raw);
    std::vector<const json::Value*> values;
    for (const json::Value* v : {args.get("recipientGameId"), recipient.get("gameId"), recipient.get("steamId"),
                                 recipient.get("platformId")})
      if (v) values.push_back(v);
    if (raw && values.empty()) withhold("sendMessage recipient requires a Steam64 gameId");
    std::string selected;
    for (const json::Value* v : values) {
      const auto id = steam64(v);
      if (!id) withhold("sendMessage recipient requires a valid Steam64");
      if (!selected.empty() && selected != *id) withhold("sendMessage recipient identities conflict");
      selected = *id;
    }
    return selected;
  }

  json::Value send_message(const json::Value& args) {
    const json::Value* message = args.get("message");
    if (!message || !message->is_string() || js_trim(message->as_string()).empty())
      withhold("sendMessage requires a nonempty message");
    const std::string recipient = message_recipient(args);
    // Takaro does not forward serverChatName to Generic connectors; modules may
    // pass opts.senderNameOverride. ARK's direct chat line has no sender field.
    std::string text = message->as_string();
    if (const json::Value* sender = as_record(args.get("opts")).get("senderNameOverride");
        sender && sender->is_string() && !js_trim(sender->as_string()).empty())
      text = js_trim(sender->as_string()) + ": " + text;
    if (!send(recipient, text))
      withhold(recipient.empty() ? "native chat broadcast was not acknowledged" : "native chat recipient was not acknowledged");
    return json::Value::object();
  }

  json::Value shutdown(const json::Value& args, AfterWrite* after_write) {
    if (!no_options(args)) withhold("shutdown accepts no options");
    auto action = std::make_shared<gate::Action>();
    action->kind = gate::Action::Kind::shutdown;
    const auto outcome = run(gate_, action);
    if (outcome.status != gate::Server::Submit::done || !outcome.success) withhold("native shutdown was not acknowledged");
    // ARK does not run Logout for players still online at exit. The world is saved
    // and the exit is staged, so report their departures now and give the event
    // stream a bounded moment to deliver them before the acknowledged exit.
    bool departures = false;
    for (const auto& player : gate_.players()) { gate_.record_logout(player.id); departures = true; }
    if (departures && wait_delivered_) (void)wait_delivered_(gate_.latest_sequence(), std::chrono::milliseconds(3000));
    // The exit is requested on a later game tick only after this response
    // has been handed to the socket.
    if (after_write) *after_write = [action] { action->response_sent.store(true, std::memory_order_release); };
    return json::Value::object();
  }
};

}  // namespace takaro
