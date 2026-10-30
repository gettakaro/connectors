#pragma once

#include "engine_exec_bindings.hpp"
#include "shutdown_request_bindings.hpp"


#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gate {
// Shared state between the game thread and the Takaro bridge threads. Every
// game read and RPC happens on the hook's game thread; the bridge only reads
// copied snapshots and queues actions here.
struct Action {
  enum class Kind { message, give_item, teleport, console, kick, ban, unban, shutdown } kind = Kind::message;
  std::u32string text;
  std::u32string sender;  // message: the chat sender name; empty = ARK's server line
  std::string player_id;
  std::string item_code;
  std::string output;
  std::string command;
  int32_t amount = 0;
  float quality = 0;
  bool blueprint = false;
  float x = 0, y = 0, z = 0;
  std::mutex mutex;
  std::condition_variable done_cv;
  bool done = false;
  bool cancelled = false;
  bool executing = false;
  bool success = false;
  std::atomic<bool> response_sent{false};
};

inline bool valid_item_code(const std::string& code) {
  if (code.empty() || code.size() > 512) return false;
  std::string path = code;
  if (path.rfind("Blueprint'", 0) == 0 && path.size() > 11 && path.back() == '\'')
    path = path.substr(10, path.size() - 11);
  if (path.rfind("/Game/", 0) != 0 || !path.ends_with("_C")) return false;
  return std::all_of(path.begin(), path.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '/' || c == '_' || c == '.';
  });
}

struct Player {
  std::string id;
  std::string name;
  std::string ip;              // remote address of the live connection, empty when unverified
  std::optional<int32_t> ping; // milliseconds, absent until the game thread measured it
};

struct Position {
  float x;
  float y;
  float z;
  std::chrono::steady_clock::time_point sampled;
  int32_t actor_index = -1;
  int32_t actor_serial = 0;
};

struct InventoryItem {
  std::string code;
  std::string name;
  int32_t amount;
};

struct CatalogItem {
  std::string code;
  std::string name;
};
struct LocationItem {
  std::string code;
  std::string name;
  float x, y, z;
  float size_x, size_y, size_z;
};

struct InventorySnapshot {
  std::vector<InventoryItem> items;
  std::chrono::steady_clock::time_point sampled;
};

struct Event {
  uint64_t seq;
  std::string type;
  std::string id;
  std::string name;
  std::string message;
  std::string attacker_id;
  std::string entity_code;
  std::string entity_name;
  std::string weapon; // entity-killed: held weapon display name, empty when unarmed or unverified
  std::string ip;              // player-connected: the connection's verified remote address
  std::optional<int32_t> ping; // player-connected: milliseconds, when already measured
  std::string timestamp;
  // Actual game-thread position captured at the event, retained exactly as
  // long as this bounded event remains replayable.
  std::optional<Position> position;
  bool position_from_death = false;
};

