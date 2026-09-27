#include "../src/engine_exec_bindings.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace ark_engine_exec;

namespace {
alignas(8) uint64_t world_storage = 0;
alignas(8) uint64_t output_storage = 0;
void* expected_world = &world_storage;
WeakWorld expected_key{12, 34};
bool handler_result = false;
bool output_enabled = true;
int handler_calls = 0;
std::wstring last_command;
void* last_output = nullptr;

void* resolve(const WeakWorld* key, bool even_if_pending_kill) {
  return !even_if_pending_kill && key &&
      key->index == expected_key.index && key->serial == expected_key.serial
      ? expected_world : nullptr;
}
void* output_device() { return output_enabled ? &output_storage : nullptr; }
bool engine_exec(void*, void* world, const wchar_t* command, void* output) {
  assert(world == expected_world);
  ++handler_calls;
  last_command = command;
  last_output = output;
  return handler_result;
}
struct FakeEngine { uintptr_t* vtable; };
}

int main() {
  uintptr_t vtable[0x300 / sizeof(uintptr_t)]{};
  vtable[0x290 / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&engine_exec);
  FakeEngine engine{vtable};
  void* engine_pointer = &engine;
  Api api;
  api.engine_global = &engine_pointer;
  api.resolve_weak = &resolve;
  api.output_device = &output_device;
  api.game_thread_tid = syscall(SYS_gettid);
  api.text_min = 0x10000;
  api.text_max = std::numeric_limits<uintptr_t>::max();
  api.expected_exec = reinterpret_cast<uintptr_t>(&engine_exec);
  api.expected_output_device = reinterpret_cast<uintptr_t>(&output_device);
  api.expected_resolver = reinterpret_cast<uintptr_t>(&resolve);
  std::memcpy(api.exec_prologue.data(), reinterpret_cast<const void*>(api.expected_exec), 8);
  std::memcpy(api.output_prologue.data(),
              reinterpret_cast<const void*>(api.expected_output_device), 8);
  std::memcpy(api.resolver_prologue.data(),
              reinterpret_cast<const void*>(api.expected_resolver), 10);

  handler_result = true;
  assert(execute(&world_storage, expected_key, "ListPlayers", api) == Status::handled);
  assert(handler_calls == 1 && last_command == L"ListPlayers");
  assert(last_output == &output_storage);
  handler_result = false;
  assert(execute(&world_storage, expected_key, "UnknownCommand", api) == Status::unhandled);
  assert(handler_calls == 2);
  assert(execute(&world_storage, expected_key, "Name \xF0\x9F\x98\x80", api) == Status::unhandled);
  assert(last_command == L"Name \U0001F600");

  const int before_invalid = handler_calls;
  assert(execute(&world_storage, expected_key, "bad\ncommand", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, " \xC2\xA0 ", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, "bad\xC2\x85" "command", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, "bad\xE2\x80\xA8" "command", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, "\xC0\xAF", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, "\xED\xA0\x80", api) == Status::invalid);
  assert(execute(&world_storage, expected_key, std::string(4097, 'x'), api) == Status::invalid);
  assert(execute(&world_storage, {12, 35}, "ListPlayers", api) == Status::invalid);
  assert(execute(&output_storage, expected_key, "ListPlayers", api) == Status::invalid);
  assert(handler_calls == before_invalid);

  Api bad = api;
  bad.game_thread_tid++;
  assert(execute(&world_storage, expected_key, "ListPlayers", bad) == Status::invalid);
  bad = api;
  bad.exec_prologue[0] ^= 1;
  assert(execute(&world_storage, expected_key, "ListPlayers", bad) == Status::invalid);
  bad = api;
  bad.output_prologue[0] ^= 1;
  assert(execute(&world_storage, expected_key, "ListPlayers", bad) == Status::invalid);
  bad = api;
  bad.resolver_prologue[0] ^= 1;
  assert(execute(&world_storage, expected_key, "ListPlayers", bad) == Status::invalid);
  bad = api;
  bad.expected_exec++;
  assert(execute(&world_storage, expected_key, "ListPlayers", bad) == Status::invalid);
  output_enabled = false;
  assert(execute(&world_storage, expected_key, "ListPlayers", api) == Status::invalid);
  assert(handler_calls == before_invalid);
}
