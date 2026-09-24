#include "../src/gate_http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <limits>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
constexpr char token[] = "0123456789abcdef0123456789abcdef";
constexpr char steam[] = "76561198000000000";

void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}

int free_loopback_port() {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd >= 0, "port reservation socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  check(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "port reservation bind");
  socklen_t length = sizeof(address);
  check(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0, "port reservation getsockname");
  const int port = ntohs(address.sin_port);
  close(fd);
  return port;
}

std::string request(int port, const std::string& path, const std::string& auth = token,
                    const std::string& body = {}, const std::string& method = "GET",
                    const std::string& extra_headers = {}) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  check(fd >= 0, "request socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(port));
  check(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "request connect");
  timeval timeout{10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const std::string wire = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" +
      (auth.empty() ? "" : "Authorization: Bearer " + auth + "\r\n") + extra_headers +
      "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < wire.size()) {
    const ssize_t n = send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
    check(n > 0, "request send");
    sent += static_cast<size_t>(n);
  }
  std::string out;
  char buffer[4096];
  for (;;) {
    const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) break;
    check(n > 0, "request recv");
    out.append(buffer, static_cast<size_t>(n));
  }
  close(fd);
  return out;
}

bool status(const std::string& response, int code) {
  return response.rfind("HTTP/1.1 " + std::to_string(code) + " ", 0) == 0;
}

