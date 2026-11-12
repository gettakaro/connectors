// The survivor: controller -> PlayerState -> MyPlayerData -> MyData struct ->
// PlayerDataID and PlayerCharacterName. Nothing unverified is reported.
#include "../src/character_bindings.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
template <size_t N> struct Block { alignas(16) std::array<unsigned char, N> bytes{}; };
template <class T> void put(void* base, size_t at, T value) {
  std::memcpy(static_cast<unsigned char*>(base) + at, &value, sizeof(value));
}
ark_entity::FName* make_name(ark_entity::FName* out, const wchar_t* text, int, int) {
  out->number = 0;
  out->index = std::wcscmp(text, L"MyPlayerData") == 0 ? 21
      : std::wcscmp(text, L"MyData") == 0 ? 22
      : std::wcscmp(text, L"PlayerDataID") == 0 ? 23
      : std::wcscmp(text, L"MyPlayerCharacterConfig") == 0 ? 24
      : std::wcscmp(text, L"PlayerCharacterName") == 0 ? 25
      : std::wcscmp(text, L"StructProperty") == 0 ? 31
      : std::wcscmp(text, L"UInt64Property") == 0 ? 32
      : std::wcscmp(text, L"StrProperty") == 0 ? 33 : 0;
  return out;
}
void game_free(const void*) {}
void field(void* property, int32_t name, int32_t offset, void* type) {
  put<ark_entity::FName>(property, 0x18, {name, 0});
  put<int32_t>(property, 0x4C, offset);
  put<void*>(property, 0x10, type);
}
}

int main() {
  using ark_character::Status;
  Block<0x200> state_class, data_class, data_struct, config_struct, data_field, my_data_field, id_field,
      config_field, name_field, object_type, struct_type, u64_type, str_type;
  Block<0x600> controller, state, data;
  Block<0x100> chunk;
  void* chunks[1] = {chunk.bytes.data()};
  void* const* chunk_table = chunks;
  int32_t count = 8;

  ark_net::Api api{};
  api.make_name = make_name;
  api.game_free = game_free;
  api.objects = {&chunk_table, &count};
  api.game_thread_tid = syscall(SYS_gettid);

  auto live = [&](void* object, int32_t index) {
    put<int32_t>(object, 0x0C, index);
    put<void*>(chunk.bytes.data(), static_cast<size_t>(index) * 16, object);
  };
  live(controller.bytes.data(), 1);
  live(state.bytes.data(), 2);
  live(data.bytes.data(), 3);
  put<void*>(state.bytes.data(), 0x10, state_class.bytes.data());
  put<void*>(data.bytes.data(), 0x10, data_class.bytes.data());
  put<void*>(controller.bytes.data(), api.player_state_offset, state.bytes.data());
  put<int32_t>(state_class.bytes.data(), 0x40, 0x600);
  put<int32_t>(data_class.bytes.data(), 0x40, 0x600);
  put<int32_t>(data_struct.bytes.data(), 0x40, 0xE0);
  put<int32_t>(config_struct.bytes.data(), 0x40, 0x88);
  put<ark_entity::FName>(struct_type.bytes.data(), 0x18, {31, 0});
  put<ark_entity::FName>(u64_type.bytes.data(), 0x18, {32, 0});
  put<ark_entity::FName>(str_type.bytes.data(), 0x18, {33, 0});

  // state.MyPlayerData at +0x100
  put<void*>(state_class.bytes.data(), 0x38, data_field.bytes.data());
  field(data_field.bytes.data(), 21, 0x100, object_type.bytes.data());
  put<void*>(state.bytes.data(), 0x100, data.bytes.data());
  // data.MyData at +0x28, the shape of build 21241282
  put<void*>(data_class.bytes.data(), 0x38, my_data_field.bytes.data());
  field(my_data_field.bytes.data(), 22, 0x28, struct_type.bytes.data());
  put<void*>(my_data_field.bytes.data(), ark_character::kStructPropertyStruct, data_struct.bytes.data());
  // MyData.PlayerDataID at +0, MyData.MyPlayerCharacterConfig at +0x40
  put<void*>(data_struct.bytes.data(), 0x38, id_field.bytes.data());
  field(id_field.bytes.data(), 23, 0, u64_type.bytes.data());
  put<void*>(id_field.bytes.data(), 0x28, config_field.bytes.data());
  field(config_field.bytes.data(), 24, 0x40, struct_type.bytes.data());
  put<void*>(config_field.bytes.data(), ark_character::kStructPropertyStruct, config_struct.bytes.data());
  // config.PlayerCharacterName at +0x68
  put<void*>(config_struct.bytes.data(), 0x38, name_field.bytes.data());
  field(name_field.bytes.data(), 25, 0x68, str_type.bytes.data());

  static wchar_t human[] = L"Human";
  ark_inventory::FString survivor{};
  survivor.data = human;
  survivor.count = 6;
  survivor.capacity = 6;
  put<ark_inventory::FString>(data.bytes.data(), 0x28 + 0x40 + 0x68, survivor);
  put<uint64_t>(data.bytes.data(), 0x28, 92463960);

  void* pc = controller.bytes.data();
  ark_character::Layout layout;
  ark_character::Reading reading;
  const char* step = "";

  assert(ark_character::read(pc, reading, layout, api, &step) == Status::ok);
  assert(reading.id == "92463960" && reading.name == "Human");

  // A new survivor keeps the object and changes the id.
  put<uint64_t>(data.bytes.data(), 0x28, 843947694);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::ok && reading.id == "843947694");

  // No survivor yet is not a failure.
  put<uint64_t>(data.bytes.data(), 0x28, 0);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::no_data);
  put<void*>(state.bytes.data(), 0x100, nullptr);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::no_data);
  put<void*>(state.bytes.data(), 0x100, data.bytes.data());

  // A value the game cannot generate is refused: a pointer read as an id, or a low number.
  put<uint64_t>(data.bytes.data(), 0x28, 0x7f0012345678ull);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::unverified &&
         std::string(step) == "id-value");
  put<uint64_t>(data.bytes.data(), 0x28, 42);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::unverified);
  put<uint64_t>(data.bytes.data(), 0x28, 92463960);

  // An unreadable name leaves the id.
  ark_inventory::FString empty{};
  put<ark_inventory::FString>(data.bytes.data(), 0x28 + 0x40 + 0x68, empty);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::ok);
  assert(reading.id == "92463960" && reading.name.empty());

  // A wrong reflected type fails the layout: the id field must be a uint64.
  ark_character::Layout fresh;
  put<void*>(id_field.bytes.data(), 0x10, str_type.bytes.data());
  assert(ark_character::read(pc, reading, fresh, api, &step) == Status::unverified &&
         std::string(step) == "player-data-layout");
  put<void*>(id_field.bytes.data(), 0x10, u64_type.bytes.data());

  // A dead player data object is not followed.
  put<int32_t>(data.bytes.data(), 0x0C, 7);
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::unverified &&
         std::string(step) == "MyPlayerData");
  put<int32_t>(data.bytes.data(), 0x0C, 3);

  // A half-built object without a class is refused, even with an empty cache.
  ark_character::Layout unset;
  put<void*>(data.bytes.data(), 0x10, nullptr);
  assert(ark_character::read(pc, reading, unset, api, &step) == Status::unverified &&
         std::string(step) == "player-data-layout");
  put<void*>(data.bytes.data(), 0x10, data_class.bytes.data());

  // Never off the game thread.
  api.game_thread_tid = 1;
  assert(ark_character::read(pc, reading, layout, api, &step) == Status::unverified && std::string(step) == "api");
  return 0;
}
