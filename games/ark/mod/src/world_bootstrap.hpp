#pragma once

// Exact ShooterGameServer 21241282 world discovery before a player joins.
// Call only from the verified FEngineLoop tick after its original call. The
// returned raw pointer is valid only for that game-thread turn; retain the
// weak index/serial and re-resolve before any later use.
#include "death_bindings.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_world_bootstrap {

using Key = ark_death::ObjectKey;
using CreateWeak = void (*)(Key*, void*);
using ResolveWeak = void* (*)(const Key*, bool);
using ClassGetter = void* (*)();

struct Api {
  void* const* engine_global = reinterpret_cast<void* const*>(0x59958F8);
  ClassGetter world_class = reinterpret_cast<ClassGetter>(0x2D1E730);
  ClassGetter game_mode_class = reinterpret_cast<ClassGetter>(0x13CCDA0);
  CreateWeak create_weak = reinterpret_cast<CreateWeak>(0x1D56970);
  ResolveWeak resolve_weak = reinterpret_cast<ResolveWeak>(0x1D56C70);
  ark_death::ObjectArray objects{};
  long game_thread_tid = 0;

  uintptr_t expected_engine_global = 0x59958F8;
  uintptr_t engine_reference = 0x81FF53;
  uintptr_t context_lookup = 0x2933F90;
  uintptr_t game_context_creation = 0x263010D;
  uintptr_t expected_engine_reference = 0x81FF53;
  uintptr_t expected_context_lookup = 0x2933F90;
  uintptr_t expected_game_context_creation = 0x263010D;
  uintptr_t expected_world_class = 0x2D1E730;
  uintptr_t expected_mode_class = 0x13CCDA0;
  uintptr_t expected_create_weak = 0x1D56970;
  uintptr_t expected_resolve_weak = 0x1D56C70;
  std::array<uint8_t, 7> engine_reference_bytes{0x48, 0x8B, 0x3D, 0x9E, 0x59, 0x17, 0x05};
  std::array<uint8_t, 10> context_lookup_bytes{0x55, 0x48, 0x89, 0xE5,
                                                0x8B, 0x8F, 0x28, 0x09, 0x00, 0x00};
  std::array<uint8_t, 10> game_context_bytes{0xBE, 0x01, 0x00, 0x00, 0x00,
                                              0xE8, 0xF9, 0xED, 0x2F, 0x00};
  std::array<uint8_t, 14> world_class_bytes{0x55, 0x48, 0x89, 0xE5,
                                              0x48, 0x8B, 0x05, 0x4D, 0x97, 0xC8, 0x02,
                                              0x48, 0x85, 0xC0};
  std::array<uint8_t, 8> mode_class_bytes{0x55, 0x48, 0x89, 0xE5,
                                           0x48, 0x8B, 0x05, 0x25};
  std::array<uint8_t, 10> create_weak_bytes{0x55, 0x48, 0x89, 0xE5,
                                             0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
  std::array<uint8_t, 11> resolve_weak_bytes{0x8B, 0x4F, 0x04, 0x31, 0xC0,
                                              0x85, 0xC9, 0x74, 0x7A, 0x8B, 0x17};
};

enum class Status { ready, not_ready, invalid_layout, ambiguous };
struct Result {
  Status status = Status::not_ready;
  void* world = nullptr;
  Key key{-1, 0};
};

template <size_t N>
inline bool exact_bytes(uintptr_t actual, uintptr_t expected, const std::array<uint8_t, N>& bytes) {
  return actual == expected && expected >= 0x10000 &&
      std::memcmp(reinterpret_cast<const void*>(actual), bytes.data(), N) == 0;
}

inline bool is_a(const void* object, const void* base) {
  if (!ark_death::plausible(object) || !ark_death::plausible(base)) return false;
  auto* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(object) + 0x10);
  for (int depth = 0; depth < 64 && ark_death::plausible(klass); ++depth) {
    if (klass == base) return true;
    klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(klass) + 0x30);
  }
  return false;
}

