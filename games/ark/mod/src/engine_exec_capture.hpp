#pragma once

// Exact-build FOutputDevice capture for GEngine::Exec. The native device
// constructor at 0xD2FCF0 establishes the object layout and six-slot vtable
// 0x4019720. Only Serialize (slot 2) is replaced to enforce a hard output
// bound; the other verified slots retain their native targets. A handled Exec
// result does not prove that a gameplay effect occurred.
#include "engine_exec_bindings.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ark_engine_capture {

constexpr size_t max_codepoints = 8192;
struct FString { wchar_t* data; int32_t count; int32_t capacity; };
struct CaptureDevice {
  uintptr_t* vtable;
  uint8_t flags[8];
  FString native_output;
  std::array<wchar_t, max_codepoints> text;
  size_t length = 0;
  bool truncated = false;
};
static_assert(offsetof(CaptureDevice, native_output) == 0x10);

using ConstructorFn = void (*)(void*, const wchar_t*);
using FreeFn = void (*)(void*);
using SerializeFn = void (*)(void*, const wchar_t*, int32_t, const void*);

struct Api : ark_engine_exec::Api {
  ConstructorFn constructor = reinterpret_cast<ConstructorFn>(0xD2FCF0);
  FreeFn engine_free = reinterpret_cast<FreeFn>(0x1AF8E30);
  uintptr_t expected_constructor = 0xD2FCF0;
  uintptr_t expected_free = 0x1AF8E30;
  uintptr_t expected_vtable = 0x4019720;
  uintptr_t expected_serialize = 0xD2FDD0;
  std::array<uint8_t, 8> constructor_prologue{0x55,0x48,0x89,0xE5,0x41,0x57,0x41,0x56};
  std::array<uint8_t, 8> free_prologue{0x55,0x48,0x89,0xE5,0x53,0x50,0x48,0x89};
};

enum class Status { invalid, unhandled, handled, output_truncated };
struct Result { Status status = Status::invalid; std::string output; };

inline void serialize(void* object, const wchar_t* message, int32_t, const void*) noexcept {
  auto* device = static_cast<CaptureDevice*>(object);
  if (!message) return;
  while (*message) {
    if (device->length == device->text.size()) {
      device->truncated = true;
      return;
    }
    device->text[device->length++] = *message++;
  }
}

inline bool append_utf8(std::string& out, uint32_t c) {
  if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return false;
  if (c <= 0x7F) out.push_back(static_cast<char>(c));
  else if (c <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (c >> 6)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (c >> 12)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (c >> 18)));
    out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
  return true;
}

inline bool exact(uintptr_t target, uintptr_t expected, const std::array<uint8_t,8>& prologue,
                  const Api& api) {
  return target == expected && target >= api.text_min && target < api.text_max &&
      std::memcmp(reinterpret_cast<const void*>(target), prologue.data(), prologue.size()) == 0;
}

// Caller supplies a fresh weak-resolved world. This helper is intentionally
// limited to the known bounded ListPlayers probe pending wider command proof.
inline Result list_players(void* world, ark_engine_exec::WeakWorld key, const Api& api) {
  Result result;
  if (!ark_engine_exec::plausible(world) || key.index < 0 || key.serial <= 0 ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      !api.engine_global || !api.resolve_weak || !api.constructor || !api.engine_free ||
      api.text_min >= api.text_max ||
      reinterpret_cast<uintptr_t>(api.resolve_weak) != api.expected_resolver ||
      api.expected_resolver < api.text_min || api.expected_resolver >= api.text_max ||
      std::memcmp(reinterpret_cast<const void*>(api.resolve_weak),
                  api.resolver_prologue.data(), api.resolver_prologue.size()) != 0 ||
      api.resolve_weak(&key, false) != world ||
      !exact(reinterpret_cast<uintptr_t>(api.constructor), api.expected_constructor,
             api.constructor_prologue, api) ||
      !exact(reinterpret_cast<uintptr_t>(api.engine_free), api.expected_free,
             api.free_prologue, api)) return result;
  void* engine = *api.engine_global;
  if (!ark_engine_exec::plausible(engine)) return result;
  auto* engine_vtable = *reinterpret_cast<const uintptr_t* const*>(engine);
  if (!ark_engine_exec::plausible(engine_vtable)) return result;
  const uintptr_t target = engine_vtable[0x290 / sizeof(void*)];
  if (!exact(target, api.expected_exec, api.exec_prologue, api)) return result;

  CaptureDevice device{};
  api.constructor(&device, L"");
  if (reinterpret_cast<uintptr_t>(device.vtable) != api.expected_vtable ||
      device.vtable[2] != api.expected_serialize || device.native_output.data != nullptr ||
      device.native_output.count != 0 || device.native_output.capacity != 0) return result;
  // The base FOutputDeviceString vtable contains exactly six slots at
  // 0x4019720..0x4019748; clone those and replace Serialize only.
  std::array<uintptr_t, 8> table{};
  for (size_t i = 0; i < 6; ++i) table[i + 2] = device.vtable[i];
  table[4] = reinterpret_cast<uintptr_t>(&serialize);
  device.vtable = table.data() + 2;
  const bool handled = reinterpret_cast<ark_engine_exec::ExecFn>(target)(
      engine, world, L"ListPlayers", &device);
  if (device.native_output.data) api.engine_free(device.native_output.data);
  if (device.truncated) { result.status = Status::output_truncated; return result; }
  try {
    result.output.reserve(device.length * 4);
    for (size_t i = 0; i < device.length; ++i) {
      if (!append_utf8(result.output, static_cast<uint32_t>(device.text[i]))) {
        result.output.clear(); return result;
      }
    }
  } catch (...) { result.output.clear(); return result; }
  result.status = handled ? Status::handled : Status::unhandled;
  return result;
}

} // namespace ark_engine_capture
