#pragma once

#include "engine_exec_bindings.hpp"
#include "shutdown_request_bindings.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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
// This is the intentionally narrow transport used to establish the real Takaro
// chat/action gate. Every game read and RPC happens in the hook's game thread;
// this class only reads immutable copies and queues actions.
struct Action {
  enum class Kind { message, give_item, teleport, console, kick, ban, unban, shutdown } kind = Kind::message;
  std::u32string text;
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

// The sidecar sends only flat, bounded action objects. Reject escapes and
// nested values; ARK class paths use printable ASCII and need no JSON escapes.
struct JsonField {
  enum class Kind { string, number, boolean } kind;
  std::string value;
};

inline bool parse_action_object(const std::string& input, std::map<std::string, JsonField>& fields) {
  fields.clear();
  if (input.empty() || input.size() > 2048) return false;
  size_t i = 0;
  const auto spaces = [&] { while (i < input.size() &&
      (input[i] == ' ' || input[i] == '\t' || input[i] == '\r' || input[i] == '\n')) ++i; };
  const auto quoted = [&](std::string& value) -> bool {
    if (i >= input.size() || input[i++] != '"') return false;
    const size_t start = i;
    while (i < input.size() && input[i] != '"') {
      const unsigned char c = static_cast<unsigned char>(input[i]);
      if (c < 0x20 || c > 0x7e || c == '\\') return false;
      ++i;
    }
    if (i >= input.size() || i - start > 1024) return false;
    value.assign(input, start, i++ - start);
    return true;
  };
  spaces();
  if (i >= input.size() || input[i++] != '{') return false;
  spaces();
  if (i < input.size() && input[i] == '}') return false;
  for (;;) {
    spaces();
    std::string key;
    if (!quoted(key) || key.empty() || key.size() > 32 || fields.count(key)) return false;
    spaces();
    if (i >= input.size() || input[i++] != ':') return false;
    spaces();
    JsonField field;
    if (i < input.size() && input[i] == '"') {
      field.kind = JsonField::Kind::string;
      if (!quoted(field.value)) return false;
    } else if (input.compare(i, 4, "true") == 0 &&
               (i + 4 == input.size() || input[i+4] == ',' || input[i+4] == '}' || std::isspace(static_cast<unsigned char>(input[i+4])))) {
      field.kind = JsonField::Kind::boolean; field.value = "true"; i += 4;
    } else if (input.compare(i, 5, "false") == 0 &&
               (i + 5 == input.size() || input[i+5] == ',' || input[i+5] == '}' || std::isspace(static_cast<unsigned char>(input[i+5])))) {
      field.kind = JsonField::Kind::boolean; field.value = "false"; i += 5;
    } else {
      field.kind = JsonField::Kind::number;
      const size_t start = i;
      while (i < input.size() && (std::isdigit(static_cast<unsigned char>(input[i])) ||
             input[i] == '-' || input[i] == '+' || input[i] == '.' ||
             input[i] == 'e' || input[i] == 'E')) ++i;
      if (start == i || i - start > 32) return false;
      field.value.assign(input, start, i - start);
    }
    fields.emplace(std::move(key), std::move(field));
    if (fields.size() > 8) return false;
    spaces();
    if (i >= input.size()) return false;
    if (input[i] == '}') { ++i; spaces(); return i == input.size(); }
    if (input[i++] != ',') return false;
  }
}

inline bool parse_action_float(const JsonField& field, float& output, float limit) {
  if (field.kind != JsonField::Kind::number) return false;
  const char* first = field.value.data();
  const char* last = first + field.value.size();
  const auto result = std::from_chars(first, last, output);
  return result.ec == std::errc{} && result.ptr == last && std::isfinite(output) &&
      std::abs(output) <= limit;
}

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

inline bool parse_native_action(const std::string& body, Action& action) {
  std::map<std::string, JsonField> fields;
  if (!parse_action_object(body, fields)) return false;
  if (action.kind == Action::Kind::give_item) {
    if (fields.size() != 4 || !fields.count("code") || !fields.count("amount") ||
        !fields.count("quality") || !fields.count("blueprint")) return false;
    const auto& code = fields.at("code");
    const auto& amount = fields.at("amount");
    const auto& quality = fields.at("quality");
    const auto& blueprint = fields.at("blueprint");
    if (code.kind != JsonField::Kind::string || !valid_item_code(code.value) ||
        amount.kind != JsonField::Kind::number || blueprint.kind != JsonField::Kind::boolean) return false;
    int value = 0;
    const auto parsed = std::from_chars(amount.value.data(), amount.value.data()+amount.value.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != amount.value.data()+amount.value.size() ||
        value < 1 || value > 10000 || !parse_action_float(quality, action.quality, 100000.0f) ||
        action.quality < 0) return false;
    action.item_code = code.value;
    action.amount = value;
    action.blueprint = blueprint.value == "true";
    return true;
  }
  if (action.kind == Action::Kind::teleport) {
    if (fields.size() != 3 || !fields.count("x") || !fields.count("y") || !fields.count("z")) return false;
    return parse_action_float(fields.at("x"), action.x, 100000000.0f) &&
        parse_action_float(fields.at("y"), action.y, 100000000.0f) &&
        parse_action_float(fields.at("z"), action.z, 100000000.0f);
  }
  return false;
}

struct Player {
  std::string id;
  std::string name;
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
  Server(std::string token, std::string boot_id) : token_(std::move(token)), boot_id_(std::move(boot_id)) {}
  ~Server() { stop(); }

