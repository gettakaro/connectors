#pragma once

// Exact ShooterGameServer 21241282 controller ConsoleCommand route.
// A returned FString is command output, never an acknowledgement of effect.
#include <cstdint>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/syscall.h>
#include <unistd.h>

namespace ark_console {

struct FString { const wchar_t* data; int32_t count; int32_t capacity; };
static_assert(sizeof(FString) == 16 && sizeof(wchar_t) == 4);
using ConsoleFn = void (*)(FString*, void*, const FString*, bool);

struct Api {
  void (*game_free)(const void*) = reinterpret_cast<void (*)(const void*)>(0x1AF8E30);
  long game_thread_tid = 0;
  uintptr_t text_min = 0x648400;
  uintptr_t text_max = 0x3F6170F;
  // ShooterPlayerController vtable 0x407BCB0: slot +0x1068 -> 0xF9CE30.
  uintptr_t expected_target = 0xF9CE30;
  std::array<uint8_t, 8> expected_prologue{0x55, 0x48, 0x89, 0xE5,
                                            0x41, 0x57, 0x41, 0x56};
};

enum class Status { invalid, called_no_output, called_with_output, malformed_output };
struct Result {
  Status status;
  std::string output;
  // Deliberately no success bool: native ConsoleCommand has no effect ack.
};

inline bool plausible(const void* pointer) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
  return value >= 0x10000 && (value & 7) == 0;
}

inline bool valid_command(std::string_view command) {
  if (command.empty() || command.size() > 1024) return false;
  for (unsigned char c : command) if (c < 0x20 || c > 0x7e) return false;
  return true;
}

inline void append_utf8(std::string& result, uint32_t c) {
  if (c < 0x80) result.push_back(static_cast<char>(c));
  else if (c < 0x800) {
    result.push_back(static_cast<char>(0xC0 | (c >> 6)));
    result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else if (c < 0x10000) {
    result.push_back(static_cast<char>(0xE0 | (c >> 12)));
    result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  } else {
    result.push_back(static_cast<char>(0xF0 | (c >> 18)));
    result.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (c & 0x3F)));
  }
}

// Controller must be freshly weak-resolved by the caller on the same game
// thread; no raw controller pointer may be retained across logout.
inline Result execute(void* controller, std::string_view command, const Api& api = {}) {
  if (!plausible(controller) || !valid_command(command) || !api.game_free ||
      api.game_thread_tid <= 0 || syscall(SYS_gettid) != api.game_thread_tid ||
      api.text_min >= api.text_max) return {Status::invalid, {}};
  auto* vtable = *reinterpret_cast<const uintptr_t* const*>(controller);
  if (!plausible(vtable)) return {Status::invalid, {}};
  const uintptr_t target = vtable[0x1068 / sizeof(void*)];
  if (target < api.text_min || target >= api.text_max || !api.expected_target ||
      target != api.expected_target ||
      std::memcmp(reinterpret_cast<const void*>(target), api.expected_prologue.data(),
          api.expected_prologue.size()) != 0) return {Status::invalid, {}};
  std::wstring wide(command.begin(), command.end()); // bounded printable ASCII
  const FString request{wide.c_str(), static_cast<int32_t>(wide.size() + 1),
      static_cast<int32_t>(wide.size() + 1)};
  FString output{};
  reinterpret_cast<ConsoleFn>(target)(&output, controller, &request, true);
  struct OutputGuard {
    FString& value;
    const Api& api;
    ~OutputGuard() { if (plausible(value.data)) api.game_free(value.data); }
  } guard{output, api};
  Result result{Status::called_no_output, {}};
  if (output.data) {
    const bool valid = plausible(output.data) && output.count > 0 &&
        output.count <= 16384 && output.capacity >= output.count &&
        output.capacity <= 32768 && output.data[output.count - 1] == 0;
    if (valid) {
      for (int32_t i = 0; i < output.count - 1; ++i) {
        const uint32_t c = static_cast<uint32_t>(output.data[i]);
        if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
          result = {Status::malformed_output, {}};
          break;
        }
        append_utf8(result.output, c);
      }
      if (result.status != Status::malformed_output && !result.output.empty())
        result.status = Status::called_with_output;
    } else result.status = Status::malformed_output;
    // 0x26DD158–61 uses this same game allocator for the returned buffer.
  } else if (output.count != 0 || output.capacity != 0) result.status = Status::malformed_output;
  return result;
}

} // namespace ark_console
