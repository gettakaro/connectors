#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only.
// Read the installed PrimalGameData's already-loaded UPrimalDinoEntry objects.
// No asset loading or guessed blueprint field offsets.
#include "inventory_bindings.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_entity {

struct FName { int32_t index = 0; int32_t number = 0; };
static_assert(sizeof(FName) == 8);
struct Entity { std::string code; std::string name; };
struct Identity {
  uintptr_t game_data = 0;
  uintptr_t storage = 0;
  int32_t count = 0;
  bool operator==(const Identity&) const = default;
};
struct Page { Identity identity; int32_t offset = 0; std::vector<Entity> entities; };
enum class Status { ok, not_ready, changed, invalid_layout, unverified_property, duplicate_code };

struct Api {
  void* const* engine_global = reinterpret_cast<void* const*>(0x59958F8);
  void* (*entry_class)() = reinterpret_cast<void* (*)()>(0x18647B0);
  void* (*dino_character_class)() = reinterpret_cast<void* (*)()>(0x17EDD80);
  void* (*name_property_class)() = reinterpret_cast<void* (*)()>(0x1D2A7D0);
  void* (*string_property_class)() = reinterpret_cast<void* (*)()>(0x1D2B470);
  FName* (*make_name)(FName*, const wchar_t*, int, int) =
      reinterpret_cast<FName* (*)(FName*, const wchar_t*, int, int)>(0x1C892D0);
  ark_inventory::FString* (*name_to_string)(ark_inventory::FString*, const FName*) =
      reinterpret_cast<ark_inventory::FString* (*)(ark_inventory::FString*, const FName*)>(0x1C0B900);
  void (*game_free)(const void*) = reinterpret_cast<void (*)(const void*)>(0x1AF8E30);
  long game_thread_tid = 0;
};

inline bool plausible(const void* p) { return ark_inventory::plausible_pointer(p); }

inline bool is_a(void* candidate, void* base) {
  if (!plausible(candidate) || !plausible(base)) return false;
  for (int depth = 0; depth < 64 && plausible(candidate); ++depth) {
    if (candidate == base) return true;
    candidate = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(candidate) + 0x30);
  }
  return false;
}

// 0x1CCC600 stores the previous UStruct+0x38 child into UField+0x28 and
// prepends the new child at +0x38. 0x1D1DB68 stores property offset at +0x4C.
inline void* find_property(void* klass, const FName& wanted, void* expected_property_class) {
  for (int depth = 0; depth < 64 && plausible(klass); ++depth) {
    auto* child = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(klass) + 0x38);
    for (int fields = 0; fields < 4096 && child; ++fields) {
      if (!plausible(child)) return nullptr;
      const FName found = *reinterpret_cast<const FName*>(reinterpret_cast<uintptr_t>(child) + 0x18);
      if (found.index == wanted.index && found.number == wanted.number) {
        void* property_class = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(child) + 0x10);
        return is_a(property_class, expected_property_class) ? child : nullptr;
      }
      child = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(child) + 0x28);
    }
    if (child) return nullptr; // cyclic or unexpectedly long field chain
    klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(klass) + 0x30);
  }
  return nullptr;
}

// Capture before the death callback's original function destroys or changes
// the actor. Dodo_Character_BP's installed CDO serializes DinoNameTag as a
// NameProperty with value Dodo, matching DinoEntry_Dodo's catalog code.
inline Status actor_code(void* actor, std::string& code, const Api& api) {
  code.clear();
  if (!plausible(actor) || !api.dino_character_class || !api.name_property_class ||
      !api.make_name || !api.name_to_string || !api.game_free ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid)
    return Status::invalid_layout;
  void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(actor) + 0x10);
  void* base = api.dino_character_class();
  void* name_class = api.name_property_class();
  if (!is_a(klass, base) || !plausible(name_class)) return Status::unverified_property;
  FName tag_name{};
  api.make_name(&tag_name, L"DinoNameTag", 1, 1);
  if (tag_name.index <= 0) return Status::unverified_property;
  void* property = find_property(klass, tag_name, name_class);
  if (!property) return Status::unverified_property;
  const int32_t object_size = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(klass) + 0x40);
  const int32_t field_offset = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(property) + 0x4C);
  if (object_size <= 0 || object_size > 65536 || field_offset < 0 || field_offset > object_size - 8)
    return Status::unverified_property;
  const auto* tag = reinterpret_cast<const FName*>(reinterpret_cast<uintptr_t>(actor) + field_offset);
  if (tag->index <= 0) return Status::unverified_property;
  ark_inventory::FString raw{};
  api.name_to_string(&raw, tag);
  ark_inventory::Api conversion{};
  conversion.game_free = api.game_free;
  if (!ark_inventory::copy_and_free(raw, conversion, code) || code.size() > 128)
    return Status::unverified_property;
  return Status::ok;
}

