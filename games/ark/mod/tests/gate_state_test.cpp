// State contract between the game thread and the Takaro bridge (successor of
// gate_http_test.cpp: the same snapshot, enrichment and action semantics,
// without the removed loopback HTTP transport).
#include "../src/gate_state.hpp"

#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool condition, const char* why) {
  if (!condition) throw std::runtime_error(why);
}

std::shared_ptr<gate::Action> wait_action(gate::Server& server) {
  for (int i = 0; i < 400; ++i) {
    if (auto action = server.take_action()) return action;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("action was not queued");
}

// What the game thread does before executing a taken action.
bool claim(const std::shared_ptr<gate::Action>& action) {
  std::lock_guard<std::mutex> lock(action->mutex);
  if (action->cancelled) return false;
  action->executing = true;
  return true;
}

std::shared_ptr<gate::Action> make(gate::Action::Kind kind, const std::string& id = {}) {
  auto action = std::make_shared<gate::Action>();
  action->kind = kind;
  action->player_id = id;
  return action;
}

constexpr char steam[] = "76561198000000000";
}  // namespace

int main() {
  try {
    gate::Server server("test-boot");
    check(server.health().boot_id == "test-boot" && !server.health().ready, "boot id and not-ready health");
    check(server.submit(make(gate::Action::Kind::message)).status == gate::Server::Submit::not_ready,
          "actions are refused before the game thread is ready");
    server.set_game_ready();

    // Broadcast reaches the queue with zero, one and two recipients.
    for (int players = 0; players < 3; ++players) {
      if (players == 1) server.record_login(steam, "One");
      if (players == 2) server.record_login("76561198000000009", "Two");
      auto action = make(gate::Action::Kind::message);
      check(gate::utf8_to_utf32("hello", action->text), "utf8 text");
      auto pending = std::async(std::launch::async, [&] { return server.submit(action); });
      auto taken = wait_action(server);
      check(taken == action && claim(taken), "broadcast queued and claimable");
      gate::Server::complete(taken, true);
      const auto outcome = pending.get();
      check(outcome.status == gate::Server::Submit::done && outcome.success, "broadcast acknowledged");
    }
    check(server.submit(make(gate::Action::Kind::message, "76561198000000002")).status ==
              gate::Server::Submit::unavailable,
          "targeted message to an absent player never falls back to broadcast");

    check(!server.items() && !server.entities() && !server.locations() && !server.bans(),
          "catalogs, locations and bans stay unavailable until sampled");
    server.record_bans({steam});
    check(server.bans() && server.bans()->size() == 1, "published native bans are readable");
    check(server.is_banned(steam) && !server.is_banned("76561198000000099"), "ban membership lookup");
    server.clear_bans();
    check(!server.bans(), "failed ban snapshot cannot return stale data");
    server.record_bans({});
    check(server.bans() && server.bans()->empty(), "validated empty ban collection");
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    check(!server.bans(), "stalled game-thread ban snapshot cannot remain readable indefinitely");
    server.record_locations({{"/Game/TheIsland/StartA", "South Zone 1", 12.5f, -4.0f, 300.0f, 100.0f, 90.0f, 180.0f}});
    check(server.locations() && server.locations()->at(0).size_x == 100.0f, "locations carry geometry");
    server.record_catalog({{"/Game/Stone.Stone_C", "Stone"}, {"/Game/Wood.Wood_C", "Wood"}});
    server.record_entities({{"Dodo", "Dodo"}, {"Raptor", "Raptor"}});
    check(server.items()->size() == 2 && server.entities()->size() == 2, "catalog snapshots are complete");
    server.clear_catalog();
    check(!server.items(), "failed catalog refresh never serves stale data");

    // Event ring paging and truncation.
    const uint64_t base = server.events_since(0, 0).latest;
    for (int i = 0; i < 140; ++i) server.record_chat(steam, "Survivor", "line " + std::to_string(i));
    auto page = server.events_since(base, 128);
    check(page.events.size() == 128 && page.events.front().seq == base + 1 && !page.truncated,
          "first page holds 128 ordered events");
    page = server.events_since(base + 128, 128);
    check(page.events.size() == 12 && page.latest == base + 140, "second page completes without skipping");
    for (int i = 140; i < 4200; ++i) server.record_chat(steam, "Survivor", "line " + std::to_string(i));
    page = server.events_since(0, 1);
    check(page.truncated && page.events.front().seq > 1, "ring truncation is reported");

    constexpr char location_steam[] = "76561198000000001";
    check(!server.location(location_steam), "location unavailable without a player");
    server.record_position(location_steam, 1, 2, 3);
    check(!server.location(location_steam), "position cannot be recorded for an absent player");
    server.record_login(location_steam, "Survivor");
    server.record_player_death(location_steam, "Survivor", "76561198000000002");
    page = server.events_since(server.events_since(0, 0).latest - 1, 1);
    check(page.events.size() == 1 && page.events[0].type == "player-death" &&
          page.events[0].attacker_id == "76561198000000002", "death event keeps the distinct attacker");
    check(!server.location(location_steam), "new player has no invented position");
    server.record_position(location_steam, 12.5f, -4.25f, 300.0f);
    auto location = server.location(location_steam);
    check(location && location->x == 12.5f && location->y == -4.25f && !location->source, "fresh live position");
    server.record_position(location_steam, std::numeric_limits<float>::quiet_NaN(), 0, 0);
    check(server.location(location_steam)->x == 12.5f, "non-finite coordinates do not replace the last finite one");
    server.clear_position(location_steam);
    check(!server.location(location_steam), "cleared position is unavailable");
    server.record_position(location_steam, 1, 2, 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    check(!server.location(location_steam), "stale position is unavailable");
    server.record_position(location_steam, 1, 2, 3);
    server.record_logout(location_steam);
    location = server.location(location_steam);
    check(location && location->source && std::string(location->source) == "departure",
          "logout keeps the recent actual position for event enrichment");
    check(!server.inventory(location_steam) && !server.player(location_steam),
          "logout invalidates inventory and online presence");

    constexpr char death_steam[] = "76561198000000003";
    server.record_login(death_steam, "Fallen");
    server.record_death_position(death_steam, 7, 8, 9, 31, 41);
    server.record_player_death(death_steam, "Fallen", "");
    constexpr char stale_steam[] = "76561198000000005";
    server.record_login(stale_steam, "Stale");
    server.record_departure_position(stale_steam, 90, 91, 92);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    server.record_logout(stale_steam);
    check(!server.location(stale_steam), "an old pre-Logout capture cannot become a fresh departure");
    location = server.location(death_steam);
    check(location && std::string(location->source) == "death", "death enrichment while its event is replayable");
    server.record_position(death_steam, 7, 8, 9, 31, 41);
    server.clear_position(death_steam);
    check(server.location(death_steam).has_value(), "a post-death tick of the same pawn keeps enrichment");
    server.record_position(death_steam, 10, 11, 12, 32, 42);
    check(server.location(death_steam)->x == 10.0f, "fresh respawn position supersedes the death position");
    server.clear_position(death_steam);
    check(!server.location(death_steam), "new pawn invalidates the death fallback");

    constexpr char dead_leave[] = "76561198000000008";
    server.record_login(dead_leave, "DeadLeave");
    server.record_death_position(dead_leave, 16, 17, 18, 33, 43);
    server.record_player_death(dead_leave, "DeadLeave", "");
    server.record_logout(dead_leave);
    location = server.location(dead_leave);
    check(location && location->x == 16.0f && std::string(location->source) == "death",
          "logout after pawn removal uses the same-life death position");

    constexpr char evict_steam[] = "76561198000000004";
    server.record_login(evict_steam, "Evicted");
    server.record_departure_position(evict_steam, 20, 21, 22);
    server.record_logout(evict_steam);
    check(server.location(evict_steam).has_value(), "departure retained before ring rollover");
    for (int i = 0; i < 4096; ++i) server.record_log("Saving world...");
    check(!server.location(evict_steam), "departure expires with its event");

    server.record_login(location_steam, "Survivor");
    check(!server.inventory(location_steam), "new player has no invented inventory");
    server.record_inventory(location_steam, {{"/Game/Stone", "Stone", 7}});
    check(server.inventory(location_steam)->at(0).amount == 7, "fresh inventory snapshot");
    server.clear_inventory(location_steam);
    check(!server.inventory(location_steam), "failed snapshot clears inventory, never a fake []");
    server.record_inventory(location_steam, {});
    check(server.inventory(location_steam) && server.inventory(location_steam)->empty(), "verified empty inventory");

    // Unclaimed action: cancelled at the start deadline, never acknowledged later.
    auto unclaimed = make(gate::Action::Kind::message);
    const auto timed_out = server.submit(unclaimed, std::chrono::milliseconds(200), std::chrono::milliseconds(200));
    check(timed_out.status == gate::Server::Submit::timeout, "unclaimed action times out");
    auto cancelled = wait_action(server);
    check(!claim(cancelled), "timed-out action is cancelled before dispatch");
    gate::Server::complete(cancelled, true);
    check(!cancelled->done, "cancelled action cannot be acknowledged later");

    // Claimed but stalled (the 2026-10-06 host freeze): the waiter gives up,
    // the game thread completes much later; the shared action stays valid.
    auto stalled = make(gate::Action::Kind::kick, location_steam);
    auto waiter = std::async(std::launch::async, [&] {
      return server.submit(stalled, std::chrono::milliseconds(300), std::chrono::milliseconds(200));
    });
    auto claimed = wait_action(server);
    check(claim(claimed), "game thread claims the kick");
    const auto gave_up = waiter.get();
    check(gave_up.status == gate::Server::Submit::timeout && !gave_up.success,
          "a stalled executing action never reports success");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gate::Server::complete(claimed, true);  // late completion after the waiter left
    check(claimed->done && claimed->success, "late completion is safe and recorded");

    server.record_login("76561198000000002", "Hunter");
    const auto kill_since = server.events_since(0, 0).latest;
    server.record_entity_killed("76561198000000002", "Dodo", "Dodo");
    page = server.events_since(kill_since, 8);
    check(page.events.size() == 1 && page.events[0].entity_code == "Dodo", "entity-killed carries the catalog code");
    const auto log_since = server.events_since(0, 0).latest;
    server.record_log("Saving world...");
    page = server.events_since(log_since, 8);
    check(page.events.size() == 1 && page.events[0].type == "log" && page.events[0].id.empty(),
          "log event has no invented player");

    {
      gate::Server survivors("survivor-boot");
      const std::string steam = "76561198000000011";
      survivors.record_login(steam, "Limon", "192.0.2.1", 40, {"92463960", "Human"});
      survivors.record_progression(steam, {20, 1900.0, 0.5});
      check(!survivors.record_character(steam, {"92463960", "Human"}), "the same survivor is no switch");
      survivors.record_chat(steam, "Limon", "hello");
      survivors.record_death_position(steam, 1, 2, 3);
      survivors.record_player_death(steam, "Limon", "");
      auto rows = survivors.events_since(0, 16);
      check(rows.events.size() == 3 && rows.events[0].character.id == "92463960" &&
                rows.events[1].character.id == "92463960" && rows.events[2].character.name == "Human",
            "every player event names the survivor");

      const auto since = rows.latest;
      check(survivors.record_character(steam, {"843947694", "Human"}), "a new survivor is a switch");
      rows = survivors.events_since(since, 16);
      check(rows.events.size() == 2 && rows.events[0].type == "player-disconnected" &&
                rows.events[0].character.id == "92463960" && rows.events[1].type == "player-connected" &&
                rows.events[1].character.id == "843947694" && rows.events[1].ip == "192.0.2.1",
            "a switch leaves as the old survivor and joins as the new one");
      check(survivors.player(steam).has_value(), "a switch keeps the player online");
      check(!survivors.progression(steam), "a new survivor does not inherit the old level");

      survivors.record_logout(steam);
      check(survivors.character(steam).id == "843947694", "the survivor stays known offline");
      const auto last = survivors.events_since(0, 16);
      check(last.events.back().type == "player-disconnected" && last.events.back().character.id == "843947694",
            "a logout names the current survivor");
      check(!last.events.back().position, "a new survivor does not leave at the old one's death position");
      check(!survivors.record_character(steam, {"1000000", "Other"}), "an offline player cannot switch");

      // No survivor at login: the first sighting must still reach Takaro as a connect.
      const std::string fresh = "76561198000000012";
      survivors.record_login(fresh, "New");
      check(survivors.character(fresh).id.empty(), "no survivor is invented");
      check(survivors.record_character(fresh, {"60000", "Human"}), "the first survivor is reported");
      check(!survivors.record_character(fresh, {"", ""}), "an unverified read changes nothing");
      check(survivors.character(fresh).id == "60000", "an unverified read keeps the survivor");
    }

    std::cout << "gate_state tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "gate_state test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
