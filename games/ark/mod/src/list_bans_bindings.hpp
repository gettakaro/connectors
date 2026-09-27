#pragma once

// Exact ShooterGameServer 21241282 GameMode+0x6E0 ban TSet snapshot.
// Return an empty list only after validating the native collection layout.
#include "moderation_bindings.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace ark_list_bans {

enum class Status { ok, unavailable, invalid_layout, invalid_entry, duplicate_id };

struct Api {
  void* (*game_mode_class)() = reinterpret_cast<void* (*)()>(0x13CCDA0);
  int (*find_index)(void*, const ark_inventory::FString*) =
      reinterpret_cast<int (*)(void*, const ark_inventory::FString*)>(0xBF4640);
  uintptr_t expected_class = 0x13CCDA0;
  uintptr_t expected_find = 0xBF4640;
  std::array<uint8_t, 8> class_prologue{0x55, 0x48, 0x89, 0xE5,
                                         0x48, 0x8B, 0x05, 0x25};
  std::array<uint8_t, 8> find_prologue{0x55, 0x48, 0x89, 0xE5,
                                        0x41, 0x57, 0x41, 0x56};
  long game_thread_tid = 0;
};

inline Status snapshot(void* world, std::vector<std::string>& out, const Api& api = {}) {
  out.clear();
  if (!ark_moderation::plausible(world) || !api.game_mode_class || !api.find_index ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      !ark_moderation::exact_target(reinterpret_cast<uintptr_t>(api.game_mode_class),
          api.expected_class, api.class_prologue) ||
      !ark_moderation::exact_target(reinterpret_cast<uintptr_t>(api.find_index),
          api.expected_find, api.find_prologue)) return Status::invalid_layout;

  void* mode = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(world) + 0x250);
  if (!ark_moderation::plausible(mode)) return Status::unavailable;
  void* base = api.game_mode_class();
  if (!ark_moderation::game_mode_is_a(mode, base)) return Status::unavailable;
  const auto collection = reinterpret_cast<uintptr_t>(mode) + 0x6E0;
  void* storage = *reinterpret_cast<void* const*>(collection);
  const int32_t max_index = *reinterpret_cast<const int32_t*>(collection + 0x08);
  const int32_t capacity = *reinterpret_cast<const int32_t*>(collection + 0x0C);
  const int32_t bit_count = *reinterpret_cast<const int32_t*>(collection + 0x28);
  const int32_t bit_capacity = *reinterpret_cast<const int32_t*>(collection + 0x2C);
  const int32_t free_count = *reinterpret_cast<const int32_t*>(collection + 0x34);
  if (max_index < 0 || max_index > 4096 || capacity < max_index || capacity > 4096 ||
      free_count < 0 || free_count > max_index || bit_count != max_index ||
      bit_capacity < bit_count || bit_capacity > 4096 ||
      (max_index && !ark_moderation::plausible(storage))) return Status::invalid_layout;
  // Native TBitArray insertion at 0xBF45F6–0xBF4625 sets the occupancy bit;
  // deletion at 0xBF4541–0xBF4562 clears it. Small sets use inline +0x10.
  const void* external_bits = *reinterpret_cast<void* const*>(collection + 0x20);
  const auto* bits = reinterpret_cast<const uint32_t*>(external_bits ? external_bits
      : reinterpret_cast<const void*>(collection + 0x10));
  if (external_bits && !ark_moderation::plausible(external_bits)) return Status::invalid_layout;
  if (!external_bits && bit_count > 128) return Status::invalid_layout;

  std::vector<std::string> pending;
  pending.reserve(static_cast<size_t>(max_index - free_count));
  std::unordered_set<std::string> seen;
  const auto* rows = reinterpret_cast<const uint8_t*>(storage);
  for (int32_t i = 0; i < max_index; ++i) {
    if (!(bits[static_cast<size_t>(i) / 32] & (uint32_t{1} << (i % 32)))) continue;
    const auto* key = reinterpret_cast<const ark_inventory::FString*>(rows + 40 * i);
    if (!ark_moderation::plausible(key->data) || key->count < 17 || key->count > 21 ||
        key->capacity < key->count || key->capacity > 128 || key->data[key->count - 1] != 0)
      return Status::invalid_entry;
    std::string id;
    id.reserve(static_cast<size_t>(key->count - 1));
    for (int32_t j = 0; j < key->count - 1; ++j) {
      const auto c = static_cast<uint32_t>(key->data[j]);
      if (c < '0' || c > '9') return Status::invalid_entry;
      id.push_back(static_cast<char>(c));
    }
    if (!ark_moderation::valid_steam64(id) || api.find_index(reinterpret_cast<void*>(collection), key) != i)
      return Status::invalid_entry;
    if (!seen.insert(id).second) return Status::duplicate_id;
    pending.push_back(std::move(id));
  }
  if (pending.size() != static_cast<size_t>(max_index - free_count)) return Status::invalid_layout;
  out.swap(pending);
  return Status::ok;
}

} // namespace ark_list_bans
