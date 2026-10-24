// Contract tests for the in-process Takaro bridge: a fake Takaro WebSocket
// server on loopback (plain ws://) and a fake game thread that executes the
// gate's queued actions. Covers WIRE-1..6, fail-closed void actions, getPlayer
// for offline players, ban metadata/expiry, event replay after an outage and
// the shutdown response-before-exit ordering.
#include "../src/takaro_bridge.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";    \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

const std::string kId = "76561198000000001";
const std::string kOther = "76561198000000002";

// ---------------------------------------------------------------- fake Takaro
class FakeTakaro {
 public:
  FakeTakaro() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listen_fd_, 4);
    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~FakeTakaro() {
    stop_ = true;
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    drop_client();
    if (thread_.joinable()) thread_.join();
  }
  std::string url() const { return "ws://127.0.0.1:" + std::to_string(port_) + "/"; }

  std::atomic<bool> answer_pings{true};
  std::atomic<bool> auto_identify{true};
  std::atomic<bool> v1{false};     // answer protocolVersion 1 when the connector offers it
  std::atomic<bool> acks{true};    // v1: acknowledge events with eventAck
  std::atomic<int> connections{0};

  void send(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ < 0) return;
    const std::string frame = ws::Codec::encode(0x1, text, false);
    (void)::send(client_, frame.data(), frame.size(), MSG_NOSIGNAL);
  }
  void drop_client() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (client_ >= 0) { shutdown(client_, SHUT_RDWR); }
  }
  // Text messages received so far (all connections).
  std::vector<json::Value> messages() {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
  }
  std::optional<json::Value> wait_for(const std::function<bool(const json::Value&)>& pred,
                                      std::chrono::milliseconds timeout = 5000ms, size_t from = 0) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = from; i < messages_.size(); ++i) if (pred(messages_[i])) return messages_[i];
      }
      std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
  }
  size_t count() { std::lock_guard<std::mutex> lock(mutex_); return messages_.size(); }

  std::optional<json::Value> request(const std::string& id, const std::string& action, json::Value args,
                                     std::chrono::milliseconds timeout = 5000ms) {
    const size_t from = count();
    json::Value payload = json::Value::object();
    payload.set("action", json::str(action));
    payload.set("args", std::move(args));
    send(json::dump(json::obj({{"type", json::str("request")}, {"requestId", json::str(id)}, {"payload", payload}})));
    return wait_for([&](const json::Value& m) {
      const auto* t = m.get("type");
      const auto* r = m.get("requestId");
      return t && t->as_string() == "response" && r && r->as_string() == id;
    }, timeout, from);
  }

 private:
  int listen_fd_ = -1;
  uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  int client_ = -1;
  std::vector<json::Value> messages_;

  void run() {
    while (!stop_) {
      const int c = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) { if (stop_) return; continue; }
      ++connections;
      serve(c);
      std::lock_guard<std::mutex> lock(mutex_);
      close(c);
      client_ = -1;
    }
  }

  void serve(int c) {
    std::string head;
    char buf[4096];
    while (head.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      head.append(buf, static_cast<size_t>(n));
    }
    const size_t at = head.find("Sec-WebSocket-Key: ");
    const std::string key = head.substr(at + 19, head.find("\r\n", at) - at - 19);
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " + ws::accept_key(key) + "\r\n\r\n";
    (void)::send(c, response.data(), response.size(), MSG_NOSIGNAL);
    { std::lock_guard<std::mutex> lock(mutex_); client_ = c; }
    std::string in = head.substr(head.find("\r\n\r\n") + 4);
    ws::Codec codec;
    for (;;) {
      std::vector<ws::Frame> frames;
      if (codec.decode(in, frames, true) == ws::Codec::Status::error) return;
      for (auto& f : frames) {
        if (f.kind == ws::Frame::Kind::ping) {
          if (answer_pings) {
            const std::string pong = ws::Codec::encode(0xA, f.payload, false);
            std::lock_guard<std::mutex> lock(mutex_);
            (void)::send(c, pong.data(), pong.size(), MSG_NOSIGNAL);
          }
        } else if (f.kind == ws::Frame::Kind::close) {
          return;
        } else if (f.kind == ws::Frame::Kind::text) {
          auto v = json::parse(f.payload);
          if (!v) continue;
          { std::lock_guard<std::mutex> lock(mutex_); messages_.push_back(*v); }
          const auto* t = v->get("type");
          if (t && t->as_string() == "identify" && auto_identify) {
            bool offered = false;
            if (const auto* versions = v->get("payload")->get("protocolVersions"))
              for (const auto& ver : versions->as_array()) offered |= ver.as_number() == 1;
            send(v1 && offered ? R"({"type":"identifyResponse","payload":{"gameServerId":"gs-1","protocolVersion":1}})"
                               : R"({"type":"identifyResponse","payload":{"gameServerId":"gs-1"}})");
          }
          if (t && t->as_string() == "gameEvent" && v1 && acks) {
            const auto* p = v->get("payload");
            if (p && p->get("streamId") && p->get("seq"))
              send(json::dump(json::obj({{"type", json::str("eventAck")}, {"payload", json::obj({
                  {"streamId", *p->get("streamId")}, {"upTo", *p->get("seq")}})}})));
          }
        }
      }
      const ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      in.append(buf, static_cast<size_t>(n));
    }
  }
};

