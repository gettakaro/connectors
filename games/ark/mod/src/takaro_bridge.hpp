#pragma once
// The Takaro Generic connection inside the game process. Threads:
//   takaro-io      owns the WebSocket (connect, TLS, identify, heartbeat,
//                  reconnect) and the event pump (reads the native event ring,
//                  advances the cursor only on pong-confirmed delivery);
//   takaro-req x4  run Generic requests against the gate (may wait for the
//                  game thread, so never on the I/O thread);
//   takaro-maint   ban reconciliation, timed-ban expiry, state persistence.
// None of them is the game thread; the game thread only touches gate::Server.
#include "gate_state.hpp"
#include "json.hpp"
#include "takaro_adapter.hpp"
#include "takaro_store.hpp"
#include "ws_client.hpp"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <algorithm>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace takaro {

struct Config {
  std::string url = "wss://connect.takaro.io/";
  std::string registration_token;
  std::string identity_token;
  std::string server_name;
  std::string state_dir;
  std::string ban_file;   // default <state_dir>/ban-metadata.json
  std::string ca_file;
  bool wire_debug = false;
  std::chrono::milliseconds ping_interval{5000};
  std::chrono::milliseconds idle_timeout{20000};
  unsigned max_missed_pongs = 2;
  std::chrono::milliseconds reconnect_base{2000};
  std::chrono::milliseconds reconnect_max{60000};
  std::chrono::milliseconds pump_interval{250};
  std::chrono::milliseconds reconcile_interval{1000};
  size_t workers = 4;
  // Generic protocol versions offered in identify; Takaro picks the highest shared one.
  // A Takaro that predates versioning ignores the field and speaks version 0.
  std::vector<int> protocol_versions{0, 1};
  // The loaded map (from the server command line). Only maps with a verified
  // world<->lat/long calibration get a declared map Dimension.
  std::string map_name;
};

// TheIsland: lat = 50 + y/8000, lon = 50 + x/8000 (verified against the client GPS),
// so lat/long 0..100 (the in-game map's frame) is x, y in [-400000, 400000],
// x to the right and y downwards. The image comes from an operator upload.
inline std::optional<json::Value> map_dimension(const std::string& map_name) {
  if (map_name != "TheIsland") return std::nullopt;
  auto range = [](double min, double max) { return json::obj({{"min", json::num(min)}, {"max", json::num(max)}}); };
  return json::obj({
      {"id", json::str("TheIsland")},
      {"name", json::str("The Island")},
      {"bounds", json::obj({{"horizontal", range(-400000, 400000)}, {"vertical", range(-400000, 400000)}})},
      {"axes", json::obj({{"horizontal", json::obj({{"field", json::str("x")}, {"direction", json::str("right")}})},
                          {"vertical", json::obj({{"field", json::str("y")}, {"direction", json::str("down")}})}})},
      {"source", json::obj({{"type", json::str("none")}})}});
}

// What this connector really does, declared on protocol version 1. The map capability is
// added separately for maps with a verified calibration (no tiles: operator upload).
// Capabilities not listed here are refused by Takaro before sending.
inline const std::vector<std::string>& capability_manifest() {
  static const std::vector<std::string> names = {
      "players.list", "players.get", "players.location", "players.inventory", "players.teleport",
      "items.list", "items.give", "entities.list", "locations.list", "chat.send", "console.execute",
      "moderation.kick", "moderation.ban", "moderation.unban", "moderation.listBans", "server.shutdown",
      "events.player-connected", "events.player-disconnected", "events.chat-message",
      "events.player-death", "events.entity-killed", "events.log"};
  return names;
}

using LogSink = std::function<void(const std::string&)>;

