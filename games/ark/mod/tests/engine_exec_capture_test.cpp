#include "../src/engine_exec_capture.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace ark_engine_capture;
namespace {
alignas(8) uint64_t world = 0;
ark_engine_exec::WeakWorld world_key{4, 7};
uintptr_t base_table[6]{};
bool handled = true;
int calls = 0;
void* resolve(const ark_engine_exec::WeakWorld* key, bool pending) {
  return !pending && key->index == 4 && key->serial == 7 ? &world : nullptr;
}
void construct(void* object, const wchar_t* initial) {
  assert(initial && !*initial);
  auto* device = static_cast<CaptureDevice*>(object);
  device->vtable = base_table;
  device->native_output = {};
}
void native_free(void* p) { std::free(p); }
void base_serialize(void*, const wchar_t*, int32_t, const void*) {}
bool execute(void*, void* context, const wchar_t* command, void* output) {
  assert(context == &world && std::wcscmp(command, L"ListPlayers") == 0);
  ++calls;
  auto* device = static_cast<CaptureDevice*>(output);
  reinterpret_cast<SerializeFn>(device->vtable[2])(device, L"76561198000000001 😀", 0, nullptr);
  return handled;
}
struct Engine { uintptr_t* vtable; };
}
int main() {
  base_table[2] = reinterpret_cast<uintptr_t>(&base_serialize);
  uintptr_t engine_table[0x300 / sizeof(uintptr_t)]{};
  engine_table[0x290 / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&execute);
  Engine engine{engine_table};
  void* global = &engine;
  Api api;
  api.engine_global = &global;
  api.resolve_weak = &resolve;
  api.constructor = &construct;
  api.engine_free = &native_free;
  api.game_thread_tid = syscall(SYS_gettid);
  api.text_min = 0x10000;
  api.text_max = std::numeric_limits<uintptr_t>::max();
  api.expected_resolver = reinterpret_cast<uintptr_t>(&resolve);
  api.expected_constructor = reinterpret_cast<uintptr_t>(&construct);
  api.expected_free = reinterpret_cast<uintptr_t>(&native_free);
  api.expected_exec = reinterpret_cast<uintptr_t>(&execute);
  api.expected_vtable = reinterpret_cast<uintptr_t>(base_table);
  api.expected_serialize = reinterpret_cast<uintptr_t>(&base_serialize);
  std::memcpy(api.resolver_prologue.data(), reinterpret_cast<const void*>(&resolve), 10);
  std::memcpy(api.constructor_prologue.data(), reinterpret_cast<const void*>(&construct), 8);
  std::memcpy(api.free_prologue.data(), reinterpret_cast<const void*>(&native_free), 8);
  std::memcpy(api.exec_prologue.data(), reinterpret_cast<const void*>(&execute), 8);
  auto result = list_players(&world, world_key, api);
  assert(result.status == Status::handled);
  assert(result.output == "76561198000000001 \xF0\x9F\x98\x80");
  handled = false;
  result = list_players(&world, world_key, api);
  assert(result.status == Status::unhandled && !result.output.empty());
  const int before = calls;
  assert(list_players(&world, {4, 8}, api).status == Status::invalid);
  Api wrong = api;
  wrong.expected_serialize++;
  assert(list_players(&world, world_key, wrong).status == Status::invalid);
  wrong = api;
  wrong.game_thread_tid++;
  assert(list_players(&world, world_key, wrong).status == Status::invalid);
  wrong = api;
  wrong.constructor_prologue[0] ^= 1;
  assert(list_players(&world, world_key, wrong).status == Status::invalid);
  wrong = api;
  wrong.exec_prologue[0] ^= 1;
  assert(list_players(&world, world_key, wrong).status == Status::invalid);
  assert(calls == before);
  CaptureDevice device{};
  const std::wstring large(max_codepoints + 1, L'A');
  serialize(&device, large.c_str(), 0, nullptr);
  assert(device.truncated && device.length == max_codepoints);
}