// ---------------------------------------------------------------- fake game
class FakeGame {
 public:
  explicit FakeGame(gate::Server& gate) : gate_(gate) {
    gate_.set_game_ready();
    gate_.set_chat_ready();
    gate_.record_catalog({{"/Game/PrimalEarth/CoreBlueprints/Items/Consumables/PrimalItemConsumable_CookedMeat.PrimalItemConsumable_CookedMeat_C", "Cooked Meat"},
                          {"/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_Stone.PrimalItemResource_Stone_C", "Stone"}});
    gate_.record_entities({{"Dodo", "Dodo"}, {"Raptor", "Raptor"}});
    thread_ = std::thread([this] { run(); });
  }
  ~FakeGame() { stop_ = true; thread_.join(); }

  std::set<std::string> banned;
  std::mutex mutex;
  std::shared_ptr<gate::Action> last_shutdown;
  std::u32string last_message;
  std::u32string last_text() { std::lock_guard<std::mutex> lock(mutex); return last_message; }
  std::atomic<int> executed{0};
  std::atomic<bool> stall{false};

 private:
  gate::Server& gate_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  void run() {
    while (!stop_) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        gate_.record_bans(std::vector<std::string>(banned.begin(), banned.end()));
      }
      if (stall) { std::this_thread::sleep_for(5ms); continue; }
      while (auto action = gate_.take_action()) {
        bool execute = false;
        { std::lock_guard<std::mutex> l(action->mutex); if (!action->cancelled) { action->executing = true; execute = true; } }
        if (!execute) continue;
        ++executed;
        bool success = true;
        switch (action->kind) {
          case gate::Action::Kind::kick: gate_.record_logout(action->player_id); break;
          case gate::Action::Kind::ban: {
            std::lock_guard<std::mutex> lock(mutex);
            banned.insert(action->player_id);
            gate_.record_bans(std::vector<std::string>(banned.begin(), banned.end()));
            break;
          }
          case gate::Action::Kind::unban: {
            std::lock_guard<std::mutex> lock(mutex);
            banned.erase(action->player_id);
            gate_.record_bans(std::vector<std::string>(banned.begin(), banned.end()));
            break;
          }
          case gate::Action::Kind::shutdown: { std::lock_guard<std::mutex> lock(mutex); last_shutdown = action; break; }
          case gate::Action::Kind::console: action->output = "handled"; break;
          case gate::Action::Kind::message: { std::lock_guard<std::mutex> lock(mutex); last_message = action->text; break; }
          default: break;
        }
        gate::Server::complete(action, success);
      }
      std::this_thread::sleep_for(5ms);
    }
  }
};

std::string temp_dir() {
  char tmpl[] = "/tmp/ark-bridge-test-XXXXXX";
  const char* dir = mkdtemp(tmpl);
  return dir ? dir : "/tmp";
}

takaro::Config config_for(const FakeTakaro& takaro, const std::string& state) {
  takaro::Config config;
  config.url = takaro.url();
  config.registration_token = "registration-secret";
  config.identity_token = "identity-1";
  config.server_name = "ARK test";
  config.state_dir = state;
  config.ping_interval = 300ms;
  config.idle_timeout = 3000ms;
  config.reconnect_base = 100ms;
  config.reconnect_max = 400ms;
  config.pump_interval = 50ms;
  config.reconcile_interval = 100ms;
  return config;
}

bool is_type(const json::Value& m, const char* type) {
  const auto* t = m.get("type");
  return t && t->is_string() && t->as_string() == type;
}

template <typename F>
bool eventually(F f, std::chrono::milliseconds timeout = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) { if (f()) return true; std::this_thread::sleep_for(10ms); }
  return f();
}

