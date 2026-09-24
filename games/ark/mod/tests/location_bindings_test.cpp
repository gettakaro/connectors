#include "../src/location_bindings.hpp"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
template <size_t N> struct Block { alignas(16) std::array<unsigned char, N> bytes{}; };
template <class T> void put(void* base, size_t at, T value) {
  std::memcpy(static_cast<unsigned char*>(base) + at, &value, sizeof(value));
}
void* base_class{};
void* actor1{};
void* actor2{};
int actor_count = 2;
bool duplicate_path = false;
ark_location::FVector positions[2]{{100, 200, 300}, {-50, 2, 10}};
ark_location::FVector extents[2]{{50, 50, 100}, {20, 30, 40}};
int frees = 0;
void* player_start_class() { return base_class; }
void get_all_actors(void*, void*, ark_location::ActorArray* out) {
  auto** rows = static_cast<void**>(std::malloc(2 * sizeof(void*)));
  assert(rows);
  rows[0] = actor1;
  rows[1] = actor2;
  *out = {rows, actor_count, 2};
}
void get_actor_bounds(void* actor, ark_location::FVector* origin,
                      ark_location::FVector* half_extent) {
  const int index = actor == actor1 ? 0 : 1;
  *origin = positions[index];
  *half_extent = extents[index];
}
void free_game(const void* pointer) {
  ++frees;
  std::free(const_cast<void*>(pointer));
}
ark_inventory::FString* copy(ark_inventory::FString* out, const wchar_t* text) {
  const size_t count = std::wcslen(text) + 1;
  auto* chars = static_cast<wchar_t*>(std::malloc(count * sizeof(wchar_t)));
  assert(chars);
  std::memcpy(chars, text, count * sizeof(wchar_t));
  *out = {chars, static_cast<int32_t>(count), static_cast<int32_t>(count)};
  return out;
}
ark_inventory::FString* get_path(ark_inventory::FString* out, void* actor) {
  return copy(out, actor == actor1 || duplicate_path
      ? L"/Game/Maps/TheIsland.TheIsland:PersistentLevel.PlayerStart_1"
      : L"/Game/Maps/TheIsland.TheIsland:PersistentLevel.PlayerStart_2");
}
ark_inventory::FString* name_to_string(ark_inventory::FString* out,
                                      const ark_entity::FName* tag) {
  return copy(out, tag->index == 1 ? L"South Zone 1" : L"None");
}
}

int main() {
  Block<0x100> base, derived;
  Block<0x500> first, second;
  base_class = base.bytes.data();
  actor1 = first.bytes.data();
  actor2 = second.bytes.data();
  put<void*>(derived.bytes.data(), 0x30, base.bytes.data());
  put<int32_t>(derived.bytes.data(), 0x40, 0x500);
  for (void* actor : {actor1, actor2}) put<void*>(actor, 0x10, derived.bytes.data());
  put<ark_entity::FName>(actor1, 0x494, {1, 0});
  put<ark_entity::FName>(actor2, 0x494, {2, 0});
  put<int32_t>(actor1, 0x49C, 3);
  put<int32_t>(actor2, 0x49C, 4);
  alignas(8) unsigned char world[16]{};
  ark_location::Api api{};
  api.player_start_class = player_start_class;
  api.get_all_actors = get_all_actors;
  api.get_actor_bounds = get_actor_bounds;
  api.get_object_path = get_path;
  api.name_to_string = name_to_string;
  api.game_free = free_game;
  api.game_thread_tid = syscall(SYS_gettid);
  api.verify_exact_entries = false;
  std::vector<ark_location::Location> locations;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::ok);
  assert(locations.size() == 2);
  assert(locations[0].name == "South Zone 1");
  assert(locations[0].spawn_region == 3 && locations[0].size.x == 100.0f &&
         locations[0].size.y == 100.0f && locations[0].size.z == 200.0f);
  assert(locations[1].name == locations[1].code);
  assert(locations[1].position.x == -50);
  assert(frees == 5); // Two paths, two tag conversions, one actor array.
  duplicate_path = true;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::duplicate_code);
  assert(locations.empty());
  duplicate_path = false;
  positions[0].x = std::numeric_limits<float>::quiet_NaN();
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::unverified_actor);
  positions[0].x = 100;
  extents[0].x = 0;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::unverified_actor);
  extents[0].x = 50;
  actor_count = 0;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::not_ready);
  actor_count = 2;
  api.verify_exact_entries = true;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::invalid_layout);
  api.verify_exact_entries = false;
  api.game_thread_tid = -1;
  assert(ark_location::snapshot(world, locations, api) == ark_location::Status::invalid_layout);
}
