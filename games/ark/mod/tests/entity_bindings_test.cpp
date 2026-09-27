#include "../src/entity_bindings.hpp"
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>

namespace {
template <size_t N> struct Block { alignas(16) std::array<unsigned char, N> bytes{}; };
template <class T> void put(void* base, size_t at, T value) {
  std::memcpy(static_cast<unsigned char*>(base) + at, &value, sizeof(value));
}
void* entry_base{};
void* name_class{};
void* string_class{};
void* dino_class{};
void* get_entry_base() { return entry_base; }
void* get_name_class() { return name_class; }
void* get_string_class() { return string_class; }
void* get_dino_class() { return dino_class; }
void test_free(const void* p) { std::free(const_cast<void*>(p)); }
ark_entity::FName* make_name(ark_entity::FName* out, const wchar_t* text, int, int) {
  out->index = std::wcscmp(text, L"DinoNameTag") == 0 ? 1 : 2;
  out->number = 0;
  return out;
}
ark_inventory::FString* name_to_string(ark_inventory::FString* out, const ark_entity::FName* value) {
  assert(value->index == 3);
  auto* copy = static_cast<wchar_t*>(std::malloc(5 * sizeof(wchar_t)));
  assert(copy);
  std::memcpy(copy, L"Dodo", 5 * sizeof(wchar_t));
  *out = {copy, 5, 5};
  return out;
}
}

int main() {
  Block<0x220> engine, holder;
  Block<0x1000> game_data;
  Block<0x200> base, klass, name_type, string_type, tag_prop, label_prop, entry1, entry2;
  const wchar_t label[] = L"Dodo";
  void* rows[2] = {entry1.bytes.data(), entry2.bytes.data()};
  entry_base = base.bytes.data();
  name_class = name_type.bytes.data();
  string_class = string_type.bytes.data();
  dino_class = klass.bytes.data();
  put<void*>(engine.bytes.data(), 0x1D0, holder.bytes.data());
  put<void*>(holder.bytes.data(), 0x30, game_data.bytes.data());
  put<void*>(game_data.bytes.data(), 0xAE8, rows);
  put<int32_t>(game_data.bytes.data(), 0xAE8 + 8, 1);
  put<int32_t>(game_data.bytes.data(), 0xAE8 + 12, 2);
  put<void*>(klass.bytes.data(), 0x30, base.bytes.data());
  put<void*>(klass.bytes.data(), 0x38, tag_prop.bytes.data());
  put<int32_t>(klass.bytes.data(), 0x40, 0x200);
  put<void*>(tag_prop.bytes.data(), 0x28, label_prop.bytes.data());
  put<void*>(tag_prop.bytes.data(), 0x10, name_type.bytes.data());
  put<void*>(label_prop.bytes.data(), 0x10, string_type.bytes.data());
  put<ark_entity::FName>(tag_prop.bytes.data(), 0x18, {1, 0});
  put<ark_entity::FName>(label_prop.bytes.data(), 0x18, {2, 0});
  put<int32_t>(tag_prop.bytes.data(), 0x4C, 0x90);
  put<int32_t>(label_prop.bytes.data(), 0x4C, 0xA0);
  for (void* entry : rows) {
    put<void*>(entry, 0x10, klass.bytes.data());
    put<ark_entity::FName>(entry, 0x90, {3, 0});
    put<ark_inventory::FString>(entry, 0xA0, {label, 5, 5});
  }
  void* engine_ptr = engine.bytes.data();
  ark_entity::Api api{};
  api.engine_global = &engine_ptr;
  api.entry_class = get_entry_base;
  api.dino_character_class = get_dino_class;
  api.name_property_class = get_name_class;
  api.string_property_class = get_string_class;
  api.make_name = make_name;
  api.name_to_string = name_to_string;
  api.game_free = test_free;
  api.game_thread_tid = syscall(SYS_gettid);
  ark_entity::Page page;
  assert(ark_entity::page(0, 128, page, api) == ark_entity::Status::ok);
  assert(page.entities.size() == 1 && page.entities[0].code == "Dodo" && page.entities[0].name == "Dodo");
  std::string actor_tag;
  assert(ark_entity::actor_code(entry1.bytes.data(), actor_tag, api) == ark_entity::Status::ok);
  assert(actor_tag == "Dodo");
  const ark_entity::Identity identity = page.identity;
  assert(ark_entity::page(0, 128, page, api, &identity) == ark_entity::Status::ok);
  put<int32_t>(game_data.bytes.data(), 0xAE8 + 8, 2);
  assert(ark_entity::page(0, 128, page, api, &identity) == ark_entity::Status::changed);
  assert(ark_entity::page(0, 128, page, api) == ark_entity::Status::duplicate_code);
  assert(page.entities.empty());
  put<int32_t>(game_data.bytes.data(), 0xAE8 + 8, 1);
  put<int32_t>(tag_prop.bytes.data(), 0x4C, 0x1FF);
  assert(ark_entity::page(0, 128, page, api) == ark_entity::Status::unverified_property);
  put<int32_t>(tag_prop.bytes.data(), 0x4C, 0x90);
  put<void*>(tag_prop.bytes.data(), 0x10, string_type.bytes.data());
  assert(ark_entity::page(0, 128, page, api) == ark_entity::Status::unverified_property);
  put<void*>(tag_prop.bytes.data(), 0x10, name_type.bytes.data());
  api.game_thread_tid = -1;
  assert(ark_entity::page(0, 128, page, api) == ark_entity::Status::invalid_layout);
}
