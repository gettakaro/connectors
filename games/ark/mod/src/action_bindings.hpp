#pragma once

// Exact ShooterGameServer 21241282 action ABIs. The caller must pass a
// controller freshly resolved from the engine weak roster on the game thread.
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_actions {

struct FString { const wchar_t* data; int32_t count; int32_t capacity; };
struct FVector { float x, y, z; };
struct FRotator { float pitch, yaw, roll; };
static_assert(sizeof(FString) == 16 && sizeof(wchar_t) == 4);
static_assert(sizeof(FVector) == 12 && sizeof(FRotator) == 12);

using GiveItemFn = bool (*)(void*, const FString*, int32_t, bool, bool, float, float);
using TeleportFn = bool (*)(void*, bool, FVector, FRotator);
using LocationFn = FVector (*)(void*);
using RotationFn = FRotator (*)(void*);
using QuantityFn = std::function<std::optional<int64_t>(void*, std::string_view)>;

struct Api {
  GiveItemFn give_item = reinterpret_cast<GiveItemFn>(0xF71E50);
  TeleportFn teleport = reinterpret_cast<TeleportFn>(0x25177F0);
  LocationFn location = reinterpret_cast<LocationFn>(0x2521760);
  long game_thread_tid = 0;
  // Actor::K2_GetActorRotation. Reflected exec thunk 0x2EEBE50 calls this
  // at 0x2EEBE6D, then writes xmm0[0..1] and xmm1[0] to a 12-byte rotator.
  RotationFn rotation = reinterpret_cast<RotationFn>(0x25217B0);
};

enum class Result { invalid, rejected, unverified, verified };

inline bool plausible(const void* pointer) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  return address >= 0x10000 && (address & 7) == 0;
}

inline bool on_game_thread(const Api& api) {
  return api.game_thread_tid > 0 && syscall(SYS_gettid) == api.game_thread_tid;
}

inline bool live_pawn(void* controller, void* pawn) {
  return plausible(controller) && plausible(pawn) &&
      *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x490) == pawn;
}

inline bool valid_code(std::string_view code) {
  if (code.size() < 10 || code.size() > 512) return false;
  if (code.starts_with("Blueprint'")) {
    if (!code.ends_with("'")) return false;
    code.remove_prefix(10);
    code.remove_suffix(1);
  }
  if (!code.starts_with("/Game/") || !code.ends_with("_C")) return false;
  for (unsigned char c : code) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.')) return false;
  }
  return true;
}

// nullopt is invalid input/context. true is the native any-created result,
// never proof that the requested full quantity was granted.
inline std::optional<bool> dispatch_give_item(void* controller, void* pawn,
    std::string_view code, int32_t amount, float quality, bool blueprint,
    const Api& api = {}) {
  if (!on_game_thread(api) || !live_pawn(controller, pawn) || !api.give_item ||
      !valid_code(code) || amount < 1 || amount > 10000 ||
      !std::isfinite(quality) || quality < 0 || quality > 100000) return std::nullopt;
  std::wstring wide(code.begin(), code.end()); // validated ASCII class path
  FString path{wide.c_str(), static_cast<int32_t>(wide.size() + 1), static_cast<int32_t>(wide.size() + 1)};
  return api.give_item(controller, &path, amount, blueprint, false, quality, 0.0f);
}

// Convenience wrapper for callers with a same-class quantity snapshot.
inline Result give_item(void* controller, void* pawn, std::string_view code,
    int32_t amount, float quality, bool blueprint, const QuantityFn& quantity,
    const Api& api = {}) {
  if (!on_game_thread(api) || !live_pawn(controller, pawn) || !valid_code(code)) return Result::invalid;
  int64_t before_count = 0;
  bool before_valid = false;
  if (quantity) {
    const std::optional<int64_t> reading = quantity(controller, code);
    if (reading && *reading >= 0 && *reading <= 1000000000) {
      before_count = *reading;
      before_valid = true;
    }
  }
  const std::optional<bool> dispatched = dispatch_give_item(controller, pawn, code, amount, quality, blueprint, api);
  if (!dispatched) return Result::invalid;
  if (!*dispatched) return Result::rejected;
  if (!before_valid || !quantity) return Result::unverified;
  const std::optional<int64_t> after = quantity(controller, code);
  if (!after || *after < 0 || *after > 1000000000) return Result::unverified;
  return *after - before_count == amount ? Result::verified : Result::unverified;
}

inline std::optional<bool> dispatch_teleport(void* controller, void* pawn, FVector target, const Api& api = {}) {
  if (!on_game_thread(api) || !live_pawn(controller, pawn) || !api.teleport || !api.rotation ||
      !std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z) ||
      std::abs(target.x) > 100000000 || std::abs(target.y) > 100000000 ||
      std::abs(target.z) > 100000000) return std::nullopt;
  const FRotator current = api.rotation(pawn);
  if (!std::isfinite(current.pitch) || !std::isfinite(current.yaw) || !std::isfinite(current.roll) ||
      std::abs(current.pitch) > 1000000 || std::abs(current.yaw) > 1000000 ||
      std::abs(current.roll) > 1000000) return std::nullopt;
  // The reflected teleport thunk passes false for the third boolean parameter.
  return api.teleport(pawn, false, target, current);
}

inline Result teleport_player(void* controller, void* pawn, FVector target, const Api& api = {}) {
  if (!api.location) return Result::invalid;
  const std::optional<bool> dispatched = dispatch_teleport(controller, pawn, target, api);
  if (!dispatched) return Result::invalid;
  if (!*dispatched) return Result::rejected;
  const FVector actual = api.location(pawn);
  if (!std::isfinite(actual.x) || !std::isfinite(actual.y) || !std::isfinite(actual.z))
    return Result::unverified;
  const float dx = actual.x - target.x, dy = actual.y - target.y, dz = actual.z - target.z;
  return dx * dx + dy * dy + dz * dz <= 25.0f * 25.0f ? Result::verified : Result::unverified;
}

} // namespace ark_actions
