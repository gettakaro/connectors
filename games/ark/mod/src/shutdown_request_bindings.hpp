#pragma once

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace ark_shutdown_request {

constexpr uintptr_t kRequestExit = 0x1b03a90;
constexpr uintptr_t kExitRequested = 0x5938b26;
constexpr uint8_t kPrologue[] = {
    0x55, 0x48, 0x89, 0xe5, 0x40, 0x84, 0xff, 0x75, 0x09,
    0xc6, 0x05, 0x86, 0x50, 0xe3, 0x03, 0x01, 0x5d, 0xc3,
    0xe8, 0x49, 0x44, 0xb4, 0xfe, // force=true branches here to abort@plt
};

using RequestFn = void (*)(bool force);

struct Api {
  RequestFn request = reinterpret_cast<RequestFn>(kRequestExit);
  volatile uint8_t* exit_requested = reinterpret_cast<volatile uint8_t*>(kExitRequested);
  const uint8_t* entry = reinterpret_cast<const uint8_t*>(kRequestExit);
  long game_thread_tid = 0;
  bool exact_binding = true; // false only for injected test functions
};

enum class Status { guarded, already_requested, requested, postcondition_failed };

inline bool signature_matches(const Api& api = {}) {
  if (!api.request || !api.exit_requested || !api.entry) return false;
  if (!api.exact_binding) return true;
  return reinterpret_cast<uintptr_t>(api.request) == kRequestExit &&
      reinterpret_cast<uintptr_t>(api.exit_requested) == kExitRequested &&
      reinterpret_cast<uintptr_t>(api.entry) == kRequestExit &&
      std::memcmp(api.entry, kPrologue, sizeof(kPrologue)) == 0;
}

inline bool ready_to_stage(const Api& api = {}) {
  return signature_matches(api) && *api.exit_requested == 0;
}

inline Status request_after_ack(bool save_completed, bool response_flushed,
                                const Api& api = {}) {
  if (!save_completed || !response_flushed || api.game_thread_tid <= 0 ||
      syscall(SYS_gettid) != api.game_thread_tid || !signature_matches(api))
    return Status::guarded;
  if (*api.exit_requested != 0) return Status::already_requested;
  api.request(false); // Let the verified main loop perform its normal exit.
  return *api.exit_requested == 1 ? Status::requested : Status::postcondition_failed;
}

constexpr bool ascii_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

constexpr char upper_ascii(char c) {
  return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

inline bool contains_unicode_space(std::string_view command) {
  // Do not let UTF-8 whitespace obscure the engine's first token. These are
  // the non-ASCII spaces accepted by the command decoder.
  for (size_t i = 0; i < command.size(); ++i) {
    const auto b = static_cast<uint8_t>(command[i]);
    if (i + 1 < command.size() && b == 0xc2 &&
        static_cast<uint8_t>(command[i + 1]) == 0xa0) return true;
    if (i + 2 >= command.size()) continue;
    const auto b1 = static_cast<uint8_t>(command[i + 1]);
    const auto b2 = static_cast<uint8_t>(command[i + 2]);
    if (b == 0xe2 && b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8a) || b2 == 0xaf)) return true;
    if (b == 0xe2 && b1 == 0x81 && b2 == 0x9f) return true;
    if (b == 0xe3 && b1 == 0x80 && b2 == 0x80) return true;
    if (b == 0xef && b1 == 0xbb && b2 == 0xbf) return true;
  }
  return false;
}

inline bool is_termination_command(std::string_view command) {
  // The engine recognizes EXIT and QUIT; the ARK cheat manager also exposes
  // DoExit. Reject each semicolon-delimited segment and cheat-prefixed form.
  const auto matches = [](std::string_view token, std::string_view expected) {
    if (token.size() != expected.size()) return false;
    for (size_t i = 0; i < token.size(); ++i) {
      if (upper_ascii(token[i]) != expected[i]) return false;
    }
    return true;
  };
  size_t start = 0;
  while (start < command.size()) {
    const size_t end = command.find(';', start);
    const std::string_view segment = command.substr(start, end == std::string_view::npos
        ? end : end - start);
    size_t pos = 0;
    while (pos < segment.size() && ascii_space(segment[pos])) ++pos;
    const size_t token_start = pos;
    while (pos < segment.size() && !ascii_space(segment[pos])) ++pos;
    const auto token = segment.substr(token_start, pos - token_start);
    if (matches(token, "EXIT") || matches(token, "QUIT") || matches(token, "DOEXIT")) return true;
    if (matches(token, "CHEAT") || matches(token, "ADMINCHEAT")) {
      while (pos < segment.size() && ascii_space(segment[pos])) ++pos;
      const size_t next_start = pos;
      while (pos < segment.size() && !ascii_space(segment[pos])) ++pos;
      const auto next = segment.substr(next_start, pos - next_start);
      if (matches(next, "EXIT") || matches(next, "QUIT") || matches(next, "DOEXIT")) return true;
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

} // namespace ark_shutdown_request
