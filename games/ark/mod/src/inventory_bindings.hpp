#pragma once

// Exact ShooterGameServer 21241282 bindings. Call only on the game thread, after
// the executable SHA-256 gate and a fresh live-controller lookup have succeeded.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace ark_inventory {

struct FString {
  const wchar_t* data;
  int32_t count;
  int32_t capacity;
};
static_assert(sizeof(FString) == 16 && sizeof(wchar_t) == 4);

struct Item {
  std::string code;   // UClass::GetPathName, never the localized item label
  std::string name;   // PrimalItem::GetItemShortName
  int32_t amount;     // PrimalItem virtual slot +0x330
};

enum class Status { ok, no_pawn_inventory, invalid_layout, unverified_item };

struct Api {
  void* (*get_inventory)(void*) = reinterpret_cast<void* (*)(void*)>(0xF75500);
  FString* (*get_short_name)(FString*, void*) =
      reinterpret_cast<FString* (*)(FString*, void*)>(0xB8CF40);
  FString* (*get_class_path)(FString*, void*) =
      reinterpret_cast<FString* (*)(FString*, void*)>(0x1293230);
  void (*game_free)(const void*) = reinterpret_cast<void (*)(const void*)>(0x1AF8E30);
  uintptr_t text_min = 0x648400;
  uintptr_t text_max = 0x3F6170F;
};

inline bool plausible_pointer(const void* pointer) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
  return value >= 0x10000 && (value & 7) == 0;
}

inline void append_utf8(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7f) out.push_back(static_cast<char>(codepoint));
  else if (codepoint <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

inline bool copy_and_free(FString& value, const Api& api, std::string& out) {
  const bool valid = value.data && plausible_pointer(value.data) && value.count > 1 &&
      value.count <= 1024 && value.capacity >= value.count && value.capacity <= 4096 &&
      value.data[value.count - 1] == 0;
  if (valid) {
    for (int32_t i = 0; i < value.count - 1; ++i) {
      const uint32_t c = static_cast<uint32_t>(value.data[i]);
      if (c == 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) { out.clear(); break; }
      append_utf8(out, c);
    }
  }
  if (value.data && plausible_pointer(value.data)) api.game_free(value.data);
  value = {};
  return valid && !out.empty();
}

inline Status snapshot(void* controller, std::vector<Item>& result, const Api& api = {}) {
  result.clear();
  if (!plausible_pointer(controller) || !api.get_inventory || !api.get_short_name ||
      !api.get_class_path || !api.game_free || api.text_min >= api.text_max) return Status::invalid_layout;
  void* inventory = api.get_inventory(controller);
  if (!inventory) return Status::no_pawn_inventory;
  if (!plausible_pointer(inventory)) return Status::invalid_layout;

  std::unordered_set<void*> seen;
  std::vector<Item> pending;
  // +0x110 is the array traversed by GetItemTemplateQuantity (0xB414C0).
  // +0x120 holds equipped items. Filter only native bIsEngram; bIsBlueprint
  // uses a different bit and a physical blueprint must remain in inventory.
  for (const uintptr_t offset : {uintptr_t{0x110}, uintptr_t{0x120}}) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(inventory) + offset;
    auto* items = *reinterpret_cast<void* const* const*>(base);
    const int32_t count = *reinterpret_cast<const int32_t*>(base + 8);
    const int32_t capacity = *reinterpret_cast<const int32_t*>(base + 12);
    if (count < 0 || count > 1024 || capacity < count || capacity > 4096 ||
        (count && !plausible_pointer(items))) return Status::invalid_layout;
    for (int32_t i = 0; i < count; ++i) {
      void* item = items[i];
      if (!item) continue; // sparse slots in Unreal TArray
      if (!plausible_pointer(item)) return Status::invalid_layout;
      if (!seen.insert(item).second) continue; // equipped item may appear twice
      if (seen.size() > 2048) return Status::invalid_layout;
      // FindAllItemsOfType (0xB5ADB0) uses +0x53 bit 0x20 for bIsEngram
      // at 0xB5AE7D; bit 0x04 is bIsBlueprint at 0xB5AE6C.
      if (reinterpret_cast<const uint8_t*>(item)[0x53] & 0x20) continue;
      auto* object = reinterpret_cast<const uintptr_t*>(item);
      const auto* vtable = reinterpret_cast<const uintptr_t*>(object[0]);
      void* klass = reinterpret_cast<void*>(object[2]); // UObject ClassPrivate +0x10
      if (!plausible_pointer(vtable) || !plausible_pointer(klass)) return Status::unverified_item;
      const uintptr_t quantity_fn = vtable[0x330 / sizeof(void*)];
      if (quantity_fn < api.text_min || quantity_fn >= api.text_max) return Status::unverified_item;
      const int amount = reinterpret_cast<int (*)(void*)>(quantity_fn)(item);
      if (amount < 1 || amount > 1000000) return Status::unverified_item;
      FString raw_name{}, raw_code{};
      std::string name, code;
      api.get_short_name(&raw_name, item);
      const bool name_ok = copy_and_free(raw_name, api, name);
      api.get_class_path(&raw_code, klass);
      const bool code_ok = copy_and_free(raw_code, api, code);
      if (!name_ok || !code_ok || code.size() > 1024) return Status::unverified_item;
      pending.push_back(Item{std::move(code), std::move(name), amount});
    }
  }
  result.swap(pending);
  return Status::ok;
}

} // namespace ark_inventory