// Port of the sidecar's mapEvent. nullopt means the event cannot be sent
// safely and is skipped (logged), so one bad record never blocks the ring.
inline std::optional<json::Value> map_event(const gate::Event& e, KnownPlayers& known) {
  auto name_of = [&](const std::string& id, const std::string& name) {
    return !name.empty() && name != id ? name : known.name(id);
  };
  json::Value data = json::Value::object();
  if (e.type == "log") {
    const std::string& msg = e.message;
    if (js_trim(msg).empty() || msg.size() > 512) return std::nullopt;
    for (unsigned char c : msg) if (c < 0x20 || c == 0x7f) return std::nullopt;
    std::string lower = msg;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* marker : {"commandline", "password", "token", "authorization", "auth=", "rcon", "steam_"})
      if (lower.find(marker) != std::string::npos) return std::nullopt;
    // Dotted quads (IPv4 addresses).
    for (size_t i = 0; i < msg.size(); ++i) {
      size_t j = i;
      int groups = 0;
      while (groups < 4) {
        size_t d = 0;
        while (j < msg.size() && std::isdigit(static_cast<unsigned char>(msg[j])) && d < 3) { ++j; ++d; }
        if (!d) break;
        ++groups;
        if (groups == 4) break;
        if (j >= msg.size() || msg[j] != '.') break;
        ++j;
      }
      if (groups == 4) return std::nullopt;
    }
    data.set("msg", json::str(msg));
    if (!e.timestamp.empty()) data.set("timestamp", json::str(e.timestamp));
    return data;
  }
  if (e.type != "chat-message" && e.type != "player-connected" && e.type != "player-disconnected" &&
      e.type != "player-death" && e.type != "entity-killed")
    return std::nullopt;
  if (!valid_steam64(e.id)) return std::nullopt;
  data.set("player", e.type == "player-connected"
                        ? player_json(e.id, name_of(e.id, e.name), e.ip, e.ping)
                        : player_json(e.id, name_of(e.id, e.name)));
  if (e.type == "chat-message") {
    data.set("msg", json::str(e.message));
    data.set("channel", json::str("global"));
  }
  if (e.type == "player-death" && valid_steam64(e.attacker_id) && e.attacker_id != e.id)
    data.set("attacker", player_json(e.attacker_id, known.name(e.attacker_id)));
  if (e.type == "player-death" && e.position && std::isfinite(e.position->x) && std::isfinite(e.position->y) &&
      std::isfinite(e.position->z))
    data.set("position", json::obj({{"x", json::num(e.position->x)}, {"y", json::num(e.position->y)},
                                    {"z", json::num(e.position->z)}}));
  if (e.type == "entity-killed") {
    if (js_trim(e.entity_code).empty() || js_trim(e.entity_name).empty()) return std::nullopt;
    data.set("entity", json::str(js_trim(e.entity_code)));
    // Takaro EventEntityKilled requires a string: the held weapon's display
    // name, or empty for an unarmed killer or an unverified weapon (never invented).
    data.set("weapon", json::str(js_trim(e.weapon)));
  }
  if (!e.timestamp.empty()) data.set("timestamp", json::str(e.timestamp));
  return data;
}

inline std::string redact_frame(const std::string& frame) {
  std::string out = frame;
  for (const char* key : {"\"registrationToken\":\"", "\"identityToken\":\"", "\"token\":\"", "\"password\":\""}) {
    size_t at = 0;
    while ((at = out.find(key, at)) != std::string::npos) {
      const size_t start = at + std::strlen(key);
      const size_t end = out.find('"', start);
      if (end == std::string::npos) break;
      out.replace(start, end - start, "<redacted>");
      at = start;
    }
  }
  if (out.size() > 2000) out = out.substr(0, 2000) + "...";
  return out;
}

class Bridge {
 public:
  Bridge(Config config, gate::Server& gate, LogSink log)
      : config_(std::move(config)), gate_(gate), log_(std::move(log)),
        known_(config_.state_dir.empty() ? std::string() : config_.state_dir + "/known-players.json"),
        bans_(gate_, BanFile(config_.ban_file.empty() && !config_.state_dir.empty()
                                 ? config_.state_dir + "/ban-metadata.json" : config_.ban_file),
              known_),
        adapter_(gate_, bans_, known_) {
    if (!config_.state_dir.empty()) cursor_path_ = config_.state_dir + "/event-cursor.json";
    if (auto dimension = map_dimension(config_.map_name)) adapter_.set_map_dimension(dimension->get("id")->as_string());
    adapter_.set_delivery_waiter([this](uint64_t seq, std::chrono::milliseconds timeout) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (persisted_.load() < seq && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      return persisted_.load() >= seq;
    });
    const Cursor saved = cursor_path_.empty() ? Cursor{} : load_cursor(cursor_path_);
    if (saved.boot_id == gate_.boot_id()) { scan_ = persisted_ = saved.seq; }
  }