void test_identify_and_wire() {
  FakeTakaro takaro;
  gate::Server gate("boot-1");
  FakeGame game(gate);
  takaro::Bridge bridge(config_for(takaro, temp_dir()), gate, [](const std::string&) {});
  bridge.start();
  auto identify = takaro.wait_for([](const json::Value& m) { return is_type(m, "identify"); });
  CHECK(identify);
  CHECK(eventually([&] { return bridge.identified(); }));
  CHECK(bridge.game_server_id() == "gs-1");

  // WIRE-3: every args shape answers with a bare array.
  int n = 0;
  for (const auto& args : {json::Value::array(), json::Value::object(), json::str("{}"), json::str("[]"),
                           json::str(""), json::Value::null()}) {
    auto response = takaro.request("players-" + std::to_string(n++), "getPlayers", args);
    CHECK(response && response->get("payload") && response->get("payload")->is_array());
  }
  // Invalid JSON string args: withheld.
  CHECK(!takaro.request("bad-args", "getPlayers", json::str("{nope"), 600ms));

  // WIRE-2: concurrent correlation.
  gate.record_login(kId, "Limon");
  std::thread a([&] { auto r = takaro.request("corr-a", "testReachability", json::Value::object()); CHECK(r); });
  std::thread b([&] { auto r = takaro.request("corr-b", "getPlayers", json::Value::object()); CHECK(r); });
  a.join(); b.join();
  auto reach = takaro.request("reach", "testReachability", json::Value::object());
  CHECK(reach && reach->get("payload")->get("connectable")->as_bool());

  // WIRE-4: flat vs nested player, JSON-string args, steam: prefix.
  auto flat = takaro.request("gp-flat", "getPlayer", json::obj({{"gameId", json::str(kId)}}));
  CHECK(flat && flat->get("payload")->get("name")->as_string() == "Limon");
  auto nested = takaro.request("gp-nested", "getPlayer",
      json::str("{\"player\":{\"gameId\":\"" + kId + "\",\"name\":\"x\"}}"));
  CHECK(nested && nested->get("payload")->get("steamId")->as_string() == kId);
  auto prefixed = takaro.request("gp-prefix", "getPlayer", json::obj({{"platformId", json::str("steam:" + kId)}}));
  CHECK(prefixed && prefixed->get("payload")->get("platformId")->as_string() == "steam:" + kId);
  // ip/ping: absent until the game thread verified them, never placeholders.
  CHECK(flat && !flat->get("payload")->get("ip") && !flat->get("payload")->get("ping"));
  gate.record_network(kId, std::string("192.0.2.10"), 48);
  gate.record_network(kId, std::nullopt, std::nullopt);  // a failed sample keeps the verified values
  gate.record_chat(kId, "Limon", "keeps network");       // chat refreshes the name only
  auto measured = takaro.request("gp-net", "getPlayer", json::obj({{"gameId", json::str(kId)}}));
  CHECK(measured && measured->get("payload")->get("ip")->as_string() == "192.0.2.10" &&
        measured->get("payload")->get("ping")->as_number() == 48);
  auto listed = takaro.request("gps-net", "getPlayers", json::Value::object());
  CHECK(listed && listed->get("payload")->as_array().size() == 1 &&
        listed->get("payload")->as_array()[0].get("ip")->as_string() == "192.0.2.10" &&
        listed->get("payload")->as_array()[0].get("ping")->as_number() == 48);

  // getPlayer for an offline player: a real IGamePlayer, never null.
  gate.record_logout(kId);
  auto offline = takaro.request("gp-offline", "getPlayer", json::obj({{"gameId", json::str(kId)}}));
  CHECK(offline && offline->get("payload")->is_object() && offline->get("payload")->get("gameId")->as_string() == kId);
  CHECK(offline && offline->get("payload")->get("name")->as_string() == "Limon");  // known name kept
  auto unknown = takaro.request("gp-unknown", "getPlayer", json::obj({{"gameId", json::str(kOther)}}));
  CHECK(unknown && unknown->get("payload")->get("gameId")->as_string() == kOther);

  // WIRE-6: unknown action and failed void action: no frame at all.
  CHECK(!takaro.request("unknown", "getMapTile", json::Value::object(), 700ms));
  CHECK(!takaro.request("kick-offline", "kickPlayer", json::obj({{"gameId", json::str(kId)}}), 700ms));
  // sendMessage to an offline recipient fails closed; broadcast succeeds.
  CHECK(!takaro.request("msg-offline", "sendMessage", json::obj({{"message", json::str("hi")},
      {"opts", json::obj({{"recipient", json::obj({{"gameId", json::str(kId)}})}})}}), 700ms));
  auto broadcast = takaro.request("msg-all", "sendMessage", json::obj({{"message", json::str("hello")},
      {"opts", json::obj({{"recipient", json::Value::null()}})}}));
  CHECK(broadcast && broadcast->get("payload")->is_object());
  auto named = takaro.request("msg-named", "sendMessage", json::obj({{"message", json::str("hi all")},
      {"opts", json::obj({{"senderNameOverride", json::str("Bot")}})}}));
  CHECK(named);
  CHECK(game.last_text() == U"Bot: hi all");

  // Explicit JSON nulls from modules are absent values.
  gate.record_login(kId, "Limon");
  auto tp = takaro.request("tp-null", "teleportPlayer", json::obj({{"gameId", json::str(kId)}, {"x", json::num(1.5)},
      {"y", json::num(2)}, {"z", json::num(-3)}, {"dimension", json::Value::null()}, {"target", json::Value::null()}}));
  CHECK(tp);
  auto give = takaro.request("give-null", "giveItem", json::obj({{"player", json::obj({{"gameId", json::str(kId)}})},
      {"item", json::str("/Game/PrimalEarth/CoreBlueprints/Resources/PrimalItemResource_Stone.PrimalItemResource_Stone_C")},
      {"amount", json::num(2)}, {"quality", json::Value::null()}, {"blueprint", json::Value::null()}}));
  CHECK(give);
  CHECK(!takaro.request("give-bad", "giveItem", json::obj({{"gameId", json::str(kId)}, {"item", json::str("Stone")}}), 600ms));
  CHECK(!takaro.request("tp-dim", "teleportPlayer", json::obj({{"gameId", json::str(kId)}, {"x", json::num(1)},
      {"y", json::num(2)}, {"z", json::num(3)}, {"dimension", json::str("nether")}}), 600ms));

  // Catalogs, search, console.
  auto items = takaro.request("items", "listItems", json::obj({{"search", json::Value::null()}}));
  CHECK(items && items->get("payload")->as_array().size() == 2);
  auto meat = takaro.request("items-search", "listItems", json::obj({{"search", json::str("cooked")}}));
  CHECK(meat && meat->get("payload")->as_array().size() == 1 &&
        meat->get("payload")->as_array()[0].get("name")->as_string() == "Cooked Meat");
  auto entities = takaro.request("entities", "listEntities", json::Value::array());
  CHECK(entities && entities->get("payload")->as_array().size() == 2);
  auto console = takaro.request("console", "executeConsoleCommand", json::obj({{"command", json::str("GetAll ShooterPlayerState PlayerName")}}));
  CHECK(console && console->get("payload")->get("success")->as_bool() &&
        console->get("payload")->get("rawResult")->as_string() == "handled");
  auto exit = takaro.request("console-exit", "executeConsoleCommand", json::obj({{"command", json::str("DoExit")}}));
  CHECK(exit && !exit->get("payload")->get("success")->as_bool() &&
        exit->get("payload")->get("errorMessage")->as_string() == "Use the dedicated shutdown action");

  // Locations: native object paths become human spawn-point names; tags win.
  gate.record_locations({{"/Game/Maps/TheIsland.TheIsland:PersistentLevel.PlayerStart_10",
                          "/Game/Maps/TheIsland.TheIsland:PersistentLevel.PlayerStart_10", 1, 2, 3, 10, 10, 10},
                         {"/Game/Maps/TheIsland.TheIsland:PersistentLevel.PlayerStart_11", "North Coast", 4, 5, 6, 10, 10, 10}});
  auto locations = takaro.request("locations", "listLocations", json::Value::object());
  CHECK(locations && locations->get("payload")->as_array().size() == 2 &&
        locations->get("payload")->as_array()[0].get("name")->as_string() == "Spawn point 10" &&
        locations->get("payload")->as_array()[1].get("name")->as_string() == "North Coast");

  // Kick with reason: notice then verified departure.
  auto kick = takaro.request("kick", "kickPlayer", json::obj({{"gameId", json::str(kId)}, {"reason", json::str("bye")}}));
  CHECK(kick);
  CHECK(!gate.player(kId));
  bridge.stop();
}

