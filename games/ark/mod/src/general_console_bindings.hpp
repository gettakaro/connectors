#pragma once

// ShooterGameServer 21241282: ShooterGameMode owns the reflected
// GlobalCommandsCheatManager UObject at +0x7D0. Class registration calls the
// ShooterGameMode getter 0x13CCDA0 at 0x19C0031 and saves its result in
// 0x58FF200; the GlobalCommandsCheatManager property registration at
// 0x19CEEC7 uses that owner class and sets Offset_Internal=0x7D0 at 0x19CEF34.
// Game code creates the manager at 0xE8491E and sets a manager flag at +0x7C.
// Its native console path (0xEA12AE) calls that object's vtable +0x200 and
// checks the handled bool, then falls back to GEngine::Exec (+0x290). This
// helper calls the same methods on the game thread with a fresh, bounded
// output device. Native FWeakObjectPtr construction at 0x1D56970 assigns a
// serial to newly referenced objects before liveness checks. A handled
// return does not prove a gameplay effect. A fresh world and GameMode are
// required; engine fallback applies only when that GameMode has no manager
// or its manager leaves the command unhandled.
#include "engine_exec_capture.hpp"
#include "world_bootstrap.hpp"
#include <array>
#include <string_view>

namespace ark_general_console {

using Key = ark_engine_exec::WeakWorld;
using CaptureDevice = ark_engine_capture::CaptureDevice;
using ClassGetter = void* (*)();
using CreateWeak = void (*)(ark_death::ObjectKey*, void*);
using ManagerExec = bool (*)(void*, const wchar_t*, void*, void*);

struct Api : ark_engine_capture::Api {
  ClassGetter game_mode_class = reinterpret_cast<ClassGetter>(0x13CCDA0);
  ClassGetter cheat_manager_class = reinterpret_cast<ClassGetter>(0x13CA9C0);
  CreateWeak create_weak = reinterpret_cast<CreateWeak>(0x1D56970);
  ark_death::ObjectArray objects{};
  uintptr_t expected_game_mode_class = 0x13CCDA0;
  uintptr_t expected_cheat_manager_class = 0x13CA9C0;
  uintptr_t expected_manager_exec = 0x8289C0;
  uintptr_t expected_create_weak = 0x1D56970;
  std::array<uint8_t, 8> game_mode_class_prologue{0x55,0x48,0x89,0xE5,0x48,0x8B,0x05,0x25};
  std::array<uint8_t, 8> cheat_manager_class_prologue{0x55,0x48,0x89,0xE5,0x48,0x8B,0x05,0x25};
  std::array<uint8_t, 8> manager_exec_prologue{0x45,0x31,0xC0,0xE9,0xF8,0xB1,0x50,0x01};
  std::array<uint8_t, 8> create_weak_prologue{0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56};
};

enum class Status { rejected, unhandled, handled, output_truncated };
struct Result { Status status = Status::rejected; std::string output; };

inline bool live_object(void* object, const Api& api) {
  // Native FWeakObjectPtr construction assigns a serial on first use. A
  // valid GameMode or manager may have no serial until we create one here.
  ark_death::ObjectKey created{-1, 0};
  api.create_weak(&created, object);
  const auto key = ark_death::object_key(object, api.objects);
  if (!key || created != *key) return false;
  const Key weak{key->index, key->serial};
  return api.resolve_weak(&weak, false) == object;
}

inline Result execute(void* world, Key world_key, std::string_view command,
                      const Api& api = {}) {
  Result result;
  if (!ark_death::plausible(world) || world_key.index < 0 || world_key.serial <= 0 ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      !api.engine_global || !api.resolve_weak || !api.constructor || !api.engine_free ||
      !api.game_mode_class || !api.cheat_manager_class || !api.create_weak ||
      api.text_min >= api.text_max ||
      !ark_engine_capture::exact(reinterpret_cast<uintptr_t>(api.constructor),
                                 api.expected_constructor, api.constructor_prologue, api) ||
      !ark_engine_capture::exact(reinterpret_cast<uintptr_t>(api.engine_free),
                                 api.expected_free, api.free_prologue, api) ||
      !ark_engine_capture::exact(reinterpret_cast<uintptr_t>(api.game_mode_class),
                                 api.expected_game_mode_class, api.game_mode_class_prologue, api) ||
      !ark_engine_capture::exact(reinterpret_cast<uintptr_t>(api.cheat_manager_class),
                                 api.expected_cheat_manager_class,
                                 api.cheat_manager_class_prologue, api) ||
      !ark_engine_capture::exact(reinterpret_cast<uintptr_t>(api.create_weak),
                                 api.expected_create_weak, api.create_weak_prologue, api))
    return result;

  std::wstring wide;
  try {
    if (!ark_engine_exec::decode_command(command, wide)) return result;
  } catch (...) { return result; }
  const uintptr_t resolver = reinterpret_cast<uintptr_t>(api.resolve_weak);
  if (resolver != api.expected_resolver || resolver < api.text_min || resolver >= api.text_max ||
      std::memcmp(reinterpret_cast<const void*>(resolver), api.resolver_prologue.data(),
                  api.resolver_prologue.size()) != 0 ||
      api.resolve_weak(&world_key, false) != world || !live_object(world, api)) return result;

  void* mode = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(world) + 0x250);
  void* mode_class = api.game_mode_class();
  void* cheat_class = api.cheat_manager_class();
  if (!ark_death::plausible(mode) || !ark_death::plausible(mode_class) ||
      !ark_death::plausible(cheat_class) ||
      !ark_world_bootstrap::is_a(mode, mode_class) || !live_object(mode, api)) return result;
  void* manager = *reinterpret_cast<void* const*>(reinterpret_cast<uintptr_t>(mode) + 0x7D0);
  uintptr_t manager_target = 0;
  if (manager) {
    if (!ark_death::plausible(manager) ||
        !ark_world_bootstrap::is_a(manager, cheat_class) || !live_object(manager, api)) return result;
    const auto* vtable = *reinterpret_cast<const uintptr_t* const*>(manager);
    if (!ark_death::plausible(vtable)) return result;
    manager_target = vtable[0x200 / sizeof(uintptr_t)];
    if (!ark_engine_capture::exact(manager_target, api.expected_manager_exec,
                                   api.manager_exec_prologue, api)) return result;
  }

