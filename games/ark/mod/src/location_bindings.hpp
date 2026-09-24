#pragma once

// Exact Linux ShooterGameServer 21241282 only. This describes the native
// collision bounds of loaded APlayerStart actors, not all ARK map regions.
#include "entity_bindings.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_location {

struct FVector { float x, y, z; };
struct ActorArray { void** data = nullptr; int32_t count = 0; int32_t capacity = 0; };
static_assert(sizeof(ActorArray) == 16);

struct Location {
  std::string code;       // Native UObject path, including its loaded level.
  std::string name;       // Native PlayerStartTag, or the native path if None.
  FVector position;       // Native GetActorBounds center, world units.
  int32_t spawn_region;   // Native APlayerStart::SpawnPointRegion.
  FVector size;           // Twice native component bounds half-extents.
};

enum class Status { ok, not_ready, invalid_layout, unverified_actor, duplicate_code };

struct Api {
  void* (*player_start_class)() = reinterpret_cast<void* (*)()>(0x2E42C80);
  void (*get_all_actors)(void*, void*, ActorArray*) =
      reinterpret_cast<void (*)(void*, void*, ActorArray*)>(0x263B340);
  void (*get_actor_bounds)(void*, FVector*, FVector*) =
      reinterpret_cast<void (*)(void*, FVector*, FVector*)>(0x26E9360);
  ark_inventory::FString* (*get_object_path)(ark_inventory::FString*, void*) =
      reinterpret_cast<ark_inventory::FString* (*)(ark_inventory::FString*, void*)>(0x1293230);
  ark_inventory::FString* (*name_to_string)(ark_inventory::FString*, const ark_entity::FName*) =
      reinterpret_cast<ark_inventory::FString* (*)(ark_inventory::FString*, const ark_entity::FName*)>(0x1C0B900);
  void (*game_free)(const void*) = reinterpret_cast<void (*)(const void*)>(0x1AF8E30);
  long game_thread_tid = 0;
  // Synthetic tests inject functions and disable this check. Native use keeps
  // it enabled after the whole-executable SHA-256 guard has passed.
  bool verify_exact_entries = true;
};

inline bool exact_entries_verified(const Api& api) {
  const auto matches = [](uintptr_t address, const unsigned char* bytes, size_t length) {
    return std::memcmp(reinterpret_cast<const void*>(address), bytes, length) == 0;
  };
  static constexpr unsigned char start[] =
      {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x81, 0xec, 0xa0, 0x02, 0x00, 0x00};
  static constexpr unsigned char all[] =
      {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
  static constexpr unsigned char bounds[] =
      {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x20};
  static constexpr unsigned char path[] =
      {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb};
  static constexpr unsigned char name[] =
      {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb};
  static constexpr unsigned char free[] =
      {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb};
  return reinterpret_cast<uintptr_t>(api.player_start_class) == 0x2E42C80 &&
      reinterpret_cast<uintptr_t>(api.get_all_actors) == 0x263B340 &&
      reinterpret_cast<uintptr_t>(api.get_actor_bounds) == 0x26E9360 &&
      reinterpret_cast<uintptr_t>(api.get_object_path) == 0x1293230 &&
      reinterpret_cast<uintptr_t>(api.name_to_string) == 0x1C0B900 &&
      reinterpret_cast<uintptr_t>(api.game_free) == 0x1AF8E30 &&
      matches(0x2E42C80, start, sizeof(start)) &&
      matches(0x263B340, all, sizeof(all)) &&
      matches(0x26E9360, bounds, sizeof(bounds)) &&
      matches(0x1293230, path, sizeof(path)) &&
      matches(0x1C0B900, name, sizeof(name)) &&
      matches(0x1AF8E30, free, sizeof(free));
}

inline bool finite(const FVector& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
      std::abs(value.x) <= 1'000'000'000.0f && std::abs(value.y) <= 1'000'000'000.0f &&
      std::abs(value.z) <= 1'000'000'000.0f;
}

inline Status snapshot(void* world, std::vector<Location>& out, const Api& api = {}) {
  out.clear();
  if (!ark_entity::plausible(world) || !api.player_start_class || !api.get_all_actors ||
      !api.get_actor_bounds || !api.get_object_path || !api.name_to_string || !api.game_free ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      (api.verify_exact_entries && !exact_entries_verified(api)))
    return Status::invalid_layout;
  void* base = api.player_start_class();
  if (!ark_entity::plausible(base)) return Status::not_ready;
  ActorArray actors{};
  api.get_all_actors(world, base, &actors);
  struct FreeActors {
    const Api& api;
    ActorArray& actors;
    ~FreeActors() { if (actors.data) api.game_free(actors.data); }
  } free_actors{api, actors};
  if (actors.count <= 0) return Status::not_ready;
  if (actors.count > 1024 || actors.capacity < actors.count || actors.capacity > 4096 ||
      !ark_entity::plausible(actors.data)) return Status::invalid_layout;
  std::vector<Location> pending;
  pending.reserve(static_cast<size_t>(actors.count));
  std::unordered_set<std::string> codes;
  for (int32_t i = 0; i < actors.count; ++i) {
    void* actor = actors.data[i];
    if (!ark_entity::plausible(actor)) return Status::unverified_actor;
    void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(actor) + 0x10);
    if (!ark_entity::is_a(klass, base)) return Status::unverified_actor;
    const int32_t object_size =
        *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(klass) + 0x40);
    if (object_size < 0x4A0 || object_size > 65536) return Status::unverified_actor;
    FVector position{}, half_extent{};
    api.get_actor_bounds(actor, &position, &half_extent);
    if (!finite(position) || !finite(half_extent) ||
        half_extent.x <= 0 || half_extent.y <= 0 || half_extent.z <= 0 ||
        half_extent.x > 500000000.0f || half_extent.y > 500000000.0f ||
        half_extent.z > 500000000.0f) return Status::unverified_actor;
    ark_inventory::FString raw_path{};
    api.get_object_path(&raw_path, actor);
    std::string code;
    ark_inventory::Api conversion{};
    conversion.game_free = api.game_free;
    if (!ark_inventory::copy_and_free(raw_path, conversion, code) || code.size() > 512)
      return Status::unverified_actor;
    if (!codes.insert(code).second) return Status::duplicate_code;
    std::string name = code;
    const auto* tag = reinterpret_cast<const ark_entity::FName*>(
        reinterpret_cast<uintptr_t>(actor) + 0x494);
    if (tag->index > 0) {
      ark_inventory::FString raw_tag{};
      api.name_to_string(&raw_tag, tag);
      std::string candidate;
      if (!ark_inventory::copy_and_free(raw_tag, conversion, candidate) ||
          candidate.size() > 128) return Status::unverified_actor;
      if (candidate != "None") name = std::move(candidate);
    }
    const int32_t region = *reinterpret_cast<const int32_t*>(
        reinterpret_cast<uintptr_t>(actor) + 0x49C);
    const FVector size{2 * half_extent.x, 2 * half_extent.y, 2 * half_extent.z};
    pending.push_back({std::move(code), std::move(name), position, region, size});
  }
  out.swap(pending);
  return Status::ok;
}

} // namespace ark_location
