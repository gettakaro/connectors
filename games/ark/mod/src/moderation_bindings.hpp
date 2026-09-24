#pragma once

// Exact ShooterGameServer 21241282 kick route. Native bool confirms selection;
// the caller must still confirm the Steam64 disappears from a fresh roster.
#include "inventory_bindings.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_moderation {

enum class Status { dispatched, native_rejected, unavailable, invalid_id, invalid_layout };
enum class BanStatus { changed_in_memory, already_in_state, native_rejected,
                       unavailable, invalid_id, invalid_layout };
enum class BanEffect { invalid, pending_departure, verified };
enum class BanProgress { reject, wait, attempt_kick, confirm };

// The GameMode bool is not an effect acknowledgment for offline IDs. Require
// a fresh, fully validated ban-set snapshot; an online ban is complete only
// after the same Steam64 disappears from the live controller roster.
inline BanEffect ban_effect(BanStatus result, bool should_ban,
                            const std::vector<std::string>& fresh_ids,
                            std::string_view steam64, bool still_online) {
  if (result != BanStatus::changed_in_memory && result != BanStatus::already_in_state)
    return BanEffect::invalid;
  const bool present = std::find(fresh_ids.begin(), fresh_ids.end(), steam64) != fresh_ids.end();
  if (present != should_ban) return BanEffect::invalid;
  return should_ban && still_online ? BanEffect::pending_departure : BanEffect::verified;
}

inline BanProgress ban_progress(BanEffect effect, int64_t elapsed_ms, bool kick_attempted) {
  if (effect == BanEffect::invalid || elapsed_ms > 2500) return BanProgress::reject;
  if (effect == BanEffect::verified) return BanProgress::confirm;
  if (!kick_attempted && elapsed_ms >= 250) return BanProgress::attempt_kick;
  return BanProgress::wait;
}
struct Api {
  void* (*shooter_game_mode_class)() = reinterpret_cast<void* (*)()>(0x13CCDA0);
  bool (*kick_player)(void*, const ark_inventory::FString*, const ark_inventory::FString*) =
      reinterpret_cast<bool (*)(void*, const ark_inventory::FString*, const ark_inventory::FString*)>(0xE91D50);
  long game_thread_tid = 0;
  uintptr_t expected_native = 0xE91D50;
  std::array<uint8_t, 8> expected_prologue{0x55, 0x48, 0x89, 0xE5,
                                            0x41, 0x57, 0x41, 0x56};
  bool (*ban_player)(void*, const ark_inventory::FString*, const ark_inventory::FString*) =
      reinterpret_cast<bool (*)(void*, const ark_inventory::FString*, const ark_inventory::FString*)>(0xE92250);
  bool (*unban_player)(void*, const ark_inventory::FString*, const ark_inventory::FString*) =
      reinterpret_cast<bool (*)(void*, const ark_inventory::FString*, const ark_inventory::FString*)>(0xE927D0);
  int (*find_ban_index)(void*, const ark_inventory::FString*) =
      reinterpret_cast<int (*)(void*, const ark_inventory::FString*)>(0xBF4640);
  uintptr_t expected_ban = 0xE92250, expected_unban = 0xE927D0,
            expected_find = 0xBF4640;
  std::array<uint8_t, 8> ban_prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
  std::array<uint8_t, 8> unban_prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x49};
  std::array<uint8_t, 8> find_prologue{0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};
  uintptr_t expected_game_mode_class = 0x13CCDA0;
  std::array<uint8_t, 8> game_mode_class_prologue{0x55, 0x48, 0x89, 0xE5,
                                                   0x48, 0x8B, 0x05, 0x25};
};

inline bool plausible(const void* p) { return ark_inventory::plausible_pointer(p); }
inline bool game_mode_is_a(void* object, void* base) {
  if (!plausible(object) || !plausible(base)) return false;
  void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(object) + 0x10);
  for (unsigned depth = 0; depth < 64 && plausible(klass); ++depth) {
    if (klass == base) return true;
    klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(klass) + 0x30);
  }
  return false;
}

inline bool valid_steam64(std::string_view steam64) {
  if (steam64.size() < 16 || steam64.size() > 20 ||
      steam64.find_first_not_of("0123456789") != std::string_view::npos) return false;
  uint64_t parsed_id = 0;
  const auto [end, error] = std::from_chars(steam64.data(), steam64.data() + steam64.size(), parsed_id);
  return error == std::errc{} && end == steam64.data() + steam64.size() && parsed_id != 0;
}