void test_events_cursor_and_outage_replay() {
  FakeTakaro takaro;
  gate::Server gate("boot-2");
  FakeGame game(gate);
  const std::string state = temp_dir();
  std::vector<std::string> logs;
  std::mutex logs_mutex;
  takaro::Bridge bridge(config_for(takaro, state), gate, [&](const std::string& l) {
    std::lock_guard<std::mutex> lock(logs_mutex);
    logs.push_back(l);
  });
  bridge.start();
  CHECK(eventually([&] { return bridge.identified(); }));
  gate.record_login(kId, "Limon");
  gate.record_chat(kId, "Limon", "hello world");
  auto chat = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "chat-message";
  });
  CHECK(chat);
  if (chat) {
    const auto* data = chat->get("payload")->get("data");
    CHECK(data->get("msg")->as_string() == "hello world");
    CHECK(data->get("channel")->as_string() == "global");
    CHECK(data->get("player")->get("steamId")->as_string() == kId);
    CHECK(data->get("player")->get("platformId")->as_string() == "steam:" + kId);
  }
  CHECK(eventually([&] { return bridge.persisted_cursor() == 2; }));
  const auto saved = takaro::load_cursor(state + "/event-cursor.json");
  CHECK(saved.boot_id == "boot-2" && saved.seq == 2);

  // A log line carrying an address is never sent, and does not block later events.
  gate.record_log("Server listening on 10.0.0.5");
  gate.record_log("World Save Complete. Took 0.9");
  auto save = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "log";
  });
  CHECK(save && save->get("payload")->get("data")->get("msg")->as_string() == "World Save Complete. Took 0.9");
  CHECK(eventually([&] { return bridge.persisted_cursor() == 4; }));

  // Outage: Takaro stops answering pings (our bytes are not proven delivered).
  takaro.answer_pings = false;
  const size_t before = takaro.count();
  gate.record_chat(kId, "Limon", "during outage");
  CHECK(takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("data")->get("msg") &&
           m.get("payload")->get("data")->get("msg")->as_string() == "during outage";
  }, 3000ms, before));
  // Missed pongs force a reconnect; the unconfirmed event is replayed.
  CHECK(eventually([&] { return takaro.connections.load() >= 2; }, 5000ms));
  takaro.answer_pings = true;
  // At-least-once: the same native event arrives again on the new connection.
  CHECK(eventually([&] {
    int seen = 0;
    for (const auto& m : takaro.messages())
      if (is_type(m, "gameEvent") && m.get("payload")->get("data")->get("msg") &&
          m.get("payload")->get("data")->get("msg")->as_string() == "during outage") ++seen;
    return seen >= 2;
  }, 5000ms));
  CHECK(eventually([&] { return bridge.persisted_cursor() == 5; }));

  // A death with a captured position carries it in the event.
  gate.record_death_position(kId, 5, 6, 7, 1, 1);
  gate.record_player_death(kId, "Limon", "");
  auto death = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "player-death";
  });
  CHECK(death && death->get("payload")->get("data")->get("position") &&
        death->get("payload")->get("data")->get("position")->get("x")->as_number() == 5);

  // entity-killed carries the held weapon's display name; unarmed stays empty.
  CHECK(eventually([&] { return gate.entities().has_value(); }));
  gate.record_entity_killed(kId, "Dodo", "Dodo", "Sword");
  auto armed = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "entity-killed";
  });
  CHECK(armed && armed->get("payload")->get("data")->get("weapon")->as_string() == "Sword" &&
        armed->get("payload")->get("data")->get("entity")->as_string() == "Dodo");
  const auto before_unarmed = takaro.messages().size();
  gate.record_entity_killed(kId, "Raptor", "Raptor");
  auto unarmed = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "entity-killed" &&
           m.get("payload")->get("data")->get("entity")->as_string() == "Raptor";
  }, 3000ms, before_unarmed);
  CHECK(unarmed && unarmed->get("payload")->get("data")->get("weapon")->as_string().empty());

  // Player disconnect carries a position the location request can enrich.
  gate.record_position(kId, 10, 20, 30);
  gate.record_departure_position(kId, 11, 21, 31);
  gate.record_logout(kId);
  CHECK(takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "player-disconnected";
  }));
  auto location = takaro.request("loc", "getPlayerLocation", json::obj({{"gameId", json::str(kId)}}));
  CHECK(location && location->get("payload")->get("x")->as_number() == 11);
  CHECK(location && !location->get("payload")->get("dimension"));  // v0: x, y, z only
  bridge.stop();
}