size_t count(const std::string& text, const std::string& needle) {
  size_t n = 0, at = 0;
  while ((at = text.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
  return n;
}

std::shared_ptr<gate::Action> wait_action(gate::Server& server) {
  for (int i = 0; i < 1000; ++i) {
    if (auto action = server.take_action()) return action;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  throw std::runtime_error("action was not queued");
}
} // namespace

int main() {
  try {
    gate::Server weak("short", "boot");
    check(!weak.start(free_loopback_port()), "short bearer token must be rejected");
    gate::Server server(token, "test-boot");
    const int port = free_loopback_port();
    check(server.start(port), "server starts on loopback");
    server.set_game_ready();

    check(status(request(port, "/health", ""), 401), "missing bearer rejected");
    check(status(request(port, "/health", "wrong"), 401), "wrong bearer rejected");
    check(status(request(port, "/health", token, {}, "GET", "Authorization: Bearer duplicate\r\n"), 401),
          "duplicate bearer rejected");
    const auto health = request(port, "/health");
    check(status(health, 200) && health.find("\"bootId\":\"test-boot\"") != std::string::npos,
          "authorized health and boot ID");
    gate::Server message_server(token, "message-boot");
    const int message_port = free_loopback_port();
    check(message_server.start(message_port), "message-only server starts on loopback");
    message_server.set_game_ready();
    auto empty_broadcast = std::async(std::launch::async, [&] {
      return request(message_port, "/message", token, "zero recipients", "POST");
    });
    auto empty_action = wait_action(message_server);
    check(empty_action->kind == gate::Action::Kind::message && empty_action->player_id.empty() &&
          empty_action->text == U"zero recipients" && message_server.player_ids().empty(),
          "zero-recipient broadcast still reaches the game-thread queue");
    gate::Server::complete(empty_action, true);
    check(status(empty_broadcast.get(), 200), "completed empty broadcast is an acknowledged no-op");
    check(status(request(message_port, std::string("/players/") + steam + "/message", token, "private", "POST"), 503),
          "targeted message to an absent player still fails");
    message_server.record_login(steam, "One");
    auto one_broadcast = std::async(std::launch::async, [&] {
      return request(message_port, "/message", token, "one recipient", "POST");
    });
    auto one_action = wait_action(message_server);
    check(one_action->kind == gate::Action::Kind::message && one_action->player_id.empty() &&
          message_server.player_ids().size() == 1, "single-recipient broadcast uses the same queue");
    gate::Server::complete(one_action, true);
    check(status(one_broadcast.get(), 200), "single-recipient broadcast acknowledges dispatch");
    message_server.record_login("76561198000000009", "Two");
    auto many_broadcast = std::async(std::launch::async, [&] {
      return request(message_port, "/message", token, "two recipients", "POST");
    });
    auto many_action = wait_action(message_server);
    check(many_action->kind == gate::Action::Kind::message && many_action->player_id.empty() &&
          message_server.player_ids().size() == 2, "multi-recipient broadcast uses the same queue");
    gate::Server::complete(many_action, true);
    check(status(many_broadcast.get(), 200), "multi-recipient broadcast acknowledges dispatch");
    message_server.stop();
    check(status(request(port, "/not-implemented"), 501), "unknown route is unavailable");
    check(status(request(port, "/items?offset=0&limit=128"), 503),
          "catalog stays unavailable until a complete native snapshot exists");
    check(status(request(port, "/entities?offset=0&limit=128"), 503),
          "entity catalog stays unavailable until a complete native snapshot exists");
    check(status(request(port, "/locations"), 503),
          "locations remain unavailable until actual loaded PlayerStart actors are sampled");
    check(status(request(port, "/bans"), 503),
          "ban list stays unavailable until native collection is validated");
    check(status(request(port, "/bans", "wrong"), 401),
          "ban list requires bearer authorization");
    server.record_bans({steam});
    check(status(request(port, "/bans"), 200) &&
          request(port, "/bans").find("\r\n\r\n[\"76561198000000000\"]") != std::string::npos,
          "ban list returns only published native Steam IDs");
    server.clear_bans();
    check(status(request(port, "/bans"), 503),
          "failed native ban snapshot cannot return stale data");
    server.record_bans({});
    check(status(request(port, "/bans"), 200) &&
          request(port, "/bans").find("\r\n\r\n[]") != std::string::npos,
          "validated empty native collection may return empty list");
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    check(status(request(port, "/bans"), 503),
          "stalled game-thread ban snapshot cannot remain readable indefinitely");
    server.record_locations({{"/Game/TheIsland/StartA", "South Zone 1", 12.5f, -4.0f, 300.0f,
                              100.0f, 90.0f, 180.0f}});
    const auto locations = request(port, "/locations");
    check(status(locations, 200) &&
          locations.find("\"code\":\"/Game/TheIsland/StartA\"") != std::string::npos &&
          locations.find("\"x\":12.500000") != std::string::npos &&
          locations.find("\"sizeX\":100.000000") != std::string::npos,
          "native location endpoint returns actual code, position and geometry");
    check(status(request(port, "/items?offset=x&limit=128"), 400),
          "catalog rejects malformed offset");
    check(status(request(port, "/items?offset=0&limit=129"), 400),
          "catalog enforces bounded page size");
    server.record_catalog({{"/Game/Stone.Stone_C", "Stone"}, {"/Game/Wood.Wood_C", "Wood"}});
    server.record_entities({{"Dodo", "Dodo"}, {"Raptor", "Raptor"}});
    const auto entity_page = request(port, "/entities?offset=0&limit=1");
    check(status(entity_page, 200) && entity_page.find("\"total\":2") != std::string::npos &&
          entity_page.find("\"code\":\"Dodo\"") != std::string::npos,
          "published native entity catalog pages exactly");
    const auto item_page = request(port, "/items?offset=0&limit=1");
    check(status(item_page, 200) && item_page.find("\"nextOffset\":1,\"total\":2,\"complete\":false") != std::string::npos &&
          item_page.find("/Game/Stone.Stone_C") != std::string::npos,
          "catalog page carries exact native class path and continuation");
    check(status(request(port, "/items?offset=2&limit=1"), 200) &&
          request(port, "/items?offset=2&limit=1").find("\"complete\":true") != std::string::npos,
          "catalog final empty page is explicitly complete");
    server.clear_catalog();
    check(status(request(port, "/items?offset=0&limit=1"), 503),
          "failed native catalog refresh never serves stale data");

    check(status(request(port, "/events?since=x"), 400), "non-numeric cursor rejected");
    check(status(request(port, "/events?since=18446744073709551616"), 400), "overflow cursor rejected");
    check(status(request(port, "/events?since=18446744073709551615"), 200), "max uint64 cursor accepted");

    for (int i = 0; i < 140; ++i) server.record_chat(steam, "Survivor", "line " + std::to_string(i));
    const auto first = request(port, "/events?since=0");
    check(status(first, 200) && count(first, "\"type\":\"chat-message\"") == 128 &&
          first.find("\"seq\":128,\"latestSeq\":140,\"hasMore\":true") != std::string::npos,
          "first page contains 128 ordered events and continuation cursor");
    const auto second = request(port, "/events?since=128");
    check(status(second, 200) && count(second, "\"type\":\"chat-message\"") == 12 &&
          second.find("\"seq\":140,\"latestSeq\":140,\"hasMore\":false") != std::string::npos,
          "second page completes without skipping events");
    for (int i = 140; i < 4100; ++i) server.record_chat(steam, "Survivor", "line " + std::to_string(i));
    const auto truncated = request(port, "/events?since=0");
    check(status(truncated, 200) && truncated.find("\"truncated\":true") != std::string::npos &&
          truncated.find("\"seq\":5,\"type\":\"chat-message\"") != std::string::npos,
          "ring truncation reports missing prefix and starts at first retained sequence");

    constexpr char location_steam[] = "76561198000000001";
    const std::string location_path = std::string("/players/") + location_steam + "/location";
    check(status(request(port, location_path), 503), "location unavailable without a player");
    server.record_position(location_steam, 1, 2, 3);
    check(status(request(port, location_path), 503), "position cannot be recorded for an absent player");
    server.record_login(location_steam, "Survivor");
    server.record_player_death(location_steam, "Survivor", "76561198000000002");
    const auto death_event = request(port, "/events?since=4101");
    check(status(death_event, 200) &&
          death_event.find("\"type\":\"player-death\"") != std::string::npos &&
          death_event.find("\"killer\":{\"gameId\":\"76561198000000002\"") != std::string::npos,
          "death event preserves validated victim and distinct attacker DTO");
    check(status(request(port, location_path), 503), "new player has no invented position");
    server.record_position(location_steam, 12.5f, -4.25f, 300.0f);
    const auto location = request(port, location_path);
    check(status(location, 200) && location.find("{\"x\":12.500000,\"y\":-4.250000,\"z\":300.000000}") != std::string::npos,
          "fresh finite player position is returned exactly");
    check(status(request(port, location_path, "wrong"), 401), "location requires bearer authorization");
    server.record_position(location_steam, std::numeric_limits<float>::quiet_NaN(), 0, 0);
    server.record_position(location_steam, 0, std::numeric_limits<float>::infinity(), 0);
    check(request(port, location_path) == location, "non-finite coordinates do not replace the last finite position");
    server.clear_position(location_steam);
    check(status(request(port, location_path), 503), "cleared position is unavailable");
    server.record_position(location_steam, 1, 2, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    check(status(request(port, location_path), 503), "stale position is unavailable");
    server.record_position(location_steam, 1, 2, 3);
    server.record_logout(location_steam);
    check(status(request(port, location_path), 200) &&
          request(port, location_path).find("\"source\":\"departure\"") != std::string::npos,
          "logout offers only the recent actual game-thread position for event enrichment");
    check(status(request(port, std::string("/players/") + location_steam + "/inventory"), 503),
          "logout still invalidates live inventory and actor access");

    server.record_login(location_steam, "Survivor");
    server.record_position(location_steam, 4, 5, 6);
    server.record_departure_position(location_steam, 4, 5, 6);
    server.record_logout(location_steam);
    const auto departure = request(port, location_path);
    check(status(departure, 200) && departure.find("\"source\":\"departure\"") != std::string::npos &&
          departure.find("\"x\":4.000000") != std::string::npos,
          "actual game-thread departure position is attached to the replayable event");
    check(status(request(port, std::string("/players/") + location_steam), 200) &&
          request(port, std::string("/players/") + location_steam).find("\r\n\r\nnull") != std::string::npos,
          "departure snapshot does not make an offline player appear online");
    constexpr char death_steam[] = "76561198000000003";
    const std::string death_path = std::string("/players/") + death_steam + "/location";
    server.record_login(death_steam, "Fallen");
    server.record_death_position(death_steam, 7, 8, 9, 31, 41);
    server.record_player_death(death_steam, "Fallen", "");
    constexpr char stale_steam[] = "76561198000000005";
    const std::string stale_path = std::string("/players/") + stale_steam + "/location";
    server.record_login(stale_steam, "Stale");
    server.record_departure_position(stale_steam, 90, 91, 92);
    constexpr char stale_death_steam[] = "76561198000000007";
    const std::string stale_death_path = std::string("/players/") + stale_death_steam + "/location";
    server.record_login(stale_death_steam, "StaleDeath");
    server.record_death_position(stale_death_steam, 93, 94, 95, 70, 80);
    std::this_thread::sleep_for(std::chrono::seconds(16));
    server.record_logout(stale_steam);
    check(status(request(port, stale_path), 503),
          "an old unconsumed pre-Logout capture cannot become a fresh departure event");
    server.record_player_death(stale_death_steam, "StaleDeath", "");
    check(status(request(port, stale_death_path), 503),
          "an old unconsumed pre-death capture cannot become a fresh death event");
    check(status(request(port, location_path), 200) &&
          request(port, location_path).find("\"source\":\"departure\"") != std::string::npos,
          "departure enrichment survives an outage beyond the old 15-second TTL");
    check(status(request(port, death_path), 200) &&
          request(port, death_path).find("\"source\":\"death\"") != std::string::npos,
          "death enrichment survives an outage while its event remains replayable");
    server.record_position(death_steam, 7, 8, 9, 31, 41);
    server.clear_position(death_steam);
    check(status(request(port, death_path), 200),
          "a post-death tick of the same verified pawn preserves event-time enrichment");
    constexpr char dead_leave_steam[] = "76561198000000008";
    const std::string dead_leave_path = std::string("/players/") + dead_leave_steam + "/location";
    server.record_login(dead_leave_steam, "DeadLeave");
    server.record_death_position(dead_leave_steam, 16, 17, 18, 33, 43);
    server.record_player_death(dead_leave_steam, "DeadLeave", "");
    server.record_logout(dead_leave_steam);
    const auto dead_leave = request(port, dead_leave_path);
    check(status(dead_leave, 200) && dead_leave.find("\"x\":16.000000") != std::string::npos &&
          dead_leave.find("\"source\":\"death\"") != std::string::npos,
          "Logout after pawn removal uses only the active same-life death position, labelled as death");
    server.record_position(death_steam, 10, 11, 12, 32, 42);
    check(request(port, death_path).find("\"x\":10.000000") != std::string::npos,
          "fresh respawn position supersedes the historical death position");
    server.clear_position(death_steam);
    check(status(request(port, death_path), 503),
          "new verified pawn invalidates death fallback after its live sample expires");
    server.record_player_death(death_steam, "Fallen", "");
    check(status(request(port, death_path), 503),
          "a later death without a new actual capture cannot reuse older death coordinates");
    constexpr char unknown_steam[] = "76561198000000006";
    const std::string unknown_path = std::string("/players/") + unknown_steam + "/location";
    server.record_login(unknown_steam, "Unverified");
    server.record_death_position(unknown_steam, 1, 2, 3, 50, 60);
    server.record_player_death(unknown_steam, "Unverified", "");
    server.record_position(unknown_steam, 1, 2, 3);
    server.clear_position(unknown_steam);
    check(status(request(port, unknown_path), 503),
          "an unverified later pawn key fails closed rather than serving historical death coordinates");
    server.record_login(location_steam, "Survivor");
    check(status(request(port, location_path), 503), "reconnect invalidates prior departure snapshot");
    server.record_logout(location_steam);
    check(status(request(port, location_path), 503),
          "a later departure without a live position cannot reuse an older departure");

    constexpr char evict_steam[] = "76561198000000004";
    const std::string evict_path = std::string("/players/") + evict_steam + "/location";
    server.record_login(evict_steam, "Evicted");
    server.record_departure_position(evict_steam, 20, 21, 22);
    server.record_logout(evict_steam);
    check(status(request(port, evict_path), 200), "new departure is retained before ring rollover");
    for (int i = 0; i < 4096; ++i) server.record_log("Saving world...");
    check(status(request(port, evict_path), 503),
          "departure position expires when its source event leaves the bounded ring");

    const std::string inventory_path = std::string("/players/") + location_steam + "/inventory";
    check(status(request(port, inventory_path), 503), "inventory unavailable without a player");
    server.record_login(location_steam, "Survivor");
    check(status(request(port, inventory_path), 503), "new player has no invented inventory");
    server.record_inventory(location_steam, {{"/Game/PrimalEarth/Core/PrimalItemResource_Stone", "Stone", 7}});
    const auto inventory = request(port, inventory_path);
    check(status(inventory, 200) && inventory.find("\"name\":\"Stone\",\"amount\":7") != std::string::npos,
          "fresh actual inventory snapshot serializes code, name and quantity");
    check(status(request(port, inventory_path, "wrong"), 401), "inventory requires bearer authorization");
    server.clear_inventory(location_steam);
    check(status(request(port, inventory_path), 503), "failed native snapshot clears inventory, never returns fake []");
    server.record_inventory(location_steam, {});
    check(status(request(port, inventory_path), 200) && request(port, inventory_path).find("\r\n\r\n[]") != std::string::npos,
          "verified empty native inventory may return []");
    server.record_logout(location_steam);
    check(status(request(port, inventory_path), 503), "logout invalidates inventory");

    const auto parse_give = [](const std::string& body) {
      gate::Action action;
      action.kind = gate::Action::Kind::give_item;
      return gate::parse_native_action(body, action);
    };
    const std::string valid_give =
        "{\"code\":\"/Game/PrimalEarth/Items/PrimalItemResource_Stone.PrimalItemResource_Stone_C\","
        "\"amount\":3,\"quality\":0,\"blueprint\":false}";
    check(parse_give(valid_give), "valid exact class action accepted");
    check(!parse_give("{\"code\":\"/Game/X.X_C\",\"code\":\"/Game/Y.Y_C\",\"amount\":1,\"quality\":0,\"blueprint\":false}"),
          "duplicate JSON field rejected");
    check(!parse_give("{\"code\":\"/Game/X.X_C\",\"amount\":1,\"quality\":0,\"blueprint\":false,\"extra\":1}"),
          "unknown JSON field rejected");
    check(!parse_give("{\"code\":\"/Game/X.X_C\\u0000\",\"amount\":1,\"quality\":0,\"blueprint\":false}"),
          "escaped Unicode rejected in class path");
    check(!parse_give("{\"code\":\"/Game/X.X_C\",\"amount\":999999999999999999999999,\"quality\":0,\"blueprint\":false}"),
          "amount overflow rejected");
    check(!parse_give("{\"code\":\"/Game/X.X_C\",\"amount\":1,\"quality\":1e999,\"blueprint\":false}"),
          "quality overflow rejected");
    check(!parse_give("{\"code\":\"/Game/X.X_C\",\"amount\":1,\"quality\":0,\"blueprint\":false"),
          "truncated JSON rejected");
    check(!parse_give("{\"code\":{},\"amount\":1,\"quality\":0,\"blueprint\":false}"),
          "nested JSON rejected");
    gate::Action teleport;
    teleport.kind = gate::Action::Kind::teleport;
    check(gate::parse_native_action("{\"x\":-12.5,\"y\":42,\"z\":0}", teleport) &&
          teleport.x == -12.5f && teleport.y == 42.0f && teleport.z == 0.0f,
          "finite teleport destination accepted");
    check(!gate::parse_native_action("{\"x\":1e999,\"y\":0,\"z\":0}", teleport),
          "nonfinite teleport destination rejected");

    const std::string give_path = std::string("/players/") + location_steam + "/give-item";
    check(status(request(port, give_path, token, valid_give, "POST"), 503),
          "absent player cannot receive an action");
    server.record_login(location_steam, "Survivor");
    auto delivered = std::async(std::launch::async, [&] {
      return request(port, give_path, token, valid_give, "POST");
    });
    auto give_action = wait_action(server);
    check(give_action->kind == gate::Action::Kind::give_item &&
          give_action->player_id == location_steam && give_action->amount == 3 &&
          give_action->quality == 0 && !give_action->blueprint,
          "authenticated give item queued with validated fields");
    gate::Server::complete(give_action, true);
    check(status(delivered.get(), 200), "game-thread item effect acknowledgement returned");
    const std::string player_prefix = std::string("/players/") + location_steam;
    check(status(request(port, player_prefix + "/kick", "wrong", {}, "POST"), 401),
          "moderation requires bearer authorization");
    check(status(request(port, player_prefix + "/ban", token, "unexpected", "POST"), 400),
          "moderation rejects unexpected body");
    check(status(request(port, "/players/123/kick", token, {}, "POST"), 400),
          "moderation rejects malformed identity");
    auto kick_request = std::async(std::launch::async, [&] {
      return request(port, player_prefix + "/kick", token, {}, "POST");
    });
    auto kick_action = wait_action(server);
    check(kick_action->kind == gate::Action::Kind::kick &&
          kick_action->player_id == location_steam,
          "kick queues exact authenticated player identity");
    gate::Server::complete(kick_action, false);
    check(status(kick_request.get(), 503), "unverified kick cannot report success");
    auto ban_request = std::async(std::launch::async, [&] {
      return request(port, player_prefix + "/ban", token, {}, "POST");
    });
    auto ban_action = wait_action(server);
    check(ban_action->kind == gate::Action::Kind::ban, "ban queues exact native action");
    gate::Server::complete(ban_action, true);
    check(status(ban_request.get(), 200), "verified in-memory ban acknowledgement returned");
    auto unban_request = std::async(std::launch::async, [&] {
      return request(port, player_prefix + "/unban", token, {}, "POST");
    });
    auto unban_action = wait_action(server);
    check(unban_action->kind == gate::Action::Kind::unban, "unban queues exact native action");
    gate::Server::complete(unban_action, true);
    check(status(unban_request.get(), 200), "verified in-memory unban acknowledgement returned");
    check(status(request(port, "/console", token, "", "POST"), 400),
          "empty console command is rejected before dispatch");
    check(status(request(port, "/console", token, "ListPlayers\nExit", "POST"), 400),
          "multi-line console command is rejected before dispatch");
    for (const std::string command : {"Exit", " quit now", "DoExit", "admincheat doexit",
                                      "GetAll Foo; CHEAT EXIT"}) {
      const auto rejection = request(port, "/console", token, command, "POST");
      check(status(rejection, 400) &&
            rejection.find("Use the dedicated shutdown action") != std::string::npos,
            "engine and cheat shutdown aliases require dedicated shutdown action");
    }
    check(status(request(port, "/console", token, "\xc2\xa0" "Exit", "POST"), 400),
          "non-ASCII command whitespace is rejected before dispatch");
    auto console_request = std::async(std::launch::async, [&] {
      return request(port, "/console", token, "ServerChat ARK CONSOLE TEST", "POST");
    });
    auto console_action = wait_action(server);
    check(console_action->kind == gate::Action::Kind::console &&
          console_action->command == "ServerChat ARK CONSOLE TEST",
          "bounded native console command is queued unchanged");
    console_action->output = "native handled";
    gate::Server::complete(console_action, true);
    const auto console_result = console_request.get();
    check(status(console_result, 200) &&
          console_result.find("\"rawResult\":\"native handled\"") != std::string::npos,
          "console handled output is returned only after game-thread completion");
    auto unhandled = std::async(std::launch::async, [&] {
      return request(port, "/console", token, "UnknownNativeCommand", "POST");
    });
    auto unhandled_action = wait_action(server);
    check(unhandled_action->command == "UnknownNativeCommand", "unknown command reaches native dispatcher");
    gate::Server::complete(unhandled_action, false);
    check(status(unhandled.get(), 503), "native unhandled command cannot report success");
    server.record_logout(location_steam);

    auto accepted = std::async(std::launch::async, [&] { return request(port, "/message", token, "hello ARK", "POST"); });
    auto action = wait_action(server);
    check(action->text == U"hello ARK", "action retains raw UTF-8 text as UTF-32");
    {
      std::lock_guard<std::mutex> lock(action->mutex);
      check(!action->cancelled, "dispatch action is live");
      action->executing = true;
    }
    gate::Server::complete(action, true);
    check(status(accepted.get(), 200), "successful game-thread dispatch acknowledged");

    const std::string targeted_path = std::string("/players/") + location_steam + "/message";
    check(status(request(port, "/players/not-steam/message", token, "private", "POST"), 400),
          "targeted message rejects malformed Steam identity");
    check(status(request(port, "/players/76561198000000002/message", token, "private", "POST"), 503),
          "targeted message does not fall back to broadcast for absent recipient");
    server.record_login(location_steam, "Survivor");
    auto targeted = std::async(std::launch::async, [&] {
      return request(port, targeted_path, token, "private ARK", "POST");
    });
    auto targeted_action = wait_action(server);
    check(targeted_action->kind == gate::Action::Kind::message &&
          targeted_action->player_id == location_steam && targeted_action->text == U"private ARK",
          "targeted message queues exact recipient and text");
    gate::Server::complete(targeted_action, true);
    check(status(targeted.get(), 200), "targeted message acknowledges completed game-thread dispatch");

    auto timed_out = std::async(std::launch::async, [&] { return request(port, "/message", token, "will cancel", "POST"); });
    const auto timeout_response = timed_out.get(); // intentionally leave action queued past the 3-second deadline
    check(status(timeout_response, 503) && timeout_response.find("game thread timeout") != std::string::npos,
          "unclaimed action returns timeout, never false success");
    auto cancelled = wait_action(server);
    {
      std::lock_guard<std::mutex> lock(cancelled->mutex);
      check(cancelled->cancelled && !cancelled->executing, "timed-out action is cancelled before dispatch");
    }
    gate::Server::complete(cancelled, true);
    check(!cancelled->done, "cancelled action cannot be acknowledged later");

    server.record_login("76561198000000002", "Hunter");
    const auto kill_since = server.latest_sequence();
    server.record_entity_killed("76561198000000002", "Dodo", "Dodo");
    const auto kill_event = request(port, std::string("/events?since=") + std::to_string(kill_since));
    check(status(kill_event, 200) && kill_event.find("\"type\":\"entity-killed\"") != std::string::npos &&
          kill_event.find("\"code\":\"Dodo\"") != std::string::npos &&
          kill_event.find("\"gameId\":\"76561198000000002\"") != std::string::npos,
          "native entity-killed event carries verified killer and catalog-backed entity");

    const auto log_since = server.latest_sequence();
    server.record_log("Saving world...");
    const auto log_event = request(port, std::string("/events?since=") + std::to_string(log_since));
    check(status(log_event, 200) && log_event.find("\"type\":\"log\"") != std::string::npos &&
          log_event.find("\"msg\":\"Saving world...\"") != std::string::npos &&
          log_event.find("\"player\"") == std::string::npos,
          "native safe log event has no invented player");
    check(status(request(port, "/shutdown", token, "unexpected", "POST"), 400),
          "shutdown rejects unverified arguments");
    auto stop_request = std::async(std::launch::async, [&] {
      return request(port, "/shutdown", token, {}, "POST");
    });
    auto stop_action = wait_action(server);
    check(stop_action->kind == gate::Action::Kind::shutdown &&
          !stop_action->response_sent.load(), "shutdown does not mark response sent before engine staging");
    gate::Server::complete(stop_action, true);
    check(status(stop_request.get(), 200) && stop_action->response_sent.load(),
          "shutdown acknowledges staged action only after response bytes were sent");
    server.stop();
    std::cout << "gate_http transport tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "gate_http transport test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