  void* engine = *api.engine_global;
  if (!ark_death::plausible(engine)) return result;
  const auto* engine_vtable = *reinterpret_cast<const uintptr_t* const*>(engine);
  if (!ark_death::plausible(engine_vtable)) return result;
  const uintptr_t engine_target = engine_vtable[0x290 / sizeof(uintptr_t)];
  if (!ark_engine_capture::exact(engine_target, api.expected_exec,
                                 api.exec_prologue, api)) return result;

  CaptureDevice device{};
  api.constructor(&device, L"");
  if (reinterpret_cast<uintptr_t>(device.vtable) != api.expected_vtable ||
      device.vtable[2] != api.expected_serialize || device.native_output.data ||
      device.native_output.count != 0 || device.native_output.capacity != 0) return result;
  std::array<uintptr_t, 8> table{};
  for (size_t i = 0; i < 6; ++i) table[i + 2] = device.vtable[i];
  table[4] = reinterpret_cast<uintptr_t>(&ark_engine_capture::serialize);
  device.vtable = table.data() + 2;
  bool handled = manager && reinterpret_cast<ManagerExec>(manager_target)(
      manager, wide.c_str(), &device, manager);
  if (!handled) handled = reinterpret_cast<ark_engine_exec::ExecFn>(engine_target)(
      engine, world, wide.c_str(), &device);
  if (device.native_output.data) api.engine_free(device.native_output.data);
  if (device.truncated) { result.status = Status::output_truncated; return result; }
  try {
    result.output.reserve(device.length * 4);
    for (size_t i = 0; i < device.length; ++i) {
      if (!ark_engine_capture::append_utf8(
              result.output, static_cast<uint32_t>(device.text[i]))) {
        result.output.clear(); return result;
      }
    }
  } catch (...) { result.output.clear(); return result; }
  result.status = handled ? Status::handled : Status::unhandled;
  return result;
}

} // namespace ark_general_console