void test_bans_and_expiry() {
  FakeTakaro takaro;
  gate::Server gate("boot-3");
  FakeGame game(gate);
  const std::string state = temp_dir();
  takaro::Bridge bridge(config_for(takaro, state), gate, [](const std::string&) {});
  bridge.start();
  CHECK(eventually([&] { return bridge.identified(); }));
  gate.record_login(kId, "Limon");
  gate.record_logout(kId);
  CHECK(eventually([&] { return takaro::read_file(state + "/known-players.json").has_value(); }));
  // Timed ban 1.5 s ahead.
  const std::string until = takaro::iso_ms(takaro::now_ms() + 1500);
  auto ban = takaro.request("ban", "banPlayer", json::obj({{"gameId", json::str(kId)}, {"reason", json::str("griefing")},
                                                           {"expiresAt", json::str(until)}}));
  CHECK(ban);
  auto list = takaro.request("bans", "listBans", json::Value::object());
  CHECK(list && list->get("payload")->as_array().size() == 1);
  if (list && list->get("payload")->as_array().size() == 1) {
    const auto& row = list->get("payload")->as_array()[0];
    CHECK(row.get("reason")->as_string() == "griefing");
    CHECK(row.get("expiresAt")->as_string() == until);
    CHECK(row.get("player")->get("name")->as_string() == "Limon");  // never the raw id
  }
  const auto file = takaro::read_file(state + "/ban-metadata.json");
  CHECK(file && file->find("\"griefing\"") != std::string::npos);
  // The maintenance thread lifts the expired ban natively.
  CHECK(eventually([&] { std::lock_guard<std::mutex> l(game.mutex); return game.banned.empty(); }, 5000ms));
  auto after = takaro.request("bans-after", "listBans", json::Value::object());
  CHECK(after && after->get("payload")->as_array().empty());
  // Permanent sentinel maps to null; unban clears metadata.
  auto permanent = takaro.request("ban-perm", "banPlayer", json::obj({{"gameId", json::str(kId)}, {"reason", json::str("x")},
                                                                      {"expiresAt", json::str(takaro::kPermanentSentinel)}}));
  CHECK(permanent);
  auto perm_list = takaro.request("bans-perm", "listBans", json::Value::object());
  CHECK(perm_list && perm_list->get("payload")->as_array().size() == 1 &&
        !perm_list->get("payload")->as_array()[0].get("expiresAt") &&
        perm_list->get("payload")->as_array()[0].has_key("expiresAt"));
  CHECK(takaro.request("unban", "unbanPlayer", json::obj({{"gameId", json::str(kId)}, {"reason", json::Value::null()}})));
  CHECK(!takaro.request("unban-reason", "unbanPlayer", json::obj({{"gameId", json::str(kId)}, {"reason", json::str("no")}}), 600ms));
  // A native ban without metadata: listBans fails closed (no frame).
  { std::lock_guard<std::mutex> l(game.mutex); game.banned.insert(kOther); }
  std::this_thread::sleep_for(100ms);  // next game-thread ban sample
  CHECK(!takaro.request("bans-unknown", "listBans", json::Value::object(), 700ms));
  { std::lock_guard<std::mutex> l(game.mutex); game.banned.erase(kOther); }
  std::this_thread::sleep_for(100ms);
  // Expired expiresAt is refused.
  CHECK(!takaro.request("ban-past", "banPlayer", json::obj({{"gameId", json::str(kId)},
      {"expiresAt", json::str("2020-01-01T00:00:00.000Z")}}), 600ms));
  bridge.stop();
  // Corrupt metadata refuses to start blind.
  takaro::write_atomic(state + "/ban-metadata.json", "{\"version\":2}");
  bool refused = false;
  try { takaro::Bridge again(config_for(takaro, state), gate, nullptr); } catch (const std::exception&) { refused = true; }
  CHECK(refused);
}