inline bool exact_target(uintptr_t actual, uintptr_t expected, const std::array<uint8_t, 8>& bytes) {
  return actual == expected && expected >= 0x10000 &&
      std::memcmp(reinterpret_cast<const void*>(expected), bytes.data(), bytes.size()) == 0;
}

inline void* game_mode(void* world, const Api& api) {
  if (!plausible(world) || !api.shooter_game_mode_class ||
      !exact_target(reinterpret_cast<uintptr_t>(api.shooter_game_mode_class),
                    api.expected_game_mode_class, api.game_mode_class_prologue)) return nullptr;
  void* mode = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(world) + 0x250);
  return plausible(mode) && game_mode_is_a(mode, api.shooter_game_mode_class()) ? mode : nullptr;
}

// 0xE91D50 accepts (optional player name, optional numeric Steam ID). Its
// numeric branch parses the second FString with wcstoll/wcstoull and selects
// the live player's unique ID. Keep the name empty to avoid persona ambiguity.
inline Status kick(void* world, std::string_view steam64, const Api& api) {
  if (!plausible(world) || !api.shooter_game_mode_class || !api.kick_player ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid)
    return Status::invalid_layout;
  if (!exact_target(reinterpret_cast<uintptr_t>(api.kick_player), api.expected_native,
                    api.expected_prologue))
    return Status::invalid_layout;
  if (!valid_steam64(steam64)) return Status::invalid_id;
  void* mode = game_mode(world, api);
  if (!mode) return Status::unavailable;
  std::wstring wide_id(steam64.begin(), steam64.end());
  static constexpr wchar_t empty[] = L"";
  const ark_inventory::FString name{empty, 1, 1};
  const ark_inventory::FString id{wide_id.c_str(),
      static_cast<int32_t>(wide_id.size() + 1), static_cast<int32_t>(wide_id.size() + 1)};
  return api.kick_player(mode, &name, &id) ? Status::dispatched : Status::native_rejected;
}

// Native +0x6E0 ban collection lookup returns -1 when the Steam64 is absent.
// E92250 can insert an offline numeric Steam ID at E923F9 yet return false:
// R14D stays zero on that branch; only the matched online branch sets R14B.
// Thus the verified before/after collection transition is decisive, not AL.
// Persistence across a server restart must still be verified by the caller.
inline BanStatus change_ban(void* world, std::string_view steam64, bool should_ban,
                            const Api& api) {
  if (!plausible(world) || api.game_thread_tid <= 0 ||
      syscall(SYS_gettid) != api.game_thread_tid || !api.ban_player ||
      !api.unban_player || !api.find_ban_index) return BanStatus::invalid_layout;
  if (!valid_steam64(steam64)) return BanStatus::invalid_id;
  if (!exact_target(reinterpret_cast<uintptr_t>(api.ban_player), api.expected_ban, api.ban_prologue) ||
      !exact_target(reinterpret_cast<uintptr_t>(api.unban_player), api.expected_unban, api.unban_prologue) ||
      !exact_target(reinterpret_cast<uintptr_t>(api.find_ban_index), api.expected_find, api.find_prologue))
    return BanStatus::invalid_layout;
  void* mode = game_mode(world, api);
  if (!mode) return BanStatus::unavailable;
  std::wstring wide_id(steam64.begin(), steam64.end());
  static constexpr wchar_t empty[] = L"";
  const ark_inventory::FString name{empty, 1, 1};
  const ark_inventory::FString id{wide_id.c_str(),
      static_cast<int32_t>(wide_id.size() + 1), static_cast<int32_t>(wide_id.size() + 1)};
  void* collection = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mode) + 0x6E0);
  const int before = api.find_ban_index(collection, &id);
  if (before < -1) return BanStatus::invalid_layout;
  if ((before >= 0) == should_ban) return BanStatus::already_in_state;
  if (should_ban) (void)api.ban_player(mode, &name, &id);
  else (void)api.unban_player(mode, &name, &id);
  const int after = api.find_ban_index(collection, &id);
  if (after < -1) return BanStatus::invalid_layout;
  return (after >= 0) == should_ban ? BanStatus::changed_in_memory : BanStatus::native_rejected;
}

} // namespace ark_moderation