inline std::string escape(const std::string& value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if (c < 32) { out += "\\u00"; out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

inline bool utf8_to_utf32(const std::string& input, std::u32string& out) {
  if (input.empty() || input.size() > 1024) return false;
  for (size_t i = 0; i < input.size();) {
    const unsigned char c = input[i++];
    uint32_t code = c;
    unsigned count = 0;
    uint32_t minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) { code = c & 31; count = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { code = c & 15; count = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { code = c & 7; count = 3; minimum = 0x10000; }
    else if (c >= 0x80) return false;
    if (i + count > input.size()) return false;
    for (unsigned j = 0; j < count; ++j) {
      const unsigned char next = input[i++];
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    if (code == 0 || (code < 32 && code != '\n' && code != '\t')) return false;
    out.push_back(static_cast<char32_t>(code));
    if (out.size() > 512) return false;
  }
  return true;
}

inline std::string utf32_to_utf8(const wchar_t* value, size_t count) {
  std::string out;
  if (!value || count > 512) return out;
  for (size_t i = 0; i < count; ++i) {
    uint32_t c = static_cast<uint32_t>(value[i]);
    if (c == 0) break;
    if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
    if (c < 32 && c != '\t') c = ' ';
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else if (c < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (c >> 6)));
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    } else if (c < 0x10000 && !(c >= 0xd800 && c <= 0xdfff)) {
      out.push_back(static_cast<char>(0xe0 | (c >> 12)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    } else if (c <= 0x10ffff) {
      out.push_back(static_cast<char>(0xf0 | (c >> 18)));
      out.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
      out.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
      out.push_back(static_cast<char>(0x80 | (c & 63)));
    }
  }
  return out;
}

inline std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto sec = std::chrono::system_clock::to_time_t(now);
  struct tm tm{};
  gmtime_r(&sec, &tm);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return stamp;
}

class Server {
 public:
  explicit Server(std::string boot_id) : boot_id_(std::move(boot_id)) {}

  void set_game_ready() { ready_.store(true, std::memory_order_release); }

  void record_chat(const std::string& id, const std::string& name, const std::string& message) {
    if (id.empty() || message.empty()) return;
    Event event{};
    event.id = id;
    event.type = "chat-message";
    event.name = name.empty() ? id : name;
    event.message = message;
    event.timestamp = utc_now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto& player = players_[id];
    player.id = id;
    player.name = event.name;  // keep the sampled ip/ping of an online player
    event.seq = ++sequence_;
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
  }

  void record_player_death(const std::string& id, const std::string& name,
                           const std::string& attacker_id) {
    if (id.empty()) return;
    Event event{};
    event.id = id;
    event.name = name.empty() ? id : name;
    event.type = "player-death";
    if (attacker_id != id) event.attacker_id = attacker_id;
    event.timestamp = utc_now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!players_.contains(id)) return;
    if (auto position = death_positions_.find(id); position != death_positions_.end()) {
      if (std::chrono::steady_clock::now() - position->second.sampled < std::chrono::seconds(2))
        event.position = position->second;
      death_positions_.erase(position);
    }
    event.seq = ++sequence_;
    if (event.position) death_event_seq_[id] = event.seq;
    else death_event_seq_.erase(id);
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
  }

  std::string entity_name_for_code(const std::string& code) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entities_ready_) return {};
    for (const auto& entity : entity_items_)
      if (entity.code == code) return entity.name;
    return {};
  }

  void record_entity_killed(const std::string& killer_id, const std::string& code,
                            const std::string& entity_name, const std::string& weapon = {}) {
    if (killer_id.empty() || code.empty() || entity_name.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto killer = players_.find(killer_id);
    if (killer == players_.end() || !entities_ready_) return;
    const auto known = std::find_if(entity_items_.begin(), entity_items_.end(),
        [&](const auto& item) { return item.code == code && item.name == entity_name; });
    if (known == entity_items_.end()) return;
    Event event{};
    event.id = killer_id;
    event.name = killer->second.name;
    event.type = "entity-killed";
    event.entity_code = code;
    event.entity_name = entity_name;
    event.weapon = weapon.size() <= 128 ? weapon : std::string();
    event.timestamp = utc_now();
    event.seq = ++sequence_;
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
  }

  void record_log(const std::string& message) {
    if (message.empty() || message.size() > 512) return;
    Event event{};
    event.type = "log";
    event.message = message;
    event.timestamp = utc_now();
    std::lock_guard<std::mutex> lock(mutex_);
    event.seq = ++sequence_;
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
  }

  void set_chat_ready() { chat_ready_.store(true, std::memory_order_release); }

  uint64_t latest_sequence() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
  }

  void record_death_position(const std::string& id, float x, float y, float z,
                             int32_t actor_index = -1, int32_t actor_serial = 0) {
    if (id.empty() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::abs(x) > 1e9f || std::abs(y) > 1e9f || std::abs(z) > 1e9f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (players_.contains(id))
      death_positions_[id] = Position{x, y, z, std::chrono::steady_clock::now(),
                                      actor_index, actor_serial};
  }

  void record_login(const std::string& id, const std::string& name, const std::string& ip = {},
                    std::optional<int32_t> ping = std::nullopt) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    departed_positions_.erase(id);
    death_positions_.erase(id);
    death_event_seq_.erase(id);
    const std::string display = name.empty() ? id : name;
    if (players_.find(id) != players_.end()) return;
    players_[id] = Player{id, display, ip, ping};
    Event event{};
    event.id = id;
    event.name = display;
    event.ip = ip;
    event.ping = ping;
    event.type = "player-connected";
    event.timestamp = utc_now();
    event.seq = ++sequence_;
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
  }

  void record_logout(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(id);
    if (it == players_.end()) return;
    Event event{};
    event.id = id;
    event.name = it->second.name;
    event.type = "player-disconnected";
    event.timestamp = utc_now();
    // A direct pre-Logout capture wins. A recent live game-thread sample is
    // the fallback when the engine has already detached the pawn.
    if (auto direct = departed_positions_.find(id); direct != departed_positions_.end() &&
        std::chrono::steady_clock::now() - direct->second.sampled < std::chrono::seconds(2)) {
      event.position = direct->second;
    } else if (auto recent = positions_.find(id); recent != positions_.end() &&
               std::chrono::steady_clock::now() - recent->second.sampled < std::chrono::seconds(2)) {
      event.position = recent->second;
    } else if (auto active = death_event_seq_.find(id); active != death_event_seq_.end()) {
      // A player may leave after death has detached the pawn. The still-active
      // death event is the last verified position of this same life, not a
      // fabricated Logout sample. A respawned pawn clears this marker.
      auto death = std::find_if(events_.rbegin(), events_.rend(), [&](const Event& prior) {
        return prior.seq == active->second;
      });
      if (death != events_.rend() && death->position) {
        event.position = death->position;
        event.position_from_death = true;
      }
    }
    event.seq = ++sequence_;
    events_.push_back(std::move(event));
    while (events_.size() > 4096) events_.pop_front();
    departed_positions_.erase(id);
    death_positions_.erase(id);
    death_event_seq_.erase(id);
    players_.erase(it);
    positions_.erase(id);
    inventories_.erase(id);
  }

  // The engine thread captures this from the live pawn immediately before
  // Logout invalidates the controller. It is the departure event's actual
  // final position and moves it into the bounded departure event on Logout.
  // It never makes the player online or permits an action against that actor.
  void record_departure_position(const std::string& id, float x, float y, float z) {
    if (id.empty() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::abs(x) > 1e9f || std::abs(y) > 1e9f || std::abs(z) > 1e9f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (players_.contains(id)) {
      departed_positions_[id] = Position{x, y, z, std::chrono::steady_clock::now()};
    }
  }

  void record_position(const std::string& id, float x, float y, float z,
                       int32_t actor_index = -1, int32_t actor_serial = 0) {
    if (id.empty() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
    if (std::abs(x) > 1e9f || std::abs(y) > 1e9f || std::abs(z) > 1e9f) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (players_.contains(id)) {
      positions_[id] = Position{x, y, z, std::chrono::steady_clock::now(), actor_index, actor_serial};
      // A tick of the dying pawn must not invalidate its own death event.
      // A different verified pawn, or an unverified identity, cannot keep
      // presenting the old death location as this survivor's current one.
      if (auto active = death_event_seq_.find(id); active != death_event_seq_.end()) {
        auto death = std::find_if(events_.rbegin(), events_.rend(), [&](const Event& event) {
          return event.seq == active->second;
        });
        if (death == events_.rend() || !death->position ||
            actor_index < 0 || actor_serial <= 0 || death->position->actor_index < 0 ||
            death->position->actor_serial <= 0 ||
            actor_index != death->position->actor_index ||
            actor_serial != death->position->actor_serial)
          death_event_seq_.erase(active);
      }
    }
  }

  // Game-thread sample of the live connection. An unverified read keeps the
  // previous verified value rather than inventing or erasing one.
  void record_network(const std::string& id, const std::optional<std::string>& ip,
                      std::optional<int32_t> ping) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(id);
    if (it == players_.end()) return;
    if (ip && !ip->empty()) it->second.ip = *ip;
    if (ping && *ping >= 0) it->second.ping = ping;
  }

  void clear_position(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    positions_.erase(id);
  }

  void record_inventory(const std::string& id, std::vector<InventoryItem> items) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (players_.contains(id)) inventories_[id] = InventorySnapshot{std::move(items), std::chrono::steady_clock::now()};
  }

  void record_catalog(std::vector<CatalogItem> items) {
    std::lock_guard<std::mutex> lock(mutex_);
    catalog_items_ = std::move(items);
    catalog_ready_ = true;
  }

  void clear_catalog() {
    std::lock_guard<std::mutex> lock(mutex_);
    catalog_items_.clear();
    catalog_ready_ = false;
  }

  void record_entities(std::vector<CatalogItem> entities) {
    std::lock_guard<std::mutex> lock(mutex_);
    entity_items_ = std::move(entities);
    entities_ready_ = true;
  }

  void clear_entities() {
    std::lock_guard<std::mutex> lock(mutex_);
    entity_items_.clear();
    entities_ready_ = false;
  }

  void record_locations(std::vector<LocationItem> locations) {
    std::lock_guard<std::mutex> lock(mutex_);
    locations_ = std::move(locations);
    locations_ready_ = true;
  }

  void clear_locations() {
    std::lock_guard<std::mutex> lock(mutex_);
    locations_.clear();
    locations_ready_ = false;
  }

  void record_bans(std::vector<std::string> ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    banned_ids_ = std::move(ids);
    bans_ready_ = true;
    bans_sampled_ = std::chrono::steady_clock::now();
  }

  void clear_bans() {
    std::lock_guard<std::mutex> lock(mutex_);
    banned_ids_.clear();
    bans_ready_ = false;
  }

  void clear_inventory(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    inventories_.erase(id);
  }

  // True when the last validated native ban sample contains `id`.
  bool is_banned(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return bans_ready_ && std::find(banned_ids_.begin(), banned_ids_.end(), id) != banned_ids_.end();
  }

  std::vector<std::string> player_ids() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(players_.size());
    for (const auto& entry : players_) ids.push_back(entry.first);
    return ids;
  }

  std::shared_ptr<Action> take_action() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (actions_.empty()) return nullptr;
    auto action = actions_.front();
    actions_.pop_front();
    return action;
  }

  static void complete(const std::shared_ptr<Action>& action, bool success) {
    std::lock_guard<std::mutex> lock(action->mutex);
    if (action->cancelled) return;
    action->success = success;
    action->done = true;
    action->done_cv.notify_all();
  }

  // ---- typed read/action API used by the Takaro bridge (never the game thread) ----
  struct Health {
    bool ready = false;
    bool chat = false;
    bool items = false;
    bool entities = false;
    std::string boot_id;
  };
  Health health() {
    Health h;
    h.ready = ready_.load(std::memory_order_acquire);
    h.chat = h.ready && chat_ready_.load(std::memory_order_acquire);
    h.boot_id = boot_id_;
    std::lock_guard<std::mutex> lock(mutex_);
    h.items = catalog_ready_;
    h.entities = entities_ready_;
    return h;
  }
  const std::string& boot_id() const { return boot_id_; }

  std::vector<Player> players() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Player> out;
    out.reserve(players_.size());
    for (const auto& entry : players_) out.push_back(entry.second);
    return out;
  }
  std::optional<Player> player(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = players_.find(id);
    if (it == players_.end()) return std::nullopt;
    return it->second;
  }

  // Fresh banned-ID sample (at most 3 s old) or nothing.
  std::optional<std::vector<std::string>> bans() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!bans_ready_ || std::chrono::steady_clock::now() - bans_sampled_ > std::chrono::seconds(3))
      return std::nullopt;
    return banned_ids_;
  }

  std::optional<std::vector<LocationItem>> locations() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!locations_ready_) return std::nullopt;
    return locations_;
  }

  struct LocationResult {
    float x = 0, y = 0, z = 0;
    const char* source = nullptr;  // nullptr live, "departure" or "death"
  };
  std::optional<LocationResult> location(const std::string& id) {
    LocationResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = positions_.find(id);
    if (it != positions_.end() &&
        std::chrono::steady_clock::now() - it->second.sampled < std::chrono::seconds(2)) {
      out.x = it->second.x; out.y = it->second.y; out.z = it->second.z;
      return out;
    }
    if (!players_.contains(id)) {
      // Only the latest retained disconnect for this identity may enrich an
      // offline event. An older disconnect cannot stand in for a newer event
      // that lacked an actual position.
      for (auto event = events_.rbegin(); event != events_.rend(); ++event) {
        if (event->id != id || event->type != "player-disconnected") continue;
        if (!event->position) return std::nullopt;
        out.x = event->position->x; out.y = event->position->y; out.z = event->position->z;
        out.source = event->position_from_death ? "death" : "departure";
        return out;
      }
      return std::nullopt;
    }
    // A new login supersedes prior deaths. A fresh live position above always
    // wins after respawn; the retained death position is event-time provenance
    // while its event is still in the replay ring.
    for (auto event = events_.rbegin(); event != events_.rend(); ++event) {
      if (event->id != id) continue;
      if (event->type == "player-connected") break;
      if (event->type == "player-death") {
        auto active = death_event_seq_.find(id);
        if (active != death_event_seq_.end() && active->second == event->seq && event->position) {
          out.x = event->position->x; out.y = event->position->y; out.z = event->position->z;
          out.source = "death";
          return out;
        }
        break;
      }
    }
    return std::nullopt;
  }

  std::optional<std::vector<InventoryItem>> inventory(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inventories_.find(id);
    if (it == inventories_.end() ||
        std::chrono::steady_clock::now() - it->second.sampled >= std::chrono::seconds(3))
      return std::nullopt;
    return it->second.items;
  }

  std::optional<std::vector<CatalogItem>> items() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!catalog_ready_) return std::nullopt;
    return catalog_items_;
  }
  std::optional<std::vector<CatalogItem>> entities() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entities_ready_) return std::nullopt;
    return entity_items_;
  }

  struct EventsPage {
    std::vector<Event> events;
    uint64_t latest = 0;
    bool truncated = false;  // events after `since` were evicted from the ring
  };
  EventsPage events_since(uint64_t since, size_t max = 128) {
    EventsPage page;
    std::lock_guard<std::mutex> lock(mutex_);
    page.latest = sequence_;
    page.truncated = !events_.empty() && since + 1 < events_.front().seq;
    for (const auto& e : events_) {
      if (page.events.size() >= max) break;
      if (e.seq <= since) continue;
      page.events.push_back(e);
    }
    return page;
  }

  enum class Submit { done, rejected, not_ready, unavailable, timeout };
  struct Outcome {
    Submit status = Submit::rejected;
    bool success = false;
    std::string output;
    std::shared_ptr<Action> action;
  };

  // Queues `action` for the game thread and waits. Not yet started after
  // `start_wait` → cancelled. Started → waits for completion until
  // `finish_wait` (the action keeps running on the game thread; the caller
  // must then treat the outcome as unknown and send no success). Never blocks
  // the game thread: it only completes the shared action.
  Outcome submit(std::shared_ptr<Action> action,
                 std::chrono::milliseconds start_wait = std::chrono::milliseconds(3000),
                 std::chrono::milliseconds finish_wait = std::chrono::milliseconds(6000)) {
    Outcome outcome;
    outcome.action = action;
    if (!ready_.load(std::memory_order_acquire)) { outcome.status = Submit::not_ready; return outcome; }
    const bool needs_online = action->kind == Action::Kind::give_item ||
        action->kind == Action::Kind::teleport || action->kind == Action::Kind::kick ||
        (action->kind == Action::Kind::message && !action->player_id.empty());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (actions_.size() >= 128 || (needs_online && !players_.count(action->player_id))) {
        outcome.status = Submit::unavailable;
        return outcome;
      }
      actions_.push_back(action);
    }
    std::unique_lock<std::mutex> lock(action->mutex);
    if (!action->done_cv.wait_for(lock, start_wait, [&] { return action->done; })) {
      if (!action->executing) {
        action->cancelled = true;
        outcome.status = Submit::timeout;
        return outcome;
      }
      if (!action->done_cv.wait_for(lock, finish_wait, [&] { return action->done; })) {
        outcome.status = Submit::timeout;
        return outcome;
      }
    }
    outcome.status = Submit::done;
    outcome.success = action->success;
    outcome.output = action->output;
    return outcome;
  }

 private:
  std::string boot_id_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> chat_ready_{false};
  std::mutex mutex_;
  std::map<std::string, Player> players_;
  std::map<std::string, Position> positions_;
  std::map<std::string, Position> departed_positions_;
  std::map<std::string, Position> death_positions_;
  std::map<std::string, uint64_t> death_event_seq_;
  std::map<std::string, InventorySnapshot> inventories_;
  std::vector<CatalogItem> catalog_items_;
  bool catalog_ready_ = false;
  std::vector<CatalogItem> entity_items_;
  bool entities_ready_ = false;
  std::vector<LocationItem> locations_;
  bool locations_ready_ = false;
  std::vector<std::string> banned_ids_;
  bool bans_ready_ = false;
  std::chrono::steady_clock::time_point bans_sampled_{};
  std::deque<Event> events_;
  std::deque<std::shared_ptr<Action>> actions_;
  uint64_t sequence_ = 0;

};
} // namespace gate
