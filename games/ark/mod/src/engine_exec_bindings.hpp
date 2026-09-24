#pragma once

// Exact ShooterGameServer 21241282 GEngine::Exec route. The returned bool
// means that the engine accepted/handled the command, not that a gameplay
// effect was independently verified. The default output device logs inside
// the engine; this wrapper does not capture or manufacture command output.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_engine_exec {

struct WeakWorld { int32_t index; int32_t serial; };
static_assert(sizeof(WeakWorld) == 8);

using ResolveWeakFn = void* (*)(const WeakWorld*, bool);
using OutputDeviceFn = void* (*)();
using ExecFn = bool (*)(void*, void*, const wchar_t*, void*);

struct Api {
  void* const* engine_global = reinterpret_cast<void* const*>(0x59958F8);
  // Generic UObject resolver; 0xEA29D0 additionally requires a controller.
  ResolveWeakFn resolve_weak = reinterpret_cast<ResolveWeakFn>(0x1D56C70);
  OutputDeviceFn output_device = reinterpret_cast<OutputDeviceFn>(0x1B0FCF0);
  long game_thread_tid = 0;
  uintptr_t text_min = 0x648400;
  uintptr_t text_max = 0x3F6170F;
  uintptr_t expected_exec = 0x262EEB0;
  uintptr_t expected_output_device = 0x1B0FCF0;
  uintptr_t expected_resolver = 0x1D56C70;
  std::array<uint8_t, 8> exec_prologue{0x55, 0x48, 0x89, 0xE5,
                                        0x41, 0x57, 0x41, 0x56};
  std::array<uint8_t, 8> output_prologue{0x55, 0x48, 0x89, 0xE5,
                                          0x53, 0x50, 0x8A, 0x05};
  std::array<uint8_t, 10> resolver_prologue{0x8B, 0x4F, 0x04, 0x31, 0xC0,
                                             0x85, 0xC9, 0x74, 0x7A, 0x8B};
};

enum class Status { invalid, unhandled, handled };

inline bool plausible(const void* p) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(p);
  return value >= 0x10000 && (value & 7u) == 0;
}

// UE4 Linux TCHAR is four-byte wchar_t. Keep this command surface bounded and
// reject control characters and malformed UTF-8 before entering game code.
inline bool decode_command(std::string_view input, std::wstring& output) {
  if (input.empty() || input.size() > 4096 || sizeof(wchar_t) != 4) return false;
  output.clear();
  bool has_nonspace = false;
  for (size_t i = 0; i < input.size();) {
    const uint8_t first = static_cast<uint8_t>(input[i]);
    uint32_t code = 0;
    size_t length = 0;
    if (first < 0x80) { code = first; length = 1; }
    else if (first >= 0xC2 && first <= 0xDF) { code = first & 0x1Fu; length = 2; }
    else if (first >= 0xE0 && first <= 0xEF) { code = first & 0x0Fu; length = 3; }
    else if (first >= 0xF0 && first <= 0xF4) { code = first & 0x07u; length = 4; }
    else return false;
    if (i + length > input.size()) return false;
    for (size_t j = 1; j < length; ++j) {
      const uint8_t byte = static_cast<uint8_t>(input[i + j]);
      if ((byte & 0xC0u) != 0x80u) return false;
      code = (code << 6) | (byte & 0x3Fu);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
        (length == 4 && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF) || code < 0x20 ||
        (code >= 0x7F && code <= 0x9F) || code == 0x2028 || code == 0x2029) return false;
    const bool space = code == 0x20 || code == 0xA0 ||
        (code >= 0x2000 && code <= 0x200A) || code == 0x202F ||
        code == 0x205F || code == 0x3000 || code == 0xFEFF;
    if (!space) has_nonspace = true;
    output.push_back(static_cast<wchar_t>(code));
    if (output.size() > 1024) return false;
    i += length;
  }
  return has_nonspace;
}

inline Status execute(void* world, WeakWorld key, std::string_view command,
                      const Api& api = {}) {
  if (!plausible(world) || key.index < 0 || key.serial <= 0 ||
      !api.engine_global || !api.resolve_weak || !api.output_device ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      api.text_min >= api.text_max) return Status::invalid;
  std::wstring wide;
  try {
    if (!decode_command(command, wide)) return Status::invalid;
  } catch (...) {
    return Status::invalid;
  }
  const uintptr_t resolver_target = reinterpret_cast<uintptr_t>(api.resolve_weak);
  const uintptr_t output_target = reinterpret_cast<uintptr_t>(api.output_device);
  if (resolver_target != api.expected_resolver ||
      resolver_target < api.text_min || resolver_target >= api.text_max ||
      std::memcmp(reinterpret_cast<const void*>(resolver_target), api.resolver_prologue.data(),
                  api.resolver_prologue.size()) != 0 ||
      api.resolve_weak(&key, false) != world) return Status::invalid;
  void* engine = *api.engine_global;
  if (!plausible(engine)) return Status::invalid;
  auto* vtable = *reinterpret_cast<const uintptr_t* const*>(engine);
  if (!plausible(vtable)) return Status::invalid;
  const uintptr_t target = vtable[0x290 / sizeof(void*)];
  if (target != api.expected_exec || output_target != api.expected_output_device ||
      target < api.text_min || target >= api.text_max ||
      output_target < api.text_min || output_target >= api.text_max ||
      std::memcmp(reinterpret_cast<const void*>(target), api.exec_prologue.data(),
                  api.exec_prologue.size()) != 0 ||
      std::memcmp(reinterpret_cast<const void*>(output_target), api.output_prologue.data(),
                  api.output_prologue.size()) != 0) return Status::invalid;
  void* output = api.output_device();
  if (!plausible(output)) return Status::invalid;
  return reinterpret_cast<ExecFn>(target)(engine, world, wide.c_str(), output)
      ? Status::handled : Status::unhandled;
}

} // namespace ark_engine_exec