void test_shutdown_response_then_exit_flag() {
  FakeTakaro takaro;
  gate::Server gate("boot-4");
  FakeGame game(gate);
  takaro::Bridge bridge(config_for(takaro, temp_dir()), gate, [](const std::string&) {});
  bridge.start();
  CHECK(eventually([&] { return bridge.identified(); }));
  gate.record_login(kId, "Limon");
  CHECK(!takaro.request("shutdown-args", "shutdown", json::obj({{"force", json::Value::boolean(true)}}), 600ms));
  auto response = takaro.request("shutdown", "shutdown", json::obj({{"force", json::Value::null()}}));
  CHECK(response);
  // Players still online get a delivered player-disconnected before the ACK.
  bool departed_first = false;
  {
    const auto messages = takaro.messages();
    size_t departure = messages.size(), ack = messages.size();
    for (size_t i = 0; i < messages.size(); ++i) {
      if (is_type(messages[i], "gameEvent") && messages[i].get("payload")->get("type")->as_string() == "player-disconnected") departure = std::min(departure, i);
      if (is_type(messages[i], "response") && messages[i].get("requestId")->as_string() == "shutdown") ack = i;
    }
    departed_first = departure < ack;
  }
  CHECK(departed_first);
  CHECK(gate.players().empty());
  CHECK(eventually([&] {
    std::lock_guard<std::mutex> l(game.mutex);
    return game.last_shutdown && game.last_shutdown->response_sent.load();
  }));
  bridge.stop();
}

