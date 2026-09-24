#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only.
// A bounded view of PrimalGameData::MasterItemList. This does not claim that
// every installed asset or mod-only blueprint is represented in that list.
#include "inventory_bindings.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_catalog {

struct ItemClass {
  std::string code;       // actual UClass path
  std::string class_name; // path leaf, not a localized display name
};
struct Identity {
  uintptr_t game_data = 0;
  uintptr_t storage = 0;
  int32_t count = 0;
  bool operator==(const Identity&) const = default;
};
struct Page {
  Identity identity;
  int32_t offset = 0;
  std::vector<ItemClass> items;
};
struct NamedItem {
  std::string code;
  std::string name; // PrimalItem::GetItemShortName on an already-loaded class default object
};
struct NamedPage {
  Identity identity;
  int32_t offset = 0;
  std::vector<NamedItem> items;
};
enum class Status { ok, not_ready, changed, invalid_layout, unverified_class };

struct Api {
  // 0xF72C55–0xF72C92 resolves these exact addresses/offsets before reading
  // MasterItemList. An injected test supplies its own global storage.
  void* const* engine_global = reinterpret_cast<void* const*>(0x59958F8);
  void* (*primal_item_class)() = reinterpret_cast<void* (*)()>(0x13479B0);
  ark_inventory::FString* (*get_class_path)(ark_inventory::FString*, void*) =
      reinterpret_cast<ark_inventory::FString* (*)(ark_inventory::FString*, void*)>(0x1293230);
  void (*game_free)(const void*) = reinterpret_cast<void (*)(const void*)>(0x1AF8E30);
  long game_thread_tid = 0;
};

inline bool plausible(const void* p) { return ark_inventory::plausible_pointer(p); }

inline bool item_class_is_a(void* candidate, void* base) {
  for (unsigned depth = 0; depth < 64 && plausible(candidate); ++depth) {
    if (candidate == base) return true;
    candidate = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(candidate) + 0x30);
  }
  return false;
}

inline Status page(int32_t offset, int32_t limit, Page& out, const Api& api,
                   const Identity* expected = nullptr) {
  out = {};
  if (!api.engine_global || !api.primal_item_class || !api.get_class_path ||
      !api.game_free || api.game_thread_tid <= 0 ||
      syscall(SYS_gettid) != api.game_thread_tid ||
      offset < 0 || limit < 1 || limit > 128) return Status::invalid_layout;
  void* engine = *api.engine_global;
  if (!plausible(engine)) return Status::not_ready;
  void* holder = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(engine) + 0x1D0);
  if (!plausible(holder)) return Status::not_ready;
  void* data = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(holder) + 0x30);
  if (!data) data = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(holder) + 0x28);
  if (!plausible(data)) return Status::not_ready;
  const uintptr_t array = reinterpret_cast<uintptr_t>(data) + 0x910;
  void* storage = *reinterpret_cast<void* const*>(array);
  const int32_t count = *reinterpret_cast<const int32_t*>(array + 8);
  const int32_t capacity = *reinterpret_cast<const int32_t*>(array + 12);
  if (count <= 0) return Status::not_ready;
  if (count > 65536 || capacity < count || capacity > 65536 || !plausible(storage) ||
      offset > count) return Status::invalid_layout;
  const Identity id{reinterpret_cast<uintptr_t>(data),
                    reinterpret_cast<uintptr_t>(storage), count};
  if (expected && !(id == *expected)) return Status::changed;
  void* primal_item = api.primal_item_class();
  if (!plausible(primal_item)) return Status::unverified_class;
  Page pending{id, offset, {}};
  const int32_t end = offset + std::min(limit, count - offset);
  pending.items.reserve(static_cast<size_t>(end - offset));
  auto* classes = reinterpret_cast<void* const*>(storage);
  ark_inventory::Api conversion{};
  conversion.game_free = api.game_free;
  for (int32_t i = offset; i < end; ++i) {
    void* klass = classes[i];
    if (!klass) continue; // sparse native TArray slot
    if (!plausible(klass) || !item_class_is_a(klass, primal_item)) return Status::unverified_class;
    if (klass == primal_item) continue; // native abstract PrimalItem base
    ark_inventory::FString raw{};
    std::string code;
    api.get_class_path(&raw, klass);
    if (!ark_inventory::copy_and_free(raw, conversion, code) || code.size() > 1024 ||
        code.front() != '/' || code.size() < 3)
      return Status::unverified_class;
    const size_t slash = code.find_last_of('/');
    if (slash == std::string::npos || slash + 1 == code.size()) return Status::unverified_class;
    pending.items.push_back({code, code.substr(slash + 1)});
  }
  out = std::move(pending);
  return Status::ok;
}

// Native 0xB8D043–0xB8D057 and 0xF72179–0xF72195 read UClass+0xF8
// as the class default object. They can lazily create it with 0x1CD3280;
// this path deliberately does not do that, so a catalog page cannot load
// assets synchronously on the game thread. The native short-name getter is
// also used for physical inventory items and yields a caller-owned FString.
inline Status named_page(int32_t offset, int32_t limit, NamedPage& out, const Api& api,
                         ark_inventory::FString* (*get_short_name)(ark_inventory::FString*, void*),
                         const Identity* expected = nullptr) {
  out = {};
  if (!get_short_name) return Status::invalid_layout;
  Page codes;
  const Status status = page(offset, limit, codes, api, expected);
  if (status != Status::ok) return status;
  // Identity.storage is the TArray's UClass* storage itself.
  auto* class_rows = reinterpret_cast<void* const*>(codes.identity.storage);
  NamedPage pending{codes.identity, offset, {}};
  pending.items.reserve(codes.items.size());
  size_t next_code = 0;
  const int32_t end = offset + std::min(limit, codes.identity.count - offset);
  ark_inventory::Api conversion{};
  conversion.game_free = api.game_free;
  void* base_class = api.primal_item_class();
  for (int32_t i = offset; i < end; ++i) {
    void* klass = class_rows[i];
    if (!klass || klass == base_class) continue;
    if (next_code >= codes.items.size()) return Status::changed;
    void* cdo = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(klass) + 0xF8);
    if (!plausible(cdo) ||
        *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(cdo) + 0x10) != klass)
      return Status::not_ready;
    ark_inventory::FString raw{};
    std::string name;
    get_short_name(&raw, cdo);
    if (!ark_inventory::copy_and_free(raw, conversion, name) || name.size() > 256)
      return Status::unverified_class;
    pending.items.push_back({codes.items[next_code++].code, std::move(name)});
  }
  if (next_code != codes.items.size()) return Status::changed;
  out = std::move(pending);
  return Status::ok;
}

} // namespace ark_catalog