  ~Bridge() { stop(); }

  bool start() {
    if (running_.exchange(true)) return false;
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) { running_ = false; return false; }
    io_ = std::thread([this] { io_loop(); });
    for (size_t i = 0; i < config_.workers; ++i) workers_.emplace_back([this] { worker_loop(); });
    maint_ = std::thread([this] { maintenance_loop(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    wake();
    { std::lock_guard<std::mutex> lock(requests_mutex_); }
    requests_cv_.notify_all();
    { std::lock_guard<std::mutex> lock(maint_mutex_); }
    maint_cv_.notify_all();
    if (io_.joinable()) io_.join();
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
    if (maint_.joinable()) maint_.join();
    if (wake_fd_ >= 0) { close(wake_fd_); wake_fd_ = -1; }
  }

  // Observable state (status file, tests).
  bool identified() const { return identified_.load(); }
  uint64_t persisted_cursor() const { return persisted_.load(); }
  std::string game_server_id() const { std::lock_guard<std::mutex> l(status_mutex_); return game_server_id_; }
  uint64_t connections() const { return connections_.load(); }

 private:
  struct Request {
    uint64_t epoch;
    std::string request_id;
    std::string action;
    json::Value args;
  };
  struct Outbound {
    uint64_t epoch;
    std::string text;
    std::function<void()> on_written;
  };
  struct InFlight { uint64_t send_id; uint64_t seq; };
  struct Ping { uint64_t id; uint64_t up_to; };

  Config config_;
  gate::Server& gate_;
  LogSink log_;
  KnownPlayers known_;
  BanManager bans_;
  Adapter adapter_;
  std::string cursor_path_;

  std::atomic<bool> running_{false};
  std::thread io_;
  std::vector<std::thread> workers_;
  std::thread maint_;
  int wake_fd_ = -1;

  // I/O thread state.
  ws::Connection conn_;
  std::atomic<uint64_t> epoch_{0};
  std::atomic<bool> identified_{false};
  std::atomic<int> protocol_version_{0};
  std::atomic<uint64_t> connections_{0};
  unsigned attempts_ = 0;
  uint64_t send_id_ = 0;
  uint64_t confirmed_id_ = 0;
  uint64_t ping_id_ = 0;
  std::deque<Ping> pings_;
  std::deque<InFlight> in_flight_;
  std::atomic<uint64_t> scan_{0};
  std::atomic<uint64_t> persisted_{0};
  std::chrono::steady_clock::time_point last_activity_{};
  std::chrono::steady_clock::time_point next_ping_{};
  std::chrono::steady_clock::time_point last_eager_ping_{};
  std::chrono::steady_clock::time_point next_pump_{};
  bool truncation_logged_ = false;

  std::mutex out_mutex_;
  std::deque<Outbound> outbound_;

  std::mutex requests_mutex_;
  std::condition_variable requests_cv_;
  std::deque<Request> requests_;

  std::mutex maint_mutex_;
  std::condition_variable maint_cv_;

  mutable std::mutex status_mutex_;
  std::string game_server_id_;
  std::string last_error_;

  void log(const std::string& line) { if (log_) log_(line); }

  void wake() {
    if (wake_fd_ < 0) return;
    const uint64_t one = 1;
    (void)!write(wake_fd_, &one, sizeof(one));
  }

  // Any thread: queue a frame for the current connection. Dropped when the
  // connection it belongs to is gone (a late result must not cross a reconnect).
  bool post(uint64_t epoch, std::string text, std::function<void()> on_written = {}) {
    if (!identified_.load() || epoch != epoch_.load()) return false;
    {
      std::lock_guard<std::mutex> lock(out_mutex_);
      if (outbound_.size() >= 1024) return false;
      outbound_.push_back({epoch, std::move(text), std::move(on_written)});
    }
    wake();
    return true;
  }

  std::string identify_frame() const {
    json::Value payload = json::Value::object();
    payload.set("identityToken", json::str(config_.identity_token));
    if (!config_.registration_token.empty()) payload.set("registrationToken", json::str(config_.registration_token));
    if (!config_.server_name.empty()) payload.set("name", json::str(config_.server_name));
    if (std::find(config_.protocol_versions.begin(), config_.protocol_versions.end(), 1) !=
        config_.protocol_versions.end()) {
      json::Value versions = json::Value::array();
      for (int v : config_.protocol_versions) versions.push(json::num(v));
      payload.set("protocolVersions", std::move(versions));
      json::Value manifest = json::Value::array();
      for (const auto& name : capability_manifest()) manifest.push(json::obj({{"name", json::str(name)}}));
      if (auto dimension = map_dimension(config_.map_name)) {
        json::Value dimensions = json::Value::array();
        dimensions.push(std::move(*dimension));
        manifest.push(json::obj({{"name", json::str("map")}, {"parameters", json::obj({{"dimensions", dimensions}})}}));
      }
      payload.set("capabilities", std::move(manifest));
    }
    return json::dump(json::obj({{"type", json::str("identify")}, {"payload", payload}}));
  }

  void set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    last_error_ = error;
  }

  void disconnect(const std::string& why) {
    if (conn_.is_open()) log("takaro-ws-closed " + why);
    conn_.close_now();
    identified_ = false;
    protocol_version_ = 0;
    adapter_.set_protocol_version(0);
    ++epoch_;
    pings_.clear();
    in_flight_.clear();
    scan_ = persisted_.load();  // unconfirmed events are replayed (at-least-once)
    {
      std::lock_guard<std::mutex> lock(out_mutex_);
      outbound_.clear();
    }
    set_error(why);
  }

  std::chrono::milliseconds backoff() const {
    auto delay = config_.reconnect_base * (1LL << std::min(attempts_, 10u));
    return std::min<std::chrono::milliseconds>(delay, config_.reconnect_max);
  }

  void io_loop() {
    // OpenSSL writes may raise SIGPIPE on a reset socket; keep it on this thread.
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);
    auto next_attempt = std::chrono::steady_clock::now();
    const auto url = ws::parse_url(config_.url);
    if (!url) { log("takaro-ws-invalid-url"); set_error("invalid TAKARO_WS_URL"); }
    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (!conn_.is_open()) {
        if (!url || now < next_attempt) { idle_wait(next_attempt); continue; }
        std::string error;
        ws::Connection::Options options;
        options.ca_file = config_.ca_file;
        log("takaro-ws-connecting " + url->host);
        if (!conn_.open(*url, options, error)) {
          const auto delay = backoff();
          ++attempts_;
          log("takaro-ws-connect-failed " + error + " retry-ms=" + std::to_string(delay.count()));
          set_error(error);
          next_attempt = std::chrono::steady_clock::now() + delay;
          continue;
        }
        ++connections_;
        ++epoch_;
        pings_.clear();
        in_flight_.clear();
        scan_ = persisted_.load();
        last_activity_ = std::chrono::steady_clock::now();
        next_ping_ = last_activity_ + config_.ping_interval;
        log("takaro-ws-open sending identify");
        conn_.send_text(identify_frame());
      }
      pollfd fds[2] = {{conn_.fd(), conn_.poll_events(), 0}, {wake_fd_, POLLIN, 0}};
      const int r = ::poll(fds, 2, static_cast<int>(config_.pump_interval.count()));
      if (r < 0 && errno != EINTR) { disconnect("poll failed"); continue; }
      if (fds[1].revents & POLLIN) { uint64_t v; (void)!read(wake_fd_, &v, sizeof(v)); }
      if (!step()) {
        const auto delay = backoff();
        ++attempts_;
        next_attempt = std::chrono::steady_clock::now() + delay;
        log("takaro-ws-reconnect-in-ms " + std::to_string(delay.count()));
      }
    }
    if (conn_.is_open()) {
      conn_.send_close(1001);
      std::string ignored;
      conn_.flush(ignored);
    }
    conn_.close_now();
    identified_ = false;
  }

  void idle_wait(std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    pollfd p{wake_fd_, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(std::clamp<long long>(ms, 1, 500))) > 0) {
      uint64_t v; (void)!read(wake_fd_, &v, sizeof(v));
    }
  }

  // One I/O iteration. False = the connection was dropped.
  bool step() {
    std::string error;
    {
      std::deque<Outbound> batch;
      { std::lock_guard<std::mutex> lock(out_mutex_); batch.swap(outbound_); }
      for (auto& out : batch) {
        if (out.epoch != epoch_.load()) continue;
        if (config_.wire_debug) log("takaro-wire SEND " + redact_frame(out.text));
        if (!conn_.send_text(out.text, std::move(out.on_written))) log("takaro-ws-send-buffer-full dropped");
      }
    }
    std::vector<ws::Frame> frames;
    const auto status = conn_.read(frames, error);
    for (auto& frame : frames) {
      last_activity_ = std::chrono::steady_clock::now();
      if (frame.kind == ws::Frame::Kind::text) handle_text(frame.payload);
      else if (frame.kind == ws::Frame::Kind::pong) note_pong(frame.payload);
    }
    if (status != ws::Connection::ReadStatus::ok) {
      disconnect(status == ws::Connection::ReadStatus::closed ? "closed by peer" : error);
      return false;
    }
    if (!conn_.is_open()) { disconnect("closed"); return false; }
    const auto now = std::chrono::steady_clock::now();
    if (identified_.load() && now >= next_pump_) {
      next_pump_ = now + config_.pump_interval;
      pump();
    }
    if (now >= next_ping_) {
      next_ping_ = now + config_.ping_interval;
      if (pings_.size() >= config_.max_missed_pongs) {
        log("takaro-ws-heartbeat-missed " + std::to_string(pings_.size()) + " pings unanswered");
        disconnect("heartbeat timeout");
        return false;
      }
      if (now - last_activity_ > config_.idle_timeout) {
        log("takaro-ws-idle-timeout");
        disconnect("idle timeout");
        return false;
      }
      send_ping();
    }
    if (!conn_.flush(error)) { disconnect(error); return false; }
    return true;
  }

  void send_ping() {
    ++ping_id_;
    pings_.push_back({ping_id_, send_id_});
    conn_.send_ping(std::to_string(ping_id_));
  }

  void note_pong(const std::string& payload) {
    for (size_t i = 0; i < pings_.size(); ++i) {
      if (std::to_string(pings_[i].id) != payload) continue;
      // Ordered transport: our ping was processed after every frame written before it.
      const uint64_t up_to = pings_[i].up_to;
      pings_.erase(pings_.begin(), pings_.begin() + static_cast<long>(i) + 1);
      if (up_to > confirmed_id_ && protocol_version_.load() == 0) confirm(up_to);
      return;
    }
  }

  void confirm(uint64_t send_id) {
    confirmed_id_ = send_id;
    uint64_t seq = persisted_.load();
    while (!in_flight_.empty() && in_flight_.front().send_id <= send_id) {
      seq = in_flight_.front().seq;
      in_flight_.pop_front();
    }
    if (seq > persisted_.load()) {
      if (!cursor_path_.empty()) (void)save_cursor(cursor_path_, Cursor{gate_.boot_id(), seq});
      persisted_ = seq;
    }
  }

  // Version 1: Takaro acknowledged every event of this boot's stream up to `seq`.
  void confirm_seq(uint64_t seq) {
    while (!in_flight_.empty() && in_flight_.front().seq <= seq) in_flight_.pop_front();
    if (seq > persisted_.load() && seq <= scan_.load()) {
      if (!cursor_path_.empty()) (void)save_cursor(cursor_path_, Cursor{gate_.boot_id(), seq});
      persisted_ = seq;
    }
  }

  void pump() {
    const auto page = gate_.events_since(scan_.load(), 64);
    if (page.truncated && !truncation_logged_) {
      log("takaro-event-ring-truncated before seq " + std::to_string(scan_.load()) + "; some events were lost");
      truncation_logged_ = true;
    }
    bool sent = false;
    for (const auto& event : page.events) {
      const auto data = map_event(event, known_);
      if (!data) {
        log("takaro-event-skipped seq=" + std::to_string(event.seq) + " type=" + event.type);
        scan_ = event.seq;
        in_flight_.push_back({send_id_, event.seq});  // confirmed with the next real send
        continue;
      }
      if (!event.id.empty() && !event.name.empty()) known_.remember(event.id, event.name);
      json::Value payload = json::obj({{"type", json::str(event.type)}, {"data", *data}});
      if (protocol_version_.load() >= 1) {
        // At-least-once on a per-boot stream: Takaro dedupes by (streamId, seq) and acks.
        payload.set("streamId", json::str(gate_.boot_id()));
        payload.set("seq", json::num(static_cast<double>(event.seq)));
      }
      const std::string frame = json::dump(json::obj({{"type", json::str("gameEvent")}, {"payload", payload}}));
      if (config_.wire_debug) log("takaro-wire SEND " + redact_frame(frame));
      if (!conn_.send_text(frame)) break;  // buffer full: retry on the next pump
      ++send_id_;
      scan_ = event.seq;
      in_flight_.push_back({send_id_, event.seq});
      sent = true;
    }
    // Version 0 has no event acks: confirm promptly with a ping instead of waiting
    // for the next heartbeat. Version 1 waits for Takaro's eventAck.
    const auto now = std::chrono::steady_clock::now();
    if (protocol_version_.load() == 0 && (sent || (!in_flight_.empty() && pings_.empty())) && now - last_eager_ping_ >= std::chrono::milliseconds(1000)) {
      last_eager_ping_ = now;
      send_ping();
    }
  }

  void handle_text(const std::string& raw) {
    if (config_.wire_debug) log("takaro-wire RECV " + redact_frame(raw));
    const auto message = json::parse(raw);
    if (!message || !message->is_object()) { log("takaro-ws-invalid-message"); return; }
    const json::Value* type = message->get("type");
    const std::string kind = type && type->is_string() ? type->as_string() : std::string();
    if (kind == "identifyResponse") {
      const json::Value& payload = as_record(message->get("payload"));
      const json::Value* failure = message->get("error") ? message->get("error") : payload.get("error");
      if (failure) {
        log("takaro-identify-failed " + redact_frame(json::dump(*failure)));
        set_error("identify rejected");
        conn_.send_close(1000);
        return;
      }
      std::string id;
      if (const auto* gsid = payload.get("gameServerId"); gsid && gsid->is_string()) id = gsid->as_string();
      else if (const auto* server = as_record(payload.get("server")).get("id"); server && server->is_string()) id = server->as_string();
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        game_server_id_ = id;
        last_error_.clear();
      }
      int version = 0;
      if (const auto* v = payload.get("protocolVersion"); v && v->is_number() && v->as_number() == 1) version = 1;
      protocol_version_ = version;
      adapter_.set_protocol_version(version);
      attempts_ = 0;
      identified_ = true;
      next_pump_ = std::chrono::steady_clock::now();
      log("takaro-identified gameServerId=" + id + " protocolVersion=" + std::to_string(version));
      return;
    }
    if (kind == "eventAck") {
      const json::Value& payload = as_record(message->get("payload"));
      const auto* stream = payload.get("streamId");
      const auto* up_to = payload.get("upTo");
      if (stream && stream->is_string() && stream->as_string() == gate_.boot_id() && up_to && up_to->is_number() &&
          up_to->as_number() >= 0)
        confirm_seq(static_cast<uint64_t>(up_to->as_number()));
      return;
    }
    if (kind == "eventError") {
      // Takaro rejected one event (validation); it still counts as handled in its ledger.
      log("takaro-event-rejected " + redact_frame(raw));
      return;
    }
    if (kind == "request") {
      const json::Value* rid = message->get("requestId");
      if (!rid || !rid->is_string() || rid->as_string().empty()) return;
      Request request;
      request.epoch = epoch_.load();
      request.request_id = rid->as_string();
      const json::Value& payload = as_record(message->get("payload"));
      if (const auto* action = payload.get("action"); action && action->is_string()) request.action = action->as_string();
      if (payload.has_key("args")) {
        for (const auto& [k, v] : payload.as_object()) if (k == "args") request.args = v;
      }
      {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        if (requests_.size() >= 64) { log("takaro-request-queue-full withheld"); return; }
        requests_.push_back(std::move(request));
      }
      requests_cv_.notify_one();
      return;
    }
    if (kind == "ping") { conn_.send_text("{\"type\":\"pong\"}"); return; }
    if (kind == "error") { log("takaro-error " + redact_frame(raw)); return; }
  }

  void worker_loop() {
    while (running_.load()) {
      Request request;
      {
        std::unique_lock<std::mutex> lock(requests_mutex_);
        requests_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return !requests_.empty() || !running_.load(); });
        if (!running_.load()) return;
        if (requests_.empty()) continue;
        request = std::move(requests_.front());
        requests_.pop_front();
      }
      const auto started = std::chrono::steady_clock::now();
      std::string outcome = "ok";
      try {
        if (request.action.empty()) withhold("request has no action");
        const json::Value args = normalize_args(&request.args);
        Adapter::AfterWrite after_write;
        json::Value payload = adapter_.handle(request.action, args, &after_write);
        const std::string frame = json::dump(json::obj({{"type", json::str("response")},
            {"requestId", json::str(request.request_id)}, {"payload", std::move(payload)}}));
        if (!post(request.epoch, frame, after_write)) outcome = "connection-gone";
      } catch (const Withhold& w) {
        outcome = std::string("withheld: ") + w.what();
        if (respond_error(request, w.code, w.what())) outcome = "error " + w.code + ": " + w.what();
      } catch (const std::exception& e) {
        outcome = std::string("withheld: internal ") + e.what();
        if (respond_error(request, "internal", "internal connector error")) outcome = "error internal";
      }
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      const std::string rid = request.request_id.size() <= 128 ? request.request_id : request.request_id.substr(0, 128);
      if (outcome != "ok" || config_.wire_debug)
        log("takaro-request action=" + request.action.substr(0, 64) + " id=" + rid + " ms=" + std::to_string(ms) +
            " result=" + outcome);
    }
  }

  // Version 1 has a real failure shape; version 0 must stay silent (fail closed).
  bool respond_error(const Request& request, const std::string& code, const std::string& message) {
    if (protocol_version_.load() < 1) return false;
    json::Value frame = json::obj({{"type", json::str("response")}, {"requestId", json::str(request.request_id)},
                                   {"payload", json::Value::null()},
                                   {"error", json::obj({{"code", json::str(code)}, {"message", json::str(message.substr(0, 512))}})}});
    return post(request.epoch, json::dump(frame));
  }

  void write_status() {
    if (config_.state_dir.empty()) return;
    json::Value status = json::Value::object();
    status.set("bootId", json::str(gate_.boot_id()));
    status.set("utc", json::str(gate::utc_now()));
    status.set("identified", json::Value::boolean(identified_.load()));
    status.set("protocolVersion", json::num(protocol_version_.load()));
    status.set("connections", json::num(static_cast<double>(connections_.load())));
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status.set("gameServerId", game_server_id_.empty() ? json::Value::null() : json::str(game_server_id_));
      status.set("lastError", last_error_.empty() ? json::Value::null() : json::str(last_error_));
    }
    status.set("eventCursor", json::num(static_cast<double>(persisted_.load())));
    status.set("eventScanCursor", json::num(static_cast<double>(scan_.load())));
    status.set("latestEventSeq", json::num(static_cast<double>(gate_.latest_sequence())));
    status.set("banReconciliationReady", json::Value::boolean(bans_.ready()));
    const auto h = gate_.health();
    status.set("gameReady", json::Value::boolean(h.ready));
    json::Value players = json::Value::array();
    for (const auto& p : gate_.players()) players.push(json::str(p.id));
    status.set("onlinePlayers", std::move(players));
    (void)write_atomic(config_.state_dir + "/status.json", json::dump(status) + "\n", 0644);
  }

  void maintenance_loop() {
    auto next_status = std::chrono::steady_clock::now();
    bool last_ok = true;
    while (running_.load()) {
      if (gate_.health().ready) {
        const bool ok = bans_.reconcile();
        if (ok != last_ok) log(ok ? "takaro-ban-reconciliation-ready" : "takaro-ban-reconciliation-failed");
        last_ok = ok;
      }
      for (const auto& p : gate_.players()) known_.remember(p.id, p.name);
      known_.flush();
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_status) { next_status = now + std::chrono::seconds(5); write_status(); }
      std::unique_lock<std::mutex> lock(maint_mutex_);
      maint_cv_.wait_for(lock, config_.reconcile_interval, [&] { return !running_.load(); });
    }
    write_status();
  }
};

}  // namespace takaro
