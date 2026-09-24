#include "../src/list_bans_bindings.hpp"
#include <array>
#include <cassert>
#include <cstring>

namespace {
alignas(8) std::array<uint8_t, 0x40> base_class{}, derived_class{};
alignas(8) std::array<uint8_t, 0x800> mode{};
alignas(8) std::array<uint8_t, 0x258> world{};
alignas(8) std::array<uint8_t, 40 * 129> rows{};
constexpr wchar_t first[] = L"76561198000000001";
constexpr wchar_t second[] = L"76561198000000002";
constexpr wchar_t third[] = L"76561198000000065";
constexpr wchar_t fourth[] = L"76561198000000128";
bool matching = true;

template <class T> void put(uint8_t* bytes, size_t offset, T value) {
  std::memcpy(bytes + offset, &value, sizeof(value));
}
void* class_getter() { return base_class.data(); }
int find_index(void*, const ark_inventory::FString* id) {
  if (!matching) return -1;
  if (std::wstring_view(id->data) == first) return 0;
  if (std::wstring_view(id->data) == second) return 2;
  if (std::wstring_view(id->data) == third) return 65;
  if (std::wstring_view(id->data) == fourth) return 128;
  return -1;
}
void set_row(size_t index, const wchar_t* id) {
  const auto length = static_cast<int32_t>(std::wcslen(id) + 1);
  put(rows.data(), 40 * index, ark_inventory::FString{id, length, length});
}
} // namespace

int main() {
  put(world.data(), 0x250, static_cast<void*>(mode.data()));
  put(mode.data(), 0x10, static_cast<void*>(derived_class.data()));
  put(derived_class.data(), 0x30, static_cast<void*>(base_class.data()));
  const size_t at = 0x6E0;
  put(mode.data(), at, static_cast<void*>(rows.data()));
  put<int32_t>(mode.data(), at + 0x08, 3);
  put<int32_t>(mode.data(), at + 0x0C, 3);
  put<uint32_t>(mode.data(), at + 0x10, 0b101);
  put<int32_t>(mode.data(), at + 0x28, 3);
  put<int32_t>(mode.data(), at + 0x2C, 64);
  put<int32_t>(mode.data(), at + 0x34, 1);
  set_row(0, first);
  set_row(2, second);
  ark_list_bans::Api api{};
  api.game_mode_class = class_getter;
  api.find_index = find_index;
  api.expected_class = reinterpret_cast<uintptr_t>(class_getter);
  api.expected_find = reinterpret_cast<uintptr_t>(find_index);
  std::memcpy(api.class_prologue.data(), reinterpret_cast<const void*>(class_getter), 8);
  std::memcpy(api.find_prologue.data(), reinterpret_cast<const void*>(find_index), 8);
  api.game_thread_tid = syscall(SYS_gettid);
  std::vector<std::string> out;
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::ok);
  assert((out == std::vector<std::string>{"76561198000000001", "76561198000000002"}));
  matching = false;
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::invalid_entry);
  assert(out.empty());
  matching = true;
  put<uint32_t>(mode.data(), at + 0x10, 0b001);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::invalid_layout);
  put<uint32_t>(mode.data(), at + 0x10, 0b101);
  put<int32_t>(mode.data(), at + 0x34, 0);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::invalid_layout);
  put<int32_t>(mode.data(), at + 0x34, 1);
  put<int32_t>(mode.data(), at + 0x28, 2);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::invalid_layout);
  put<int32_t>(mode.data(), at + 0x28, 3);
  api.class_prologue[0] ^= 0xff;
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::invalid_layout);
  api.class_prologue[0] ^= 0xff;
  put<int32_t>(mode.data(), at + 0x08, 0);
  put<int32_t>(mode.data(), at + 0x34, 0);
  put<int32_t>(mode.data(), at + 0x28, 0);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::ok);
  assert(out.empty());
  // Retained slots with all occupancy bits cleared are a validated empty set.
  put<int32_t>(mode.data(), at + 0x08, 3);
  put<int32_t>(mode.data(), at + 0x28, 3);
  put<int32_t>(mode.data(), at + 0x34, 3);
  put<uint32_t>(mode.data(), at + 0x10, 0);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::ok);
  assert(out.empty());
  // Native inline bit allocator spans +0x10..+0x1F, or 128 slots.
  put<int32_t>(mode.data(), at + 0x08, 66);
  put<int32_t>(mode.data(), at + 0x0C, 66);
  put<int32_t>(mode.data(), at + 0x28, 66);
  put<int32_t>(mode.data(), at + 0x2C, 128);
  put<int32_t>(mode.data(), at + 0x34, 65);
  put<uint32_t>(mode.data(), at + 0x18, 0b10);
  set_row(65, third);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::ok);
  assert((out == std::vector<std::string>{"76561198000000065"}));
  // Beyond 128 slots, TBitArray uses the external allocation at +0x20.
  std::array<uint32_t, 5> external{};
  external[4] = 1;
  put(mode.data(), at + 0x20, static_cast<void*>(external.data()));
  put<int32_t>(mode.data(), at + 0x08, 129);
  put<int32_t>(mode.data(), at + 0x0C, 129);
  put<int32_t>(mode.data(), at + 0x28, 129);
  put<int32_t>(mode.data(), at + 0x2C, 160);
  put<int32_t>(mode.data(), at + 0x34, 128);
  set_row(128, fourth);
  assert(ark_list_bans::snapshot(world.data(), out, api) == ark_list_bans::Status::ok);
  assert((out == std::vector<std::string>{"76561198000000128"}));
}
