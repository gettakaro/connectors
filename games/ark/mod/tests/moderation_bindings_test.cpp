#include "../src/moderation_bindings.hpp"
#include <cassert>
#include <cstring>

using namespace ark_moderation;
namespace {
int kicks = 0;
bool native_result = true;
bool banned = false;
bool apply_ban = true;
bool ban_return = true;
bool unban_return = true;
alignas(8) unsigned char mode_class[0x38]{};
alignas(8) unsigned char derived_class[0x38]{};
void* static_mode_class() { return mode_class; }
bool kick_native(void*, const ark_inventory::FString* name,
                 const ark_inventory::FString* id) {
  ++kicks;
  assert(name->count == 1 && name->data[0] == 0);
  assert(std::wstring_view(id->data) == L"76561198000000001");
  return native_result;
}
bool ban_native(void*, const ark_inventory::FString* name,
                const ark_inventory::FString* id) {
  assert(name->count == 1 && name->data[0] == 0);
  assert(std::wstring_view(id->data) == L"76561198000000001");
  if (apply_ban) banned = true;
  return ban_return;
}
bool unban_native(void*, const ark_inventory::FString* name,
                  const ark_inventory::FString* id) {
  assert(name->count == 1 && name->data[0] == 0);
  assert(std::wstring_view(id->data) == L"76561198000000001");
  banned = false;
  return unban_return;
}
int find_native(void*, const ark_inventory::FString* id) {
  assert(std::wstring_view(id->data) == L"76561198000000001");
  return banned ? 0 : -1;
}
template<class T> void put(unsigned char* p, size_t offset, T value) {
  std::memcpy(p + offset, &value, sizeof(value));
}
}

int main() {
  alignas(8) unsigned char world[0x258]{};
  alignas(8) unsigned char mode[0x740]{};
  put(world, 0x250, static_cast<void*>(mode));
  put(mode, 0x10, static_cast<void*>(derived_class));
  put(derived_class, 0x30, static_cast<void*>(mode_class));
  Api api{static_mode_class, kick_native, syscall(SYS_gettid)};
  api.expected_game_mode_class = reinterpret_cast<uintptr_t>(&static_mode_class);
  std::memcpy(api.game_mode_class_prologue.data(), reinterpret_cast<const void*>(&static_mode_class), 8);
  api.expected_native = reinterpret_cast<uintptr_t>(&kick_native);
  std::memcpy(api.expected_prologue.data(), reinterpret_cast<const void*>(&kick_native),
              api.expected_prologue.size());
  api.ban_player = ban_native;
  api.unban_player = unban_native;
  api.find_ban_index = find_native;
  api.expected_ban = reinterpret_cast<uintptr_t>(&ban_native);
  api.expected_unban = reinterpret_cast<uintptr_t>(&unban_native);
  api.expected_find = reinterpret_cast<uintptr_t>(&find_native);
  std::memcpy(api.ban_prologue.data(), reinterpret_cast<const void*>(&ban_native), 8);
  std::memcpy(api.unban_prologue.data(), reinterpret_cast<const void*>(&unban_native), 8);
  std::memcpy(api.find_prologue.data(), reinterpret_cast<const void*>(&find_native), 8);
  constexpr auto steam64 = "76561198000000001";
  const std::vector<std::string> present{steam64};
  const std::vector<std::string> absent{};
  assert(ban_effect(BanStatus::changed_in_memory, true, present, steam64, true) ==
         BanEffect::pending_departure);
  assert(ban_effect(BanStatus::already_in_state, true, present, steam64, true) ==
         BanEffect::pending_departure);
  assert(ban_effect(BanStatus::already_in_state, true, present, steam64, false) ==
         BanEffect::verified);
  assert(ban_effect(BanStatus::changed_in_memory, false, absent, steam64, false) ==
         BanEffect::verified);
  assert(ban_effect(BanStatus::already_in_state, false, absent, steam64, true) ==
         BanEffect::verified);
  assert(ban_effect(BanStatus::changed_in_memory, true, absent, steam64, false) ==
         BanEffect::invalid);
  assert(ban_effect(BanStatus::native_rejected, true, present, steam64, false) ==
         BanEffect::invalid);
  assert(ban_progress(BanEffect::invalid, 0, false) == BanProgress::reject);
  assert(ban_progress(BanEffect::verified, 0, false) == BanProgress::confirm);
  assert(ban_progress(BanEffect::pending_departure, 249, false) == BanProgress::wait);
  assert(ban_progress(BanEffect::pending_departure, 250, false) == BanProgress::attempt_kick);
  assert(ban_progress(BanEffect::pending_departure, 500, true) == BanProgress::wait);
  assert(ban_progress(BanEffect::pending_departure, 2501, true) == BanProgress::reject);
  assert(kick(world, steam64, api) == Status::dispatched);
  assert(kicks == 1);
  assert(change_ban(world, steam64, true, api) == BanStatus::changed_in_memory && banned);
  assert(change_ban(world, steam64, true, api) == BanStatus::already_in_state);
  assert(change_ban(world, steam64, false, api) == BanStatus::changed_in_memory && !banned);
  assert(change_ban(world, steam64, false, api) == BanStatus::already_in_state);
  ban_return = false; // exact native offline-ID branch can change state yet return false
  assert(change_ban(world, steam64, true, api) == BanStatus::changed_in_memory && banned);
  unban_return = false;
  assert(change_ban(world, steam64, false, api) == BanStatus::changed_in_memory && !banned);
  ban_return = true;
  unban_return = true;
  apply_ban = false;
  assert(change_ban(world, steam64, true, api) == BanStatus::native_rejected && !banned);
  apply_ban = true;
  native_result = false;
  assert(kick(world, steam64, api) == Status::native_rejected);
  native_result = true;
  assert(kick(world, "bad-id", api) == Status::invalid_id);
  assert(kick(world, "18446744073709551616", api) == Status::invalid_id);
  assert(kicks == 2);
  put(world, 0x250, static_cast<void*>(nullptr));
  assert(kick(world, steam64, api) == Status::unavailable);
  put(world, 0x250, static_cast<void*>(mode));
  api.game_thread_tid = -1;
  assert(kick(world, steam64, api) == Status::invalid_layout);
  api.game_thread_tid = syscall(SYS_gettid);
  api.expected_prologue[0] ^= 0xff;
  assert(kick(world, steam64, api) == Status::invalid_layout);
  api.expected_prologue[0] ^= 0xff;
  api.game_mode_class_prologue[0] ^= 0xff;
  assert(kick(world, steam64, api) == Status::unavailable);
  assert(change_ban(world, steam64, true, api) == BanStatus::unavailable);
}
