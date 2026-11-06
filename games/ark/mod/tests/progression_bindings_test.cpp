// Character level and experience: controller -> GetPlayerCharacter ->
// MyCharacterStatusComponent -> reflected scalars. Nothing unverified is reported.
#include "../src/progression_bindings.hpp"
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
int name_lookups = 0;
ark_entity::FName* make_name(ark_entity::FName* out, const wchar_t* text, int, int) {
  ++name_lookups;
  out->number = 0;
  out->index = std::wcscmp(text, L"MyCharacterStatusComponent") == 0 ? 21
      : std::wcscmp(text, L"BaseCharacterLevel") == 0 ? 22
      : std::wcscmp(text, L"ExtraCharacterLevel") == 0 ? 23
      : std::wcscmp(text, L"ExperiencePoints") == 0 ? 24
      : std::wcscmp(text, L"IntProperty") == 0 ? 31
      : std::wcscmp(text, L"UInt16Property") == 0 ? 32
      : std::wcscmp(text, L"FloatProperty") == 0 ? 33 : 0;
  return out;
}
void game_free(const void*) {}
void* character_result = nullptr;
void* fake_character(void*) { return character_result; }
float next_threshold = 100.f, previous_threshold = 50.f;
float fake_next(void*) { return next_threshold; }
float fake_previous(void*) { return previous_threshold; }
}

int main() {
  using ark_progression::Status;
  // progress_between
  assert(ark_progression::progress_between(75.0, 50.f, 100.f) == 0.5);
  assert(ark_progression::progress_between(500.0, 50.f, 100.f) == 1.0);  // point not spent yet
  assert(ark_progression::progress_between(10.0, 50.f, 100.f) == 0.0);
  assert(!ark_progression::progress_between(100.0, 100.f, 100.f));       // level cap
  assert(!ark_progression::progress_between(100.0, 50.f, NAN));

  Block<0x200> controller_class, character_class, status_class, component_field, base_field, extra_field,
      xp_field, object_type, int_type, u16_type, float_type;
  Block<0x200> controller, character, status;
  Block<0x100> chunk;
  void* chunks[1] = {chunk.bytes.data()};
  void* const* chunk_table = chunks;
  int32_t count = 8;

  ark_progression::Api api{};
  api.player_character = fake_character;
  api.required_next = fake_next;
  api.required_previous = fake_previous;
  api.verify_exact_entries = false;
  api.net.make_name = make_name;
  api.net.game_free = game_free;
  api.net.objects = {&chunk_table, &count};
  api.net.game_thread_tid = syscall(SYS_gettid);

  auto live = [&](void* object, int32_t index) {
    put<int32_t>(object, 0x0C, index);
    put<void*>(chunk.bytes.data(), static_cast<size_t>(index) * 16, object);
  };
  live(controller.bytes.data(), 1);
  live(character.bytes.data(), 2);
  live(status.bytes.data(), 3);
  put<void*>(controller.bytes.data(), 0x10, controller_class.bytes.data());
  put<void*>(character.bytes.data(), 0x10, character_class.bytes.data());
  put<void*>(status.bytes.data(), 0x10, status_class.bytes.data());
  for (auto* klass : {&controller_class, &character_class, &status_class})
    put<int32_t>(klass->bytes.data(), 0x40, 0x200);
  put<ark_entity::FName>(int_type.bytes.data(), 0x18, {31, 0});
  put<ark_entity::FName>(u16_type.bytes.data(), 0x18, {32, 0});
  put<ark_entity::FName>(float_type.bytes.data(), 0x18, {33, 0});
  // character.MyCharacterStatusComponent at +0x100
  put<void*>(character_class.bytes.data(), 0x38, component_field.bytes.data());
  put<ark_entity::FName>(component_field.bytes.data(), 0x18, {21, 0});
  put<int32_t>(component_field.bytes.data(), 0x4C, 0x100);
  put<void*>(component_field.bytes.data(), 0x10, object_type.bytes.data());
  put<void*>(character.bytes.data(), 0x100, status.bytes.data());
  // status: base +0x120, extra +0x124, experience +0x128
  put<void*>(status_class.bytes.data(), 0x38, base_field.bytes.data());
  put<ark_entity::FName>(base_field.bytes.data(), 0x18, {22, 0});
  put<int32_t>(base_field.bytes.data(), 0x4C, 0x120);
  put<void*>(base_field.bytes.data(), 0x10, int_type.bytes.data());
  put<void*>(base_field.bytes.data(), 0x28, extra_field.bytes.data());
  put<ark_entity::FName>(extra_field.bytes.data(), 0x18, {23, 0});
  put<int32_t>(extra_field.bytes.data(), 0x4C, 0x124);
  put<void*>(extra_field.bytes.data(), 0x10, u16_type.bytes.data());
  put<void*>(extra_field.bytes.data(), 0x28, xp_field.bytes.data());
  put<ark_entity::FName>(xp_field.bytes.data(), 0x18, {24, 0});
  put<int32_t>(xp_field.bytes.data(), 0x4C, 0x128);
  put<void*>(xp_field.bytes.data(), 0x10, float_type.bytes.data());
  put<int32_t>(status.bytes.data(), 0x120, 1);
  put<uint16_t>(status.bytes.data(), 0x124, 12);
  put<float>(status.bytes.data(), 0x128, 62.5f);

  void* pc = controller.bytes.data();
  ark_progression::Layout layout;
  ark_progression::Reading reading;
  const char* step = "";

  // No body yet: not a failure, and nothing is read.
  character_result = nullptr;
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::no_character);
  character_result = character.bytes.data();

  // Level = base + extra, progress between the two thresholds.
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::ok);
  assert(reading.level == 13 && reading.experience == 62.5 && reading.progress == 0.25);

  // The verified layout is cached: only the component lookup names a field again.
  const int lookups = name_lookups;
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::ok);
  assert(name_lookups == lookups + 1);

  // Level cap: no fraction, the level still counts.
  next_threshold = previous_threshold = 100.f;
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::ok && !reading.progress);
  next_threshold = 100.f; previous_threshold = 50.f;

  // Impossible values are refused, never clamped into a level.
  put<int32_t>(status.bytes.data(), 0x120, -40);
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::unverified &&
         std::string(step) == "level-value");
  put<int32_t>(status.bytes.data(), 0x120, 1);
  put<float>(status.bytes.data(), 0x128, NAN);
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::unverified &&
         std::string(step) == "experience-value");
  put<float>(status.bytes.data(), 0x128, 62.5f);

  // A field with the wrong reflected type fails the layout, so nothing is read.
  ark_progression::Layout fresh;
  put<void*>(extra_field.bytes.data(), 0x10, int_type.bytes.data());
  assert(ark_progression::read(pc, reading, fresh, api, &step) == Status::unverified &&
         std::string(step) == "status-layout");
  put<void*>(extra_field.bytes.data(), 0x10, u16_type.bytes.data());

  // A null component is "no body"; a dead object is not followed.
  put<void*>(character.bytes.data(), 0x100, nullptr);
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::no_character);
  put<void*>(character.bytes.data(), 0x100, status.bytes.data());
  put<int32_t>(status.bytes.data(), 0x0C, 7);
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::unverified &&
         std::string(step) == "MyCharacterStatusComponent");
  put<int32_t>(status.bytes.data(), 0x0C, 3);

  // Never off the game thread.
  api.net.game_thread_tid = 1;
  assert(ark_progression::read(pc, reading, layout, api, &step) == Status::unverified && std::string(step) == "api");
  return 0;
}