  bool start(int port = 18891) {
    if (token_.size() < 24 || token_.size() > 256) return false;
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(fd_, 16)) {
      close(fd_); fd_ = -1; return false;
    }
    active_ = true;
    worker_ = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    if (!active_.exchange(false)) return;
    shutdown(fd_, SHUT_RDWR);
    if (worker_.joinable()) worker_.join();
    close(fd_);
    fd_ = -1;
  }

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
    players_[id] = Player{id, event.name};
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
                            const std::string& entity_name) {
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

  void record_login(const std::string& id, const std::string& name) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    departed_positions_.erase(id);
    death_positions_.erase(id);
    death_event_seq_.erase(id);
    const std::string display = name.empty() ? id : name;
    if (players_.find(id) != players_.end()) return;
    players_[id] = Player{id, display};
    Event event{};
    event.id = id;
    event.name = display;
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

 private:
  std::string token_;
  std::string boot_id_;
  int fd_ = -1;
  std::thread worker_;
  std::atomic<bool> active_{false};
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

  static bool reply(int c, int status, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                         status == 401 ? "Unauthorized" : status == 404 ? "Not Found" :
                         status == 503 ? "Service Unavailable" : "Not Implemented";
    const std::string headers = "HTTP/1.1 " + std::to_string(status) + " " + reason +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n";
    const std::string response = headers + body;
    size_t sent = 0;
    while (sent < response.size()) {
      const ssize_t n = send(c, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    return sent == response.size();
  }

  bool authorized(const std::string& headers) const {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string prefix = "authorization: bearer ";
    size_t pos = lower.find(prefix);
    if (pos == std::string::npos || (pos && headers[pos - 1] != '\n')) return false;
    if (lower.find(prefix, pos + 1) != std::string::npos) return false;
    pos += prefix.size();
    const size_t end = headers.find("\r\n", pos);
    if (end == std::string::npos || end - pos != token_.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < token_.size(); ++i) diff |= static_cast<unsigned char>(headers[pos + i] ^ token_[i]);
    return diff == 0;
  }

  void run() {
    while (active_.load(std::memory_order_relaxed)) {
      const int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) continue;
      timeval timeout{3, 0};
      setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      serve(c);
      close(c);
    }
  }

  void serve(int c) {
    std::string request;
    char buf[4096];
    size_t header_end = std::string::npos;
    while (request.size() <= 16384 && (header_end = request.find("\r\n\r\n")) == std::string::npos) {
      const ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      request.append(buf, static_cast<size_t>(n));
    }
    if (header_end == std::string::npos || header_end > 16384) { reply(c, 400, "{\"error\":\"invalid headers\"}"); return; }
    const std::string headers = request.substr(0, header_end + 2);
    if (!authorized(headers)) { reply(c, 401, "{\"error\":\"unauthorized\"}"); return; }
    const size_t line_end = headers.find("\r\n");
    const std::string line = headers.substr(0, line_end);
    const size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 <= s1) { reply(c, 400, "{\"error\":\"invalid request\"}"); return; }
    const std::string method = line.substr(0, s1);
    const std::string path = line.substr(s1 + 1, s2 - s1 - 1);
    size_t length = 0;
    std::string lower_headers = headers;
    std::transform(lower_headers.begin(), lower_headers.end(), lower_headers.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string length_header = "content-length: ";
    const size_t length_pos = lower_headers.find(length_header);
    if (lower_headers.find("transfer-encoding:") != std::string::npos ||
        (length_pos != std::string::npos &&
         (length_pos == 0 || lower_headers[length_pos - 1] != '\n' ||
          lower_headers.find(length_header, length_pos + 1) != std::string::npos))) {
      reply(c, 400, "{\"error\":\"ambiguous framing\"}"); return;
    }
    if (length_pos != std::string::npos) {
      const size_t begin = length_pos + length_header.size();
      const size_t end = headers.find("\r\n", begin);
      const std::string digits = headers.substr(begin, end - begin);
      if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) {
        reply(c, 400, "{\"error\":\"invalid length\"}"); return;
      }
      length = static_cast<size_t>(std::stoul(digits));
    }
    if (length > 4096) { reply(c, 400, "{\"error\":\"message too large\"}"); return; }
    std::string body = request.substr(header_end + 4);
    while (body.size() < length) {
      const ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      body.append(buf, static_cast<size_t>(n));
    }
    body.resize(length);
    if (method == "GET" && path == "/health") {
      const bool ready = ready_.load(std::memory_order_acquire);
      const bool chat = ready && chat_ready_.load(std::memory_order_acquire);
      bool items = false, entities = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        items = catalog_ready_;
        entities = entities_ready_;
      }
      reply(c, 200, "{\"status\":\"" + std::string(ready_ ? "ok" : "starting") +
          "\",\"bootId\":" + escape(boot_id_) + ",\"build\":\"21241282\",\"capabilities\":{\"chat\":\"" +
          (chat ? "ok" : "starting") + "\",\"sendMessage\":\"" + (chat ? "ok" : "starting") +
          "\",\"roster\":\"" + (ready ? "ok" : "starting") +
          "\",\"items\":\"" + (items ? "ok" : "unavailable") +
          "\",\"entities\":\"" + (entities ? "ok" : "unavailable") + "\"}}");
      return;
    }
    if (method == "GET" && path == "/players") {
      std::string json = "[";
      std::vector<Player> snapshot;
      { std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, p] : players_) snapshot.push_back(p);
      }
      for (const auto& p : snapshot) {
        if (json.size() > 1) json += ',';
        json += player_json(p);
      }
      json += ']'; reply(c, 200, json); return;
    }
    if (method == "GET" && path == "/bans") {
      std::vector<std::string> snapshot;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (!bans_ready_ || std::chrono::steady_clock::now() - bans_sampled_ >
            std::chrono::seconds(3)) {
          reply(c, 503, "{\"error\":\"native bans unavailable\"}"); return;
        }
        snapshot = banned_ids_;
      }
      std::string json = "[";
      for (const auto& id : snapshot) {
        if (json.size() > 1) json += ',';
        json += escape(id);
        if (json.size() > 256000) {
          reply(c, 503, "{\"error\":\"native bans payload too large\"}"); return;
        }
      }
      json += ']'; reply(c, 200, json); return;
    }
    if (method == "GET" && path == "/locations") {
      std::vector<LocationItem> snapshot;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (!locations_ready_) { reply(c, 503, "{\"error\":\"native locations unavailable\"}"); return; }
        snapshot = locations_;
      }
      std::string json = "[";
      for (const auto& item : snapshot) {
        if (json.back() != '[') json += ',';
        char coords[160];
        const int n = std::snprintf(coords, sizeof(coords),
            "{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}",
            static_cast<double>(item.x), static_cast<double>(item.y),
            static_cast<double>(item.z));
        if (n <= 0 || n >= static_cast<int>(sizeof(coords))) {
          reply(c, 503, "{\"error\":\"native locations encoding failed\"}"); return;
        }
        char size[160];
        const int size_n = std::snprintf(size, sizeof(size),
            ",\"sizeX\":%.6f,\"sizeY\":%.6f,\"sizeZ\":%.6f}",
            static_cast<double>(item.size_x), static_cast<double>(item.size_y),
            static_cast<double>(item.size_z));
        if (size_n <= 0 || size_n >= static_cast<int>(sizeof(size))) {
          reply(c, 503, "{\"error\":\"native locations size encoding failed\"}"); return;
        }
        json += "{\"code\":" + escape(item.code) + ",\"name\":" + escape(item.name) +
                ",\"position\":" + coords + size;
        if (json.size() > 256000) {
          reply(c, 503, "{\"error\":\"native locations payload too large\"}"); return;
        }
      }
      json += ']';
      reply(c, 200, json); return;
    }
    if (method == "GET" && path.rfind("/players/", 0) == 0 && path.ends_with("/location")) {
      const std::string id = path.substr(9, path.size() - 9 - 9);
      Position point{};
      bool found = false;
      bool departed = false;
      bool died = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        auto it = positions_.find(id);
        if (it != positions_.end() &&
            std::chrono::steady_clock::now() - it->second.sampled < std::chrono::seconds(2)) {
          point = it->second;
          found = true;
        } else if (!players_.contains(id)) {
          // Only the latest retained disconnect for this identity may enrich
          // an offline event. An older disconnect cannot stand in for a newer
          // event that lacked an actual position.
          for (auto event = events_.rbegin(); event != events_.rend(); ++event) {
            if (event->id != id || event->type != "player-disconnected") continue;
            if (event->position) {
              point = *event->position;
              found = true;
              departed = !event->position_from_death;
              died = event->position_from_death;
            }
            break;
          }
        } else {
          // A new login supersedes prior deaths. A fresh live position above
          // always wins after respawn; retained death position is event-time
          // provenance while its event is still in the replay ring.
          for (auto event = events_.rbegin(); event != events_.rend(); ++event) {
            if (event->id != id) continue;
            if (event->type == "player-connected") break;
            if (event->type == "player-death") {
              auto active = death_event_seq_.find(id);
              if (active != death_event_seq_.end() && active->second == event->seq && event->position)
                { point = *event->position; found = died = true; }
              break;
            }
          }
        }
      }
      if (!found) { reply(c, 503, "{\"error\":\"live position unavailable\"}"); return; }
      char body[192];
      const int size = std::snprintf(body, sizeof(body),
                                     departed ? "{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"source\":\"departure\"}"
                                              : died ? "{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f,\"source\":\"death\"}"
                                                     : "{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f}",
                                     static_cast<double>(point.x), static_cast<double>(point.y),
                                     static_cast<double>(point.z));
      if (size <= 0 || size >= static_cast<int>(sizeof(body))) {
        reply(c, 503, "{\"error\":\"position encoding failed\"}"); return;
      }
      reply(c, 200, std::string(body, static_cast<size_t>(size))); return;
    }
    if (method == "GET" && path.rfind("/players/", 0) == 0 && path.ends_with("/inventory")) {
      const std::string id = path.substr(9, path.size() - 9 - 10);
      InventorySnapshot snapshot{};
      bool found = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        auto it = inventories_.find(id);
        if (it != inventories_.end() &&
            std::chrono::steady_clock::now() - it->second.sampled < std::chrono::seconds(3)) {
          snapshot = it->second;
          found = true;
        }
      }
      if (!found) { reply(c, 503, "{\"error\":\"live inventory unavailable\"}"); return; }
      std::string json = "[";
      for (const auto& item : snapshot.items) {
        if (json.size() > 1) json += ',';
        json += "{\"code\":" + escape(item.code) + ",\"name\":" + escape(item.name) +
                ",\"amount\":" + std::to_string(item.amount) + "}";
        if (json.size() > 256000) {
          reply(c, 503, "{\"error\":\"inventory exceeds transport bound\"}"); return;
        }
      }
      json += ']';
      reply(c, 200, json); return;
    }
    const bool entity_page = path.rfind("/entities?offset=", 0) == 0;
    if (method == "GET" && (path.rfind("/items?offset=", 0) == 0 || entity_page)) {
      const size_t prefix_length = entity_page ? sizeof("/entities?offset=") - 1 : sizeof("/items?offset=") - 1;
      const size_t limit_mark = path.find("&limit=", prefix_length);
      if (limit_mark == std::string::npos) {
        reply(c, 400, "{\"error\":\"invalid catalog page\"}"); return;
      }
      const std::string offset_digits = path.substr(prefix_length, limit_mark - prefix_length);
      const std::string limit_digits = path.substr(limit_mark + sizeof("&limit=") - 1);
      int offset = -1, limit = -1;
      const auto offset_result = std::from_chars(offset_digits.data(), offset_digits.data() + offset_digits.size(), offset);
      const auto limit_result = std::from_chars(limit_digits.data(), limit_digits.data() + limit_digits.size(), limit);
      if (offset_digits.empty() || limit_digits.empty() ||
          offset_result.ec != std::errc{} || offset_result.ptr != offset_digits.data() + offset_digits.size() ||
          limit_result.ec != std::errc{} || limit_result.ptr != limit_digits.data() + limit_digits.size() ||
          offset < 0 || limit < 1 || limit > 128) {
        reply(c, 400, "{\"error\":\"invalid catalog page\"}"); return;
      }
      std::vector<CatalogItem> page;
      size_t total = 0, next = 0;
      bool available = false, in_range = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        available = entity_page ? entities_ready_ : catalog_ready_;
        if (available) {
          const auto& catalog = entity_page ? entity_items_ : catalog_items_;
          total = catalog.size();
          in_range = static_cast<size_t>(offset) <= total;
          if (in_range) {
            next = std::min(total, static_cast<size_t>(offset) + static_cast<size_t>(limit));
            page.assign(catalog.begin() + offset, catalog.begin() + next);
          }
        }
      }
      if (!available) { reply(c, 503, entity_page
          ? "{\"error\":\"native entity catalog unavailable\"}"
          : "{\"error\":\"native item catalog unavailable\"}"); return; }
      if (!in_range) { reply(c, 400, "{\"error\":\"catalog offset out of range\"}"); return; }
      std::string json = "{\"offset\":" + std::to_string(offset) +
          ",\"nextOffset\":" + std::to_string(next) + ",\"total\":" +
          std::to_string(total) + ",\"complete\":" + (next == total ? "true" : "false") + ",\"items\":[";
      for (const auto& item : page) {
        if (json.back() != '[') json += ',';
        json += "{\"code\":" + escape(item.code) + ",\"name\":" + escape(item.name) + "}";
      }
      json += "]}";
      if (json.size() > 256000) { reply(c, 503, "{\"error\":\"catalog page exceeds transport bound\"}"); return; }
      reply(c, 200, json); return;
    }
    if (method == "GET" && path.rfind("/players/", 0) == 0) {
      const std::string id = path.substr(9);
      std::string json = "null";
      Player snapshot;
      bool found = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        auto it = players_.find(id);
        if (it != players_.end()) { snapshot = it->second; found = true; }
      }
      if (found) json = player_json(snapshot);
      reply(c, 200, json); return;
    }
    if (method == "GET" && path.rfind("/events?since=", 0) == 0) {
      const std::string digits = path.substr(14);
      if (digits.empty() || digits.size() > 20 || digits.find_first_not_of("0123456789") != std::string::npos) {
        reply(c, 400, "{\"error\":\"invalid cursor\"}"); return;
      }
      uint64_t since = 0;
      const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), since);
      if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
        reply(c, 400, "{\"error\":\"invalid cursor\"}"); return;
      }
      std::string json = "{\"bootId\":" + escape(boot_id_) + ",\"events\":[";
      bool truncated = false;
      uint64_t sequence = 0;
      uint64_t last_emitted = since;
      std::vector<Event> snapshot;
      { std::lock_guard<std::mutex> lock(mutex_);
        sequence = sequence_;
        truncated = !events_.empty() && since < events_.front().seq - 1;
        for (const auto& e : events_) {
          if (e.seq > since) snapshot.push_back(e);
          if (snapshot.size() >= 128) break;
        }
      }
      for (const auto& e : snapshot) {
          std::string item = "{\"seq\":" + std::to_string(e.seq) + ",\"type\":" + escape(e.type) +
                  ",\"ts\":" + escape(e.timestamp) + ",\"data\":{";
          if (e.type == "log") item += "\"msg\":" + escape(e.message);
          else item += "\"player\":" + player_json(Player{e.id, e.name});
          if (e.type == "chat-message") item += ",\"msg\":" + escape(e.message) + ",\"channel\":\"global\"";
          if (e.type == "player-death" && !e.attacker_id.empty())
            item += ",\"killer\":" + player_json(Player{e.attacker_id, e.attacker_id});
          if (e.type == "entity-killed")
            item += ",\"entity\":{\"code\":" + escape(e.entity_code) +
                    ",\"name\":" + escape(e.entity_name) + "}";
          item += "}}";
          if (json.size() + item.size() > 256000 && last_emitted != since) break;
          if (json.back() != '[') json += ',';
          json += item;
          last_emitted = e.seq;
      }
      if (last_emitted == since && since > sequence) last_emitted = sequence;
      json += "],\"seq\":" + std::to_string(last_emitted) +
              ",\"latestSeq\":" + std::to_string(sequence) +
              ",\"hasMore\":" + (last_emitted < sequence ? "true" : "false") +
              ",\"truncated\":" + (truncated ? "true" : "false") + "}";
      reply(c, 200, json); return;
    }
    if (method == "POST" && path.rfind("/players/", 0) == 0 &&
        (path.ends_with("/give-item") || path.ends_with("/teleport"))) {
      if (!ready_) { reply(c, 503, "{\"error\":\"game thread not ready\"}"); return; }
      const bool give_item = path.ends_with("/give-item");
      const size_t suffix = give_item ? sizeof("/give-item") - 1 : sizeof("/teleport") - 1;
      const std::string id = path.substr(sizeof("/players/") - 1,
          path.size() - (sizeof("/players/") - 1) - suffix);
      if (id.size() != 17 || id.find_first_not_of("0123456789") != std::string::npos) {
        reply(c, 400, "{\"error\":\"invalid player id\"}"); return;
      }
      auto action = std::make_shared<Action>();
      action->kind = give_item ? Action::Kind::give_item : Action::Kind::teleport;
      action->player_id = id;
      if (!parse_native_action(body, *action)) {
        reply(c, 400, "{\"error\":\"invalid native action\"}"); return;
      }
      bool queued = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (actions_.size() < 128 && players_.count(id)) { actions_.push_back(action); queued = true; }
      }
      if (!queued) { reply(c, 503, "{\"error\":\"player unavailable or queue full\"}"); return; }
      std::unique_lock<std::mutex> lock(action->mutex);
      if (!action->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return action->done; })) {
        if (!action->executing) {
          action->cancelled = true;
          reply(c, 503, "{\"error\":\"game thread timeout\"}"); return;
        }
        action->done_cv.wait(lock, [&] { return action->done; });
      }
      reply(c, action->success ? 200 : 503,
          action->success ? "{\"success\":true}" : "{\"error\":\"native action failed\"}");
      return;
    }
    if (method == "POST" && path == "/console") {
      if (!ready_) { reply(c, 503, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"game thread not ready\"}"); return; }
      std::wstring decoded_command;
      if (!ark_engine_exec::decode_command(body, decoded_command)) {
        reply(c, 400, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"invalid command\"}"); return;
      }
      if (ark_shutdown_request::is_termination_command(body)) {
        reply(c, 400, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"Use the dedicated shutdown action\"}"); return;
      }
      if (ark_shutdown_request::contains_unicode_space(body)) {
        reply(c, 400, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"Unsupported command whitespace\"}"); return;
      }
      auto action = std::make_shared<Action>();
      action->kind = Action::Kind::console;
      action->command = body;
      bool queued = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (actions_.size() < 128) { actions_.push_back(action); queued = true; }
      }
      if (!queued) { reply(c, 503, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"queue full\"}"); return; }
      std::unique_lock<std::mutex> lock(action->mutex);
      if (!action->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return action->done; })) {
        if (!action->executing) {
          action->cancelled = true;
          reply(c, 503, "{\"success\":false,\"rawResult\":\"\",\"errorMessage\":\"game thread timeout\"}"); return;
        }
        action->done_cv.wait(lock, [&] { return action->done; });
      }
      reply(c, action->success ? 200 : 503,
          "{\"success\":" + std::string(action->success ? "true" : "false") +
          ",\"rawResult\":" + escape(action->output) +
          ",\"errorMessage\":" + (action->success ? "null" : "\"native console rejected or unhandled\"") + "}");
      return;
    }
    if (method == "POST" && path.rfind("/players/", 0) == 0 &&
        (path.ends_with("/kick") || path.ends_with("/ban") || path.ends_with("/unban"))) {
      if (!ready_) { reply(c, 503, "{\"success\":false,\"errorMessage\":\"game thread not ready\"}"); return; }
      const bool kick = path.ends_with("/kick"), unban = path.ends_with("/unban");
      const size_t suffix = kick ? 5 : (unban ? 6 : 4);
      const std::string id = path.substr(sizeof("/players/") - 1,
          path.size() - (sizeof("/players/") - 1) - suffix);
      if (id.size() != 17 || id.find_first_not_of("0123456789") != std::string::npos || !body.empty()) {
        reply(c, 400, "{\"success\":false,\"errorMessage\":\"invalid moderation request\"}"); return;
      }
      auto action = std::make_shared<Action>();
      action->kind = kick ? Action::Kind::kick : (unban ? Action::Kind::unban : Action::Kind::ban);
      action->player_id = id;
      bool queued = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (actions_.size() < 128 && (!kick || players_.count(id))) { actions_.push_back(action); queued = true; }
      }
      if (!queued) { reply(c, 503, "{\"success\":false,\"errorMessage\":\"player unavailable or queue full\"}"); return; }
      std::unique_lock<std::mutex> lock(action->mutex);
      if (!action->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return action->done; })) {
        if (!action->executing) {
          action->cancelled = true;
          reply(c, 503, "{\"success\":false,\"errorMessage\":\"game thread timeout\"}"); return;
        }
        action->done_cv.wait(lock, [&] { return action->done; });
      }
      reply(c, action->success ? 200 : 503,
          action->success ? "{\"success\":true}" : "{\"success\":false,\"errorMessage\":\"native moderation effect unverified\"}");
      return;
    }
    if (method == "POST" && path == "/shutdown") {
      if (!ready_) { reply(c, 503, "{\"success\":false,\"errorMessage\":\"game thread not ready\"}"); return; }
      if (!body.empty()) { reply(c, 400, "{\"success\":false,\"errorMessage\":\"unexpected body\"}"); return; }
      auto action = std::make_shared<Action>();
      action->kind = Action::Kind::shutdown;
      bool queued = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (actions_.size() < 128) { actions_.push_back(action); queued = true; }
      }
      if (!queued) { reply(c, 503, "{\"success\":false,\"errorMessage\":\"queue full\"}"); return; }
      std::unique_lock<std::mutex> lock(action->mutex);
      if (!action->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return action->done; })) {
        if (!action->executing) {
          action->cancelled = true;
          reply(c, 503, "{\"success\":false,\"errorMessage\":\"game thread timeout\"}"); return;
        }
        action->done_cv.wait(lock, [&] { return action->done; });
      }
      const bool flushed = reply(c, action->success ? 200 : 503,
          action->success ? "{\"success\":true}" : "{\"success\":false,\"errorMessage\":\"shutdown unavailable\"}");
      action->response_sent.store(flushed && action->success, std::memory_order_release);
      return;
    }
    const bool targeted_message = path.rfind("/players/", 0) == 0 && path.ends_with("/message");
    if (method == "POST" && (path == "/message" || targeted_message)) {
      if (!ready_) { reply(c, 503, "{\"error\":\"game thread not ready\"}"); return; }
      auto action = std::make_shared<Action>();
      if (targeted_message) {
        action->player_id = path.substr(sizeof("/players/") - 1,
            path.size() - (sizeof("/players/") - 1) - sizeof("/message") + 1);
        if (action->player_id.size() != 17 ||
            action->player_id.find_first_not_of("0123456789") != std::string::npos) {
          reply(c, 400, "{\"error\":\"invalid recipient\"}"); return;
        }
      }
      if (!utf8_to_utf32(body, action->text)) { reply(c, 400, "{\"error\":\"invalid message\"}"); return; }
      bool queued = false;
      { std::lock_guard<std::mutex> lock(mutex_);
        if (actions_.size() < 128 &&
            (!targeted_message || players_.count(action->player_id) != 0)) {
          actions_.push_back(action); queued = true;
        }
      }
      if (!queued) { reply(c, 503, "{\"error\":\"no native recipient or queue full\"}"); return; }
      std::unique_lock<std::mutex> lock(action->mutex);
      if (!action->done_cv.wait_for(lock, std::chrono::seconds(3), [&] { return action->done; })) {
        if (!action->executing) {
          action->cancelled = true;
          reply(c, 503, "{\"error\":\"game thread timeout\"}"); return;
        }
        action->done_cv.wait(lock, [&] { return action->done; });
      }
      reply(c, action->success ? 200 : 503, action->success ? "{\"success\":true}" : "{\"error\":\"native dispatch failed\"}");
      return;
    }
    reply(c, 501, "{\"error\":\"native route unavailable\"}");
  }

  static std::string player_json(const Player& p) {
    return "{\"gameId\":" + escape(p.id) + ",\"name\":" + escape(p.name) +
        ",\"steamId\":" + escape(p.id) + ",\"platformId\":" + escape("steam:" + p.id) + "}";
  }
};
} // namespace gate