inline Result discover(const Api& api = {}) {
  if (api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      !api.engine_global || !api.world_class || !api.game_mode_class ||
      !api.create_weak || !api.resolve_weak || !api.objects.chunks || !api.objects.count ||
      reinterpret_cast<uintptr_t>(api.engine_global) != api.expected_engine_global ||
      !exact_bytes(api.engine_reference, api.expected_engine_reference, api.engine_reference_bytes) ||
      !exact_bytes(api.context_lookup, api.expected_context_lookup, api.context_lookup_bytes) ||
      !exact_bytes(api.game_context_creation, api.expected_game_context_creation, api.game_context_bytes) ||
      !exact_bytes(reinterpret_cast<uintptr_t>(api.world_class), api.expected_world_class,
                   api.world_class_bytes) ||
      !exact_bytes(reinterpret_cast<uintptr_t>(api.game_mode_class), api.expected_mode_class,
                   api.mode_class_bytes) ||
      !exact_bytes(reinterpret_cast<uintptr_t>(api.create_weak), api.expected_create_weak,
                   api.create_weak_bytes) ||
      !exact_bytes(reinterpret_cast<uintptr_t>(api.resolve_weak), api.expected_resolve_weak,
                   api.resolve_weak_bytes)) return {Status::invalid_layout};
  int32_t global_displacement = 0;
  std::memcpy(&global_displacement, api.engine_reference_bytes.data() + 3, sizeof(global_displacement));
  if (api.engine_reference + 7 + static_cast<intptr_t>(global_displacement) !=
      reinterpret_cast<uintptr_t>(api.engine_global)) return {Status::invalid_layout};

  void* engine = *api.engine_global;
  if (!ark_death::plausible(engine)) return {Status::not_ready};
  const auto base = reinterpret_cast<uintptr_t>(engine) + 0x920;
  auto* contexts = *reinterpret_cast<void* const* const*>(base);
  const int32_t count = *reinterpret_cast<const int32_t*>(base + 8);
  const int32_t capacity = *reinterpret_cast<const int32_t*>(base + 12);
  if (count < 0 || count > 8 || capacity < count || capacity > 32 ||
      (count && !ark_death::plausible(contexts))) return {Status::invalid_layout};
  if (!count) return {Status::not_ready};
  void* world_class = api.world_class();
  void* mode_class = api.game_mode_class();
  if (!ark_death::plausible(world_class) || !ark_death::plausible(mode_class))
    return {Status::not_ready};

  Result selected{};
  for (int32_t i = 0; i < count; ++i) {
    void* context = contexts[i];
    if (!ark_death::plausible(context)) return {Status::invalid_layout};
    if (*reinterpret_cast<const uint8_t*>(context) != 1) continue; // EWorldType::Game
    void* world = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(context) + 0x260);
    if (!ark_death::plausible(world) || !is_a(world, world_class)) continue;
    void* mode = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(world) + 0x250);
    if (!ark_death::plausible(mode) || !is_a(mode, mode_class)) continue;
    const auto roster = reinterpret_cast<uintptr_t>(world) + 0x488;
    const int32_t players = *reinterpret_cast<const int32_t*>(roster + 8);
    const int32_t reserved = *reinterpret_cast<const int32_t*>(roster + 12);
    if (players < 0 || players > 128 || reserved < players || reserved > 4096)
      return {Status::invalid_layout};
    Key created{-1, 0};
    api.create_weak(&created, world);
    const auto actual = ark_death::object_key(world, api.objects);
    if (!actual || created != *actual || api.resolve_weak(&created, false) != world)
      return {Status::not_ready};
    if (selected.status == Status::ready && selected.world != world)
      return {Status::ambiguous};
    selected = {Status::ready, world, created};
  }
  return selected;
}

} // namespace ark_world_bootstrap
