#include "../src/action_bindings.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

using namespace ark_actions;

namespace {
int64_t inventory_quantity = 2;
int grant_increment = 3;
bool grant_result = true;
bool teleport_result = true;
// The engine lifts a standing survivor placed below this height up to it.
float teleport_floor = -std::numeric_limits<float>::infinity();
FVector position{};
FRotator orientation{12.0f, 34.0f, 56.0f};
int grant_calls = 0;
int teleport_calls = 0;

bool grant(void*, const FString* path, int32_t amount, bool blueprint, bool extra, float quality, float second) {
  ++grant_calls;
  assert(path && path->count > 1 && path->data[path->count - 1] == 0);
  assert(amount == 3 && blueprint && !extra && quality == 2.5f && second == 0);
  if (grant_result) inventory_quantity += grant_increment;
  return grant_result;
}
bool teleport(void*, bool flag, FVector target, FRotator rotation) {
  ++teleport_calls;
  assert(!flag && rotation.pitch == orientation.pitch && rotation.yaw == orientation.yaw &&
         rotation.roll == orientation.roll);
  if (teleport_result) position = FVector{target.x, target.y, std::max(target.z, teleport_floor)};
  return teleport_result;
}
FVector location(void*) { return position; }
FRotator rotation(void*) { return orientation; }
}

int main() {
  alignas(8) unsigned char controller[0x500]{};
  alignas(8) unsigned char pawn[16]{};
  *reinterpret_cast<void**>(controller + 0x490) = pawn;
  Api api{grant, teleport, location, syscall(SYS_gettid)};
  api.rotation = rotation;
  const char* code = "/Game/Items/PrimalItem_Stone.PrimalItem_Stone_C";
  const QuantityFn count = [](void*, std::string_view) { return std::optional<int64_t>(inventory_quantity); };

  assert(give_item(controller, pawn, code, 3, 2.5f, true, count, api) == Result::verified);
  assert(grant_calls == 1);
  grant_result = false;
  assert(give_item(controller, pawn, code, 3, 2.5f, true, count, api) == Result::rejected);
  grant_result = true;
  grant_increment = 4;
  assert(give_item(controller, pawn, code, 3, 2.5f, true, count, api) == Result::unverified);
  grant_increment = 3;
  assert(give_item(controller, pawn, code, 3, 2.5f, true, {}, api) == Result::unverified);
  assert(give_item(controller, pawn, "Stone", 3, 2.5f, true, count, api) == Result::invalid);
  assert(give_item(controller, pawn, code, 0, 2.5f, true, count, api) == Result::invalid);
  assert(give_item(controller, pawn, code, 3, std::numeric_limits<float>::quiet_NaN(), true, count, api) == Result::invalid);
  Api wrong_thread = api; wrong_thread.game_thread_tid = -1;
  assert(give_item(controller, pawn, code, 3, 2.5f, true, count, wrong_thread) == Result::invalid);

  const FVector destination{100, -50, 240};
  assert(teleport_player(controller, pawn, destination, api) == Result::verified);
  assert(teleport_calls == 1);
  teleport_result = false;
  assert(teleport_player(controller, pawn, destination, api) == Result::rejected);
  teleport_result = true;
  // A point saved while prone: the engine lifts the survivor 40 units to standing height.
  teleport_floor = destination.z + 40;
  assert(teleport_player(controller, pawn, destination, api) == Result::verified);
  assert(position.z == destination.z + 40);
  // A lift beyond 100, or a fall below the point, is not where Takaro asked.
  teleport_floor = destination.z + 101;
  assert(teleport_player(controller, pawn, destination, api) == Result::unverified);
  teleport_floor = -std::numeric_limits<float>::infinity();
  Api sank = api; sank.location = [](void*) { return FVector{100, -50, 214}; };
  assert(teleport_player(controller, pawn, destination, sank) == Result::unverified);
  Api moved = api; moved.location = [](void*) { return FVector{130, -50, 240}; };
  assert(teleport_player(controller, pawn, destination, moved) == Result::unverified);
  teleport_floor = -std::numeric_limits<float>::infinity();
  position = FVector{0, 0, 0};
  Api no_readback = api; no_readback.location = [](void*) { return FVector{0, 0, 0}; };
  assert(teleport_player(controller, pawn, destination, no_readback) == Result::unverified);
  assert(teleport_player(controller, pawn, FVector{std::numeric_limits<float>::infinity(), 0, 0}, api) == Result::invalid);
  assert(teleport_player(controller, pawn, destination, wrong_thread) == Result::invalid);
  orientation.yaw = std::numeric_limits<float>::quiet_NaN();
  assert(teleport_player(controller, pawn, destination, api) == Result::invalid);
  orientation.yaw = 34.0f;
  Api no_rotation = api; no_rotation.rotation = nullptr;
  assert(teleport_player(controller, pawn, destination, no_rotation) == Result::invalid);
  *reinterpret_cast<void**>(controller + 0x490) = nullptr;
  assert(teleport_player(controller, pawn, destination, api) == Result::invalid);

  // Amounts above one stack arrive in rounds (one per tick); each round is read back.
  {
    auto st = give_step(250, 10, 0, true, std::optional<int64_t>(110));
    assert(st.state == GiveState::more && st.delivered == 100);
    st = give_step(250, 10, 100, true, std::optional<int64_t>(210));
    assert(st.state == GiveState::more && st.delivered == 200);
    st = give_step(250, 10, 200, true, std::optional<int64_t>(260));
    assert(st.state == GiveState::done && st.delivered == 250);
    assert(give_step(5, 0, 0, true, std::optional<int64_t>(5)).state == GiveState::done);
    // No progress, refusal, failed read-back, over-delivery and bad input fail closed.
    st = give_step(250, 10, 100, true, std::optional<int64_t>(110));
    assert(st.state == GiveState::failed && st.delivered == 100);
    // A partial grant reports false but the read-back shows progress: continue.
    st = give_step(250, 0, 0, false, std::optional<int64_t>(100));
    assert(st.state == GiveState::more && st.delivered == 100);
    assert(give_step(5, 0, 0, false, std::optional<int64_t>(0)).state == GiveState::failed);
    assert(give_step(5, 0, 0, std::nullopt, std::optional<int64_t>(5)).state == GiveState::failed);
    assert(give_step(5, 0, 0, true, std::nullopt).state == GiveState::failed);
    assert(give_step(5, 0, 0, true, std::optional<int64_t>(6)).state == GiveState::failed);
    assert(give_step(5, 10, 0, true, std::optional<int64_t>(9)).state == GiveState::failed);
    assert(give_step(0, 0, 0, true, std::optional<int64_t>(0)).state == GiveState::failed);
    assert(give_step(10001, 0, 0, true, std::optional<int64_t>(10001)).state == GiveState::failed);
  }
}
