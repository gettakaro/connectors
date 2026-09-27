#include "../src/action_bindings.hpp"
#include <cassert>
#include <cstring>
#include <limits>

using namespace ark_actions;

namespace {
int64_t inventory_quantity = 2;
int grant_increment = 3;
bool grant_result = true;
bool teleport_result = true;
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
  if (teleport_result) position = target;
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
}