inline bool copy_borrowed_string(const ark_inventory::FString& value, std::string& out,
                                 int32_t maximum = 256) {
  out.clear();
  if (!plausible(value.data) || value.count <= 1 || value.count > maximum + 1 ||
      value.capacity < value.count || value.capacity > 4096 || value.data[value.count - 1] != 0)
    return false;
  for (int32_t i = 0; i < value.count - 1; ++i) {
    const uint32_t cp = static_cast<uint32_t>(value.data[i]);
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    ark_inventory::append_utf8(out, cp);
  }
  return !out.empty() && out.size() <= static_cast<size_t>(maximum);
}

inline Status page(int32_t offset, int32_t limit, Page& out, const Api& api,
                   const Identity* expected = nullptr) {
  out = {};
  if (!api.engine_global || !api.entry_class || !api.name_property_class ||
      !api.string_property_class || !api.make_name || !api.name_to_string || !api.game_free ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      offset < 0 || limit < 1 || limit > 128) return Status::invalid_layout;
  void* engine = *api.engine_global;
  if (!plausible(engine)) return Status::not_ready;
  void* holder = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(engine) + 0x1D0);
  if (!plausible(holder)) return Status::not_ready;
  void* data = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(holder) + 0x30);
  if (!data) data = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(holder) + 0x28);
  if (!plausible(data)) return Status::not_ready;
  constexpr uintptr_t entries_offset = 0xAE8;
  const uintptr_t array = reinterpret_cast<uintptr_t>(data) + entries_offset;
  auto* storage = *reinterpret_cast<void* const* const*>(array);
  const int32_t count = *reinterpret_cast<const int32_t*>(array + 8);
  const int32_t capacity = *reinterpret_cast<const int32_t*>(array + 12);
  if (count <= 0) return Status::not_ready;
  if (count > 4096 || capacity < count || capacity > 4096 || !plausible(storage) || offset > count)
    return Status::invalid_layout;
  const Identity identity{reinterpret_cast<uintptr_t>(data),
                          reinterpret_cast<uintptr_t>(storage), count};
  if (expected && !(identity == *expected)) return Status::changed;

  void* entry_base = api.entry_class();
  void* name_class = api.name_property_class();
  void* string_class = api.string_property_class();
  if (!plausible(entry_base) || !plausible(name_class) || !plausible(string_class))
    return Status::unverified_property;
  FName tag_name{}, label_name{};
  api.make_name(&tag_name, L"DinoNameTag", 1, 1);
  api.make_name(&label_name, L"DinoDescriptiveName", 1, 1);
  if (tag_name.index <= 0 || label_name.index <= 0) return Status::unverified_property;

  Page pending{identity, offset, {}};
  std::unordered_set<std::string> seen;
  const int32_t end = offset + std::min(limit, count - offset);
  pending.entities.reserve(static_cast<size_t>(end - offset));
  ark_inventory::Api conversion{};
  conversion.game_free = api.game_free;
  for (int32_t i = offset; i < end; ++i) {
    void* entry = storage[i];
    if (!entry) continue; // sparse native TArray slot
    if (!plausible(entry)) return Status::invalid_layout;
    void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(entry) + 0x10);
    if (!is_a(klass, entry_base)) return Status::unverified_property;
    void* tag_property = find_property(klass, tag_name, name_class);
    void* label_property = find_property(klass, label_name, string_class);
    if (!tag_property || !label_property) return Status::unverified_property;
    const int32_t object_size = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(klass) + 0x40);
    const int32_t tag_offset = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(tag_property) + 0x4C);
    const int32_t label_offset = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(label_property) + 0x4C);
    if (object_size <= 0 || object_size > 65536 || tag_offset < 0 ||
        label_offset < 0 || tag_offset > object_size - 8 ||
        label_offset > object_size - static_cast<int32_t>(sizeof(ark_inventory::FString)))
      return Status::unverified_property;
    const auto* tag = reinterpret_cast<const FName*>(reinterpret_cast<uintptr_t>(entry) + tag_offset);
    if (tag->index <= 0) return Status::unverified_property;
    ark_inventory::FString raw_code{};
    api.name_to_string(&raw_code, tag);
    std::string code;
    const bool code_ok = ark_inventory::copy_and_free(raw_code, conversion, code);
    const auto* label = reinterpret_cast<const ark_inventory::FString*>(reinterpret_cast<uintptr_t>(entry) + label_offset);
    std::string name;
    if (!code_ok || code.size() > 128 || !copy_borrowed_string(*label, name))
      return Status::unverified_property;
    if (!seen.insert(code).second) return Status::duplicate_code;
    pending.entities.push_back({std::move(code), std::move(name)});
  }
  out = std::move(pending);
  return Status::ok;
}

} // namespace ark_entity
