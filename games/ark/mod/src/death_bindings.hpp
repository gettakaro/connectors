#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only. Install the
// detour after the executable hash and exact prologue checks. All callbacks
// and original calls run on the game thread; no actor pointer survives invoke.
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_death {

constexpr uintptr_t kBaseDeath = 0xA08D60;
constexpr std::array<uint8_t, 12> kPrologue{
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57,
    0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
using OriginalFn = void (*)(void* victim, void* context, void* killer_character,
    void* other, float damage);

struct ObjectKey {
  int32_t index;
  int32_t serial;
  bool operator==(const ObjectKey&) const = default;
};

inline uint64_t packed(ObjectKey key) {
  return (static_cast<uint64_t>(static_cast<uint32_t>(key.index)) << 32) |
      static_cast<uint32_t>(key.serial);
}

struct ObjectArray {
  void* const* const* chunks = reinterpret_cast<void* const* const*>(0x595B7F0);
  const int32_t* count = reinterpret_cast<const int32_t*>(0x595B804);
};

inline bool plausible(const void* pointer) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
  return value >= 0x10000 && (value & 7) == 0;
}

inline bool prologue_matches(const void* address = reinterpret_cast<const void*>(kBaseDeath)) {
  return address && std::memcmp(address, kPrologue.data(), kPrologue.size()) == 0;
}

// Reconstruct the same index/serial pair used by native FWeakObjectPtr.
// 0x1D56E28 reads UObject+0x0C; 0x1D56E52–67 reads the 16-byte object item.
inline std::optional<ObjectKey> object_key(const void* object, const ObjectArray& array = {}) {
  if (!plausible(object) || !array.count || !array.chunks) return std::nullopt;
  const int32_t count = *array.count;
  const int32_t index = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(object) + 0x0C);
  if (count <= 0 || count > 10000000 || index < 0 || index >= count) return std::nullopt;
  auto* chunks = *array.chunks;
  if (!plausible(chunks)) return std::nullopt;
  auto* chunk = reinterpret_cast<const uint8_t*>(chunks[static_cast<uint32_t>(index) >> 16]);
  if (!plausible(chunk)) return std::nullopt;
  const uint8_t* item = chunk + (static_cast<uint32_t>(index) & 0xFFFFu) * 16u;
  const void* actual = *reinterpret_cast<void* const*>(item);
  const int32_t serial = *reinterpret_cast<const int32_t*>(item + 8);
  if (actual != object || serial <= 0) return std::nullopt;
  return ObjectKey{index, serial};
}

enum class Type { player_death, entity_killed };
struct Event {
  Type type;
  std::string victim_id;     // Steam64 for player death, entity class path otherwise
  std::string victim_name;   // copied survivor display name before original; bounded to 128 bytes
  std::string attacker_id;   // Steam64 only when killer pawn matched fresh weak roster
  ObjectKey victim_key;
};
using CaptureFn = std::function<std::optional<Event>(void* victim, void* killer_character, ObjectKey)>;
using EmitFn = std::function<void(const Event&)>;

class HookSupport {
public:
  HookSupport(OriginalFn original, CaptureFn capture, EmitFn emit, long game_thread_tid,
      ObjectArray objects = {})
      : original_(original), capture_(std::move(capture)), emit_(std::move(emit)),
        game_thread_tid_(game_thread_tid), objects_(objects) {}

  void invoke(void* victim, void* context, void* killer_character, void* other, float damage) {
    if (!original_) return; // installation must fail closed before reaching here
    if (syscall(SYS_gettid) != game_thread_tid_ || !plausible(victim) || !capture_ || !emit_) {
      original_(victim, context, killer_character, other, damage);
      return;
    }
    // All known PrimalCharacter/ShooterCharacter/dino vtables route +0x400
    // to 0xA02110, which returns this exact bIsDead bit. Base 0xA08D94
    // skips OnDied when it is already set.
    const bool was_dead = (*reinterpret_cast<const uint8_t*>(
        reinterpret_cast<uintptr_t>(victim) + 0x8A8) & 0x20u) != 0;
    const std::optional<ObjectKey> key = was_dead ? std::nullopt : object_key(victim, objects_);
    if (!key || completed_.contains(packed(*key)) || in_flight_.contains(packed(*key))) {
      original_(victim, context, killer_character, other, damage);
      return;
    }
    const uint64_t id = packed(*key);
    try {
      in_flight_.insert(id);
    } catch (...) {
      original_(victim, context, killer_character, other, damage);
      return;
    }
    struct Scope {
      std::unordered_set<uint64_t>& set;
      uint64_t id;
      ~Scope() { set.erase(id); }
    } scope{in_flight_, id};
    std::optional<Event> captured;
    try {
      captured = capture_(victim, killer_character, *key);
    } catch (...) {
      original_(victim, context, killer_character, other, damage);
      return;
    }
    original_(victim, context, killer_character, other, damage);
    // Base 0xA08DA1–B1 broadcasts the OnDied delegate unconditionally after
    // the false guard; no actor dereference after original is required.
    if (!captured || captured->victim_id.empty() || captured->victim_id.size() > 512 ||
        captured->victim_name.size() > 128 ||
        captured->attacker_id.size() > 64) return;
    try {
      if (!completed_.insert(id).second) return;
      completed_order_.push_back(id);
      if (completed_order_.size() > 4096) {
        completed_.erase(completed_order_.front());
        completed_order_.pop_front();
      }
      emit_(*captured);
    } catch (...) {
      // Observability must not unwind through the engine's death callback.
    }
  }

private:
  OriginalFn original_;
  CaptureFn capture_;
  EmitFn emit_;
  long game_thread_tid_;
  ObjectArray objects_;
  std::unordered_set<uint64_t> in_flight_;
  std::unordered_set<uint64_t> completed_;
  std::deque<uint64_t> completed_order_;
};

} // namespace ark_death