void test_game_thread_stall_withholds() {
  // A stalled game thread (the 2026-10-06 host freeze) must not answer with a
  // false success, must not block the I/O thread, and must recover after.
  FakeTakaro takaro;
  gate::Server gate("boot-5");
  FakeGame game(gate);
  takaro::Bridge bridge(config_for(takaro, temp_dir()), gate, [](const std::string&) {});
  bridge.start();
  CHECK(eventually([&] { return bridge.identified(); }));
  gate.record_login(kId, "Limon");
  game.stall = true;
  const auto started = std::chrono::steady_clock::now();
  CHECK(!takaro.request("msg-stalled", "sendMessage", json::obj({{"message", json::str("x")}}), 4000ms));
  // Reads stay served from snapshots while the game thread is stalled.
  auto players = takaro.request("players-stalled", "getPlayers", json::Value::object(), 1500ms);
  CHECK(players && players->get("payload")->as_array().size() == 1);
  CHECK(std::chrono::steady_clock::now() - started < 6000ms);
  game.stall = false;
  auto msg = takaro.request("msg-after", "sendMessage", json::obj({{"message", json::str("y")}}));
  CHECK(msg);
  bridge.stop();
}

void test_identify_rejected_backs_off() {
  FakeTakaro takaro;
  takaro.auto_identify = false;
  gate::Server gate("boot-6");
  FakeGame game(gate);
  takaro::Bridge bridge(config_for(takaro, temp_dir()), gate, [](const std::string&) {});
  bridge.start();
  auto identify = takaro.wait_for([](const json::Value& m) { return is_type(m, "identify"); });
  CHECK(identify);
  if (identify) {
    const auto* payload = identify->get("payload");
    CHECK(payload->get("registrationToken")->as_string() == "registration-secret");
    CHECK(payload->get("identityToken")->as_string() == "identity-1");
    CHECK(payload->get("name")->as_string() == "ARK test");
  }
  takaro.send(R"({"type":"identifyResponse","payload":{"error":{"message":"Invalid registrationToken provided"}}})");
  CHECK(eventually([&] { return takaro.connections.load() >= 2; }, 4000ms));
  CHECK(!bridge.identified());
  // Requests before identify are not answered.
  bridge.stop();
}

void test_redaction_and_codec() {
  const std::string frame = R"({"type":"identify","payload":{"identityToken":"abc","registrationToken":"sekret"}})";
  const std::string redacted = takaro::redact_frame(frame);
  CHECK(redacted.find("sekret") == std::string::npos && redacted.find("abc") == std::string::npos);
  // Fragmented text + interleaved ping decode.
  ws::Codec codec;
  std::string in;
  in += std::string("\x01\x03" "abc", 5);
  in += std::string("\x89\x01" "p", 3);
  in += std::string("\x80\x02" "de", 4);
  std::vector<ws::Frame> frames;
  CHECK(codec.decode(in, frames) == ws::Codec::Status::need_more);
  CHECK(frames.size() == 2 && frames[0].kind == ws::Frame::Kind::ping && frames[1].payload == "abcde");
  // Masked server frames are a protocol error.
  ws::Codec strict;
  std::string masked = ws::Codec::encode(0x1, "x", true);
  std::vector<ws::Frame> none;
  CHECK(strict.decode(masked, none) == ws::Codec::Status::error);
  // Round-trip with 16-bit and 64-bit lengths.
  for (size_t size : {125u, 126u, 65535u, 65536u, 200000u}) {
    ws::Codec c;
    std::string big(size, 'x');
    std::string encoded = ws::Codec::encode(0x1, big, true);
    std::vector<ws::Frame> out;
    CHECK(c.decode(encoded, out, true) != ws::Codec::Status::error && out.size() == 1 && out[0].payload == big);
  }
  CHECK(ws::parse_url("wss://connect.k8s.takaro.dev/")->host == "connect.k8s.takaro.dev");
  CHECK(ws::parse_url("ws://127.0.0.1:3004/x")->port == 3004);
  CHECK(!ws::parse_url("https://x/"));
  CHECK(!ws::parse_url("wss://user@host/"));
  // ISO dates.
  CHECK(takaro::parse_iso_ms("2026-10-12T10:00:00Z") == takaro::parse_iso_ms("2026-10-12T12:00:00.000+02:00"));
  CHECK(!takaro::parse_iso_ms("2026-02-31T10:00:00Z"));
  CHECK(!takaro::parse_iso_ms("2026-10-12T10:00:00"));
  CHECK(takaro::iso_ms(*takaro::parse_iso_ms("2026-10-12T10:00:00.5Z")) == "2026-10-12T10:00:00.500Z");
}

