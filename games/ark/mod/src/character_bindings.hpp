#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only.
// A player's survivor, read on the game thread (PostLogin and the existing 2 s
// sample). It needs no living body, so it also works on the respawn screen:
//   controller+0x488 PlayerState
//   PlayerState.MyPlayerData (UPrimalPlayerData)
//   .MyData (StructProperty) .PlayerDataID (UInt64Property)
//   .MyData .MyPlayerCharacterConfig (StructProperty) .PlayerCharacterName (StrProperty)
// PlayerDataID stays the same across deaths, respawns and restarts. Creating a
// new survivor replaces it with a new random id (0xE93EF0, 50001 or higher).
// See character-analysis.md. Field offsets come from reflection and are cached
// per player data class. Anything unverified yields no character.
#include "net_bindings.hpp"
#include <cstdint>
#include <string>

namespace ark_character {

struct Reading {
  std::string id;
  std::string name;  // empty when the survivor has no readable name
};

enum class Status { ok, no_data, unverified };

// UStructProperty::Struct, stored by its constructor at 0x1D2B4F0.
constexpr size_t kStructPropertyStruct = 0x70;
constexpr uint64_t kFirstGeneratedId = 50001;

struct Layout {
  void* data_class = nullptr;
  int32_t player_data_id = -1;  // from the start of the player data object
  int32_t survivor_name = -1;
};

inline int32_t member_offset(void* property, int32_t width, int32_t container_size) {
  const int32_t offset = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(property) + 0x4C);
  if (container_size <= 0 || container_size > 65536 || offset < 0 || offset > container_size - width) return -1;
  return offset;
}

inline void* named_property(void* owner, const wchar_t* name, const wchar_t* type, const ark_net::Api& api) {
  ark_entity::FName field{};
  api.make_name(&field, name, 1, 1);
  if (field.index <= 0) return nullptr;
  void* property = ark_weapon::find_named_field(owner, field);
  return property && ark_net::property_type_is(property, type, api) ? property : nullptr;
}

inline int32_t container_size(void* owner) {
  return *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(owner) + 0x40);
}

inline void* struct_of(void* property) {
  void* value = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(property) + kStructPropertyStruct);
  return ark_entity::plausible(value) ? value : nullptr;
}

inline bool resolve_layout(void* data, Layout& layout, const ark_net::Api& api) {
  void* klass = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(data) + 0x10);
  if (!ark_entity::plausible(klass)) return false;
  if (layout.data_class == klass) return true;
  void* my_data = named_property(klass, L"MyData", L"StructProperty", api);
  void* data_struct = my_data ? struct_of(my_data) : nullptr;
  if (!data_struct) return false;
  const int32_t data_size = container_size(data_struct);
  const int32_t data_at = member_offset(my_data, data_size, container_size(klass));
  void* id = named_property(data_struct, L"PlayerDataID", L"UInt64Property", api);
  if (data_at < 0 || !id) return false;
  const int32_t id_at = member_offset(id, 8, data_size);
  if (id_at < 0 || (data_at + id_at) % 8 != 0) return false;
  Layout found{klass, data_at + id_at, -1};
  // The name is optional: a missing one must not hide the id.
  void* config = named_property(data_struct, L"MyPlayerCharacterConfig", L"StructProperty", api);
  void* config_struct = config ? struct_of(config) : nullptr;
  if (config_struct) {
    const int32_t config_size = container_size(config_struct);
    const int32_t config_at = member_offset(config, config_size, data_size);
    void* name = named_property(config_struct, L"PlayerCharacterName", L"StrProperty", api);
    const int32_t name_at =
        name ? member_offset(name, static_cast<int32_t>(sizeof(ark_inventory::FString)), config_size) : -1;
    if (config_at >= 0 && name_at >= 0) found.survivor_name = data_at + config_at + name_at;
  }
  layout = found;
  return true;
}

inline Status read(void* controller, Reading& out, Layout& layout, const ark_net::Api& api,
                   const char** failed_step = nullptr) {
  auto fail = [&](const char* step) {
    if (failed_step) *failed_step = step;
    return Status::unverified;
  };
  if (!ark_net::on_game_thread(api)) return fail("api");
  if (!ark_weapon::live_object(controller, api.objects)) return fail("controller");
  void* state = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(controller) + api.player_state_offset);
  if (!state) return Status::no_data;
  ark_weapon::Api objects{};
  objects.make_name = api.make_name;
  objects.objects = api.objects;
  void* data = nullptr;
  if (!ark_weapon::object_field(state, L"MyPlayerData", data, objects)) return fail("MyPlayerData");
  if (!data) return Status::no_data;
  if (!resolve_layout(data, layout, api)) return fail("player-data-layout");
  const auto base = reinterpret_cast<uintptr_t>(data);
  const uint64_t id = *reinterpret_cast<const uint64_t*>(base + layout.player_data_id);
  if (id == 0) return Status::no_data;  // not created yet: spawn screen of a new player
  if (id < kFirstGeneratedId || id > 0xFFFFFFFFull) return fail("id-value");
  Reading reading{std::to_string(id), {}};
  if (layout.survivor_name >= 0)
    ark_entity::copy_borrowed_string(
        *reinterpret_cast<const ark_inventory::FString*>(base + layout.survivor_name), reading.name, 128);
  out = std::move(reading);
  return Status::ok;
}

} // namespace ark_character
