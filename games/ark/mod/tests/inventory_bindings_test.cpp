#include "../src/inventory_bindings.hpp"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
alignas(8) unsigned char controller[0x500]{};
alignas(8) unsigned char inventory[0x160]{};
alignas(8) unsigned char item[0x340]{};
alignas(8) unsigned char blueprint[0x340]{};
alignas(8) unsigned char recipe[0x340]{};
alignas(8) unsigned char klass[0x40]{};
uintptr_t vtable[0x338 / sizeof(uintptr_t)]{};
void* primary[] = {item, blueprint, recipe, nullptr};
void* equipped[] = {item};
void* recipes[] = {recipe};

void* get_inventory(void*) { return inventory; }
int quantity(void* value) { return value == blueprint ? 3 : 17; }
ark_inventory::FString* text(ark_inventory::FString* out, const wchar_t* value) {
  const size_t length = std::wcslen(value) + 1;
  auto* copy = static_cast<wchar_t*>(std::malloc(length * sizeof(wchar_t)));
  assert(copy);
  std::memcpy(copy, value, length * sizeof(wchar_t));
  *out = {copy, static_cast<int32_t>(length), static_cast<int32_t>(length)};
  return out;
}
ark_inventory::FString* short_name(ark_inventory::FString* out, void*) { return text(out, L"Stone"); }
ark_inventory::FString* class_path(ark_inventory::FString* out, void*) {
  return text(out, L"/Game/PrimalEarth/Items/PrimalItem_Stone.PrimalItem_Stone_C");
}

void set_array(uintptr_t offset, void** data, int count) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(inventory) + offset;
  *reinterpret_cast<void***>(base) = data;
  *reinterpret_cast<int32_t*>(base + 8) = count;
  *reinterpret_cast<int32_t*>(base + 12) = count;
}
}

int main() {
  *reinterpret_cast<uintptr_t*>(item) = reinterpret_cast<uintptr_t>(vtable);
  *reinterpret_cast<uintptr_t*>(item + 0x10) = reinterpret_cast<uintptr_t>(klass);
  *reinterpret_cast<uintptr_t*>(blueprint) = reinterpret_cast<uintptr_t>(vtable);
  *reinterpret_cast<uintptr_t*>(blueprint + 0x10) = reinterpret_cast<uintptr_t>(klass);
  blueprint[0x53] = 0x04; // physical blueprint, retained
  recipe[0x53] = 0x20; // learned engram, excluded even with an ordinary name
  vtable[0x330 / 8] = reinterpret_cast<uintptr_t>(&quantity);
  set_array(0x110, primary, 4);
  set_array(0x120, equipped, 1);
  set_array(0x140, recipes, 1); // learned recipes are not carried inventory
  ark_inventory::Api api{};
  api.get_inventory = get_inventory;
  api.get_short_name = short_name;
  api.get_class_path = class_path;
  api.game_free = [](const void* pointer) { std::free(const_cast<void*>(pointer)); };
  api.text_min = reinterpret_cast<uintptr_t>(&quantity);
  api.text_max = api.text_min + 1;

  std::vector<ark_inventory::Item> rows;
  assert(ark_inventory::snapshot(controller, rows, api) == ark_inventory::Status::ok);
  assert(rows.size() == 2); // equipped duplicate and engram are not counted
  assert(rows[0].amount == 17 && rows[0].name == "Stone");
  assert(rows[1].amount == 3 && rows[1].name == "Stone");
  assert(rows[0].code == "/Game/PrimalEarth/Items/PrimalItem_Stone.PrimalItem_Stone_C");
  set_array(0x110, primary, 1025);
  assert(ark_inventory::snapshot(controller, rows, api) == ark_inventory::Status::invalid_layout && rows.empty());
  set_array(0x110, primary, 4);
  vtable[0x330 / 8] = 0;
  assert(ark_inventory::snapshot(controller, rows, api) == ark_inventory::Status::unverified_item && rows.empty());
  std::cout << "inventory bindings tests passed\n";
}
