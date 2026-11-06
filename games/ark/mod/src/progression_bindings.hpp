#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only.
// A player's character level and experience, read on the game thread inside
// the existing 2 s network sample:
//   0xF440A0 AShooterPlayerController::GetPlayerCharacter (weak pointer, null
//            while the player has no living body)
//   character.MyCharacterStatusComponent (+0xCE0)
//   status.BaseCharacterLevel (+0x6C4) + status.ExtraCharacterLevel (+0x6C8),
//            the sum execGetCharacterLevel returns (0xA5CDB0)
//   status.ExperiencePoints (+0x6CC)
//   0xA5D100 / 0xA5D200 GetExperienceRequiredForNextLevelUp / ...PreviousLevelUp
// See progression-analysis.md. The field offsets come from reflection and are
// cached per status class. Anything unverified yields no reading: a level is
// never invented.
#include "net_bindings.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace ark_progression {

struct Reading {
  int32_t level = 0;
  double experience = 0;
  std::optional<double> progress;  // absent at the level cap
};

enum class Status { ok, no_character, unverified };

// Offsets of one verified status class.
struct Layout {
  void* status_class = nullptr;
  int32_t base_level = -1;
  int32_t extra_level = -1;
  int32_t experience = -1;
};

struct Api {
  void* (*player_character)(void*) = reinterpret_cast<void* (*)(void*)>(0xF440A0);
  float (*required_next)(void*) = reinterpret_cast<float (*)(void*)>(0xA5D100);
  float (*required_previous)(void*) = reinterpret_cast<float (*)(void*)>(0xA5D200);
  ark_net::Api net{};
  // Synthetic tests inject functions and disable this check. Native use keeps
  // it enabled after the whole-executable SHA-256 guard has passed.
  bool verify_exact_entries = true;
};

inline bool exact_entries_verified(const Api& api) {
  static constexpr unsigned char character[] =
      {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x81, 0xc3, 0x54, 0x0a, 0x00, 0x00};
  static constexpr unsigned char next[] =
      {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89, 0xfd};
  static constexpr unsigned char previous[] =
      {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x0f, 0x57, 0xc0};
  return reinterpret_cast<uintptr_t>(api.player_character) == 0xF440A0 &&
      reinterpret_cast<uintptr_t>(api.required_next) == 0xA5D100 &&
      reinterpret_cast<uintptr_t>(api.required_previous) == 0xA5D200 &&
      std::memcmp(reinterpret_cast<const void*>(0xF440A0), character, sizeof(character)) == 0 &&
      std::memcmp(reinterpret_cast<const void*>(0xA5D100), next, sizeof(next)) == 0 &&
      std::memcmp(reinterpret_cast<const void*>(0xA5D200), previous, sizeof(previous)) == 0;
}

inline bool resolve_layout(void* status, Layout& layout, const Api& api) {
  void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(status) + 0x10);
  if (layout.status_class == klass) return true;
  Layout found{klass,
               ark_net::scalar_offset(status, L"BaseCharacterLevel", L"IntProperty", 4, api.net),
               ark_net::scalar_offset(status, L"ExtraCharacterLevel", L"UInt16Property", 2, api.net),
               ark_net::scalar_offset(status, L"ExperiencePoints", L"FloatProperty", 4, api.net)};
  if (found.base_level < 0 || found.extra_level < 0 || found.experience < 0) return false;
  layout = found;
  return true;
}

// Fraction of the way from the previous threshold to the next. XP may run past
// the next threshold: ARK only raises the level when the player spends the
// point. Equal thresholds mean the level cap, where no fraction exists.
inline std::optional<double> progress_between(double experience, float previous, float next) {
  if (!std::isfinite(previous) || !std::isfinite(next) || next <= previous) return std::nullopt;
  const double fraction = (experience - previous) / (static_cast<double>(next) - previous);
  return std::round(std::clamp(fraction, 0.0, 1.0) * 10000.0) / 10000.0;
}

inline Status read(void* controller, Reading& out, Layout& layout, const Api& api,
                   const char** failed_step = nullptr) {
  auto fail = [&](const char* step) {
    if (failed_step) *failed_step = step;
    return Status::unverified;
  };
  if (!ark_net::on_game_thread(api.net) || !api.player_character || !api.required_next ||
      !api.required_previous || (api.verify_exact_entries && !exact_entries_verified(api)))
    return fail("api");
  if (!ark_weapon::live_object(controller, api.net.objects)) return fail("controller");
  void* character = api.player_character(controller);
  if (!character) return Status::no_character;
  ark_weapon::Api objects{};
  objects.make_name = api.net.make_name;
  objects.objects = api.net.objects;
  void* status = nullptr;
  if (!ark_weapon::object_field(character, L"MyCharacterStatusComponent", status, objects))
    return fail("MyCharacterStatusComponent");
  if (!status) return Status::no_character;
  if (!resolve_layout(status, layout, api)) return fail("status-layout");
  const auto base = reinterpret_cast<uintptr_t>(status);
  const int64_t level = static_cast<int64_t>(*reinterpret_cast<const int32_t*>(base + layout.base_level)) +
      *reinterpret_cast<const uint16_t*>(base + layout.extra_level);
  const float experience = *reinterpret_cast<const float*>(base + layout.experience);
  if (level < 1 || level > 100000) return fail("level-value");
  if (!std::isfinite(experience) || experience < 0.f) return fail("experience-value");
  out.level = static_cast<int32_t>(level);
  out.experience = std::round(static_cast<double>(experience) * 1000.0) / 1000.0;
  out.progress = progress_between(experience, api.required_previous(status), api.required_next(status));
  return Status::ok;
}

} // namespace ark_progression