void test_protocol_v1() {
  FakeTakaro takaro;
  takaro.v1 = true;
  takaro.acks = false;
  gate::Server gate("boot-v1");
  FakeGame game(gate);
  const std::string state = temp_dir();
  auto config = config_for(takaro, state);
  config.ping_interval = 5000ms;  // pongs must not confirm anything on v1
  config.map_name = "TheIsland";
  takaro::Bridge bridge(config, gate, [](const std::string&) {});
  bridge.start();
  auto identify = takaro.wait_for([](const json::Value& m) { return is_type(m, "identify"); });
  CHECK(identify && identify->get("payload")->get("protocolVersions"));
  bool locations = false, map = false;
  if (identify)
    for (const auto& c : identify->get("payload")->get("capabilities")->as_array()) {
      locations |= c.get("name")->as_string() == "locations.list";
      if (c.get("name")->as_string() != "map") continue;
      const auto& dim = c.get("parameters")->get("dimensions")->as_array()[0];
      map = dim.get("id")->as_string() == "TheIsland" &&
            dim.get("bounds")->get("horizontal")->get("min")->as_number() == -400000 &&
            dim.get("bounds")->get("vertical")->get("max")->as_number() == 400000 &&
            dim.get("axes")->get("vertical")->get("direction")->as_string() == "down" &&
            dim.get("source")->get("type")->as_string() == "none";
    }
  CHECK(locations && map);
  CHECK(!takaro::map_dimension("Ragnarok"));  // no declared map without a verified calibration
  CHECK(eventually([&] { return bridge.identified(); }));
  // Structured errors instead of silence.
  auto kick = takaro.request("v1-kick", "kickPlayer", json::obj({{"gameId", json::str(kId)}}), 2000ms);
  CHECK(kick && kick->has_key("payload") && !kick->get("payload") &&
        kick->get("error")->get("code")->as_string() == "game_error");
  auto unknown = takaro.request("v1-unknown", "getMapImage", json::Value::object(), 2000ms);
  CHECK(unknown && unknown->get("error")->get("code")->as_string() == "unsupported");
  auto bad = takaro.request("v1-bad", "giveItem", json::obj({{"gameId", json::str(kId)}, {"item", json::str("Stone")}}), 2000ms);
  CHECK(bad && bad->get("error")->get("code")->as_string() == "invalid_args");
  auto noid = takaro.request("v1-noid", "getPlayer", json::Value::object(), 2000ms);
  CHECK(noid && noid->get("error")->get("code")->as_string() == "invalid_args");
  // Events carry the boot stream and native seq; only eventAck advances the cursor.
  gate.record_login(kId, "Limon", "198.51.100.7", 35);
  auto connected = takaro.wait_for([](const json::Value& m) {
    return is_type(m, "gameEvent") && m.get("payload")->get("type")->as_string() == "player-connected";
  });
  CHECK(connected && connected->get("payload")->get("streamId")->as_string() == "boot-v1" &&
        connected->get("payload")->get("seq")->as_number() == 1);
  CHECK(connected && connected->get("payload")->get("data")->get("player")->get("ip")->as_string() == "198.51.100.7" &&
        connected->get("payload")->get("data")->get("player")->get("ping")->as_number() == 35);
  // Positions name the declared Dimension; teleports accept it and refuse any other.
  gate.record_position(kId, -113926.75f, 234912.75f, -14334.5f);
  auto location = takaro.request("v1-loc", "getPlayerLocation", json::obj({{"gameId", json::str(kId)}}), 2000ms);
  CHECK(location && location->get("payload")->get("dimension")->as_string() == "TheIsland" &&
        location->get("payload")->get("y")->as_number() == 234912.75);
  auto tp = takaro.request("v1-tp", "teleportPlayer", json::obj({{"gameId", json::str(kId)}, {"x", json::num(1)},
      {"y", json::num(2)}, {"z", json::num(3)}, {"dimension", json::str("TheIsland")}}), 2000ms);
  CHECK(tp && tp->get("payload") && tp->get("payload")->is_object());
  auto tp_other = takaro.request("v1-tp-other", "teleportPlayer", json::obj({{"gameId", json::str(kId)}, {"x", json::num(1)},
      {"y", json::num(2)}, {"z", json::num(3)}, {"dimension", json::str("Ragnarok")}}), 2000ms);
  CHECK(tp_other && tp_other->get("error")->get("code")->as_string() == "unsupported");
  std::this_thread::sleep_for(1500ms);
  CHECK(bridge.persisted_cursor() == 0);
  takaro.acks = true;
  gate.record_chat(kId, "Limon", "acked");
  CHECK(eventually([&] { return bridge.persisted_cursor() == 2; }));
  CHECK(takaro::load_cursor(state + "/event-cursor.json").seq == 2);
  bridge.stop();
}

int main() {
  test_redaction_and_codec();
  test_identify_and_wire();
  test_events_cursor_and_outage_replay();
  test_bans_and_expiry();
  test_shutdown_response_then_exit_flag();
  test_game_thread_stall_withholds();
  test_identify_rejected_backs_off();
  test_protocol_v1();
  if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
  std::cout << "takaro_bridge_test passed\n";
  return 0;
}
