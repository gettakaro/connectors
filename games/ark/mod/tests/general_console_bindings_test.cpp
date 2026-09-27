#include "../src/general_console_bindings.hpp"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace ark_general_console;
namespace {
alignas(8) unsigned char world[0x300]{};
alignas(8) unsigned char mode[0x800]{};
alignas(8) unsigned char manager[0x100]{};
alignas(8) unsigned char mode_class[0x40]{};
alignas(8) unsigned char cheat_class[0x40]{};
alignas(8) unsigned char other_class[0x40]{};
alignas(8) unsigned char chunk[16 * 3]{};
void* chunk_pointer = chunk;
void* const* chunks = &chunk_pointer;
int32_t object_count = 3;
uintptr_t base_table[6]{};
uintptr_t manager_table[0x208 / 8]{};
uintptr_t engine_table[0x298 / 8]{};
struct Engine { uintptr_t* vtable; } engine{engine_table};
void* engine_pointer = &engine;
enum class Behavior { empty, text, truncate } behavior = Behavior::empty;
bool manager_handles = true;
bool engine_handles = false;
int manager_calls = 0;
int engine_calls = 0;
std::wstring last_command;

void set_pointer(void* object, size_t offset, void* value) {
  std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}
void set_index(void* object, int32_t index) {
  std::memcpy(static_cast<unsigned char*>(object) + 0x0C, &index, sizeof(index));
  set_pointer(chunk, 16 * index, object);
  const int32_t serial = index == 0 ? 10 : 0;
  std::memcpy(chunk + 16 * index + 8, &serial, sizeof(serial));
}
void create_weak(ark_death::ObjectKey* key, void* object) {
  assert(key && object);
  int32_t index = -1;
  std::memcpy(&index, static_cast<unsigned char*>(object) + 0x0C, sizeof(index));
  assert(index >= 0 && index < object_count);
  int32_t serial = 0;
  std::memcpy(&serial, chunk + 16 * index + 8, sizeof(serial));
  if (!serial) {
    serial = index + 10;
    std::memcpy(chunk + 16 * index + 8, &serial, sizeof(serial));
  }
  *key = {index, serial};
}
void* resolve(const Key* key, bool pending) {
  if (pending || !key || key->index < 0 || key->index >= object_count ||
      key->serial != key->index + 10) return nullptr;
  void* value = nullptr;
  std::memcpy(&value, chunk + 16 * key->index, sizeof(value));
  return value;
}
void* get_mode_class() { return mode_class; }
void* get_cheat_class() { return cheat_class; }
void construct(void* object, const wchar_t* initial) {
  assert(initial && !*initial);
  auto* device = static_cast<CaptureDevice*>(object);
  device->vtable = base_table;
  device->native_output = {};
}
void engine_free(void* p) { std::free(p); }
void base_serialize(void*, const wchar_t*, int32_t, const void*) {}
void write(void* output, const wchar_t* message) {
  auto* device = static_cast<CaptureDevice*>(output);
  reinterpret_cast<ark_engine_capture::SerializeFn>(device->vtable[2])(
      device, message, 0, nullptr);
}
bool manager_exec(void* object, const wchar_t* command, void* output, void* executor) {
  assert(object == manager && executor == manager && output);
  ++manager_calls;
  last_command = command;
  if (behavior == Behavior::text) write(output, L"manager \U0001F600");
  if (behavior == Behavior::truncate) {
    const std::wstring large(ark_engine_capture::max_codepoints + 1, L'A');
    write(output, large.c_str());
  }
  return manager_handles;
}
bool engine_exec(void* object, void* context, const wchar_t* command, void* output) {
  assert(object == &engine && context == world && output);
  ++engine_calls;
  last_command = command;
  if (behavior == Behavior::text) write(output, L"engine");
  return engine_handles;
}
template <size_t N> void capture(std::array<uint8_t, N>& bytes, uintptr_t function) {
  std::memcpy(bytes.data(), reinterpret_cast<const void*>(function), N);
}
}

int main() {
  set_index(world, 0); set_index(mode, 1); set_index(manager, 2);
  set_pointer(world, 0x250, mode);
  set_pointer(mode, 0x10, mode_class);
  set_pointer(manager, 0x10, cheat_class);
  set_pointer(mode, 0x7D0, manager);
  set_pointer(manager, 0, manager_table);
  manager_table[0x200 / 8] = reinterpret_cast<uintptr_t>(&manager_exec);
  engine_table[0x290 / 8] = reinterpret_cast<uintptr_t>(&engine_exec);
  base_table[2] = reinterpret_cast<uintptr_t>(&base_serialize);

  Api api;
  api.engine_global = &engine_pointer;
  api.resolve_weak = &resolve;
  api.constructor = &construct;
  api.engine_free = &engine_free;
  api.game_mode_class = &get_mode_class;
  api.cheat_manager_class = &get_cheat_class;
  api.create_weak = &create_weak;
  api.objects = {&chunks, &object_count};
  api.game_thread_tid = syscall(SYS_gettid);
  api.text_min = 0x10000;
  api.text_max = std::numeric_limits<uintptr_t>::max();
  api.expected_resolver = reinterpret_cast<uintptr_t>(&resolve);
  api.expected_constructor = reinterpret_cast<uintptr_t>(&construct);
  api.expected_free = reinterpret_cast<uintptr_t>(&engine_free);
  api.expected_game_mode_class = reinterpret_cast<uintptr_t>(&get_mode_class);
  api.expected_cheat_manager_class = reinterpret_cast<uintptr_t>(&get_cheat_class);
  api.expected_manager_exec = reinterpret_cast<uintptr_t>(&manager_exec);
  api.expected_create_weak = reinterpret_cast<uintptr_t>(&create_weak);
  api.expected_exec = reinterpret_cast<uintptr_t>(&engine_exec);
  api.expected_vtable = reinterpret_cast<uintptr_t>(base_table);
  api.expected_serialize = reinterpret_cast<uintptr_t>(&base_serialize);
  capture(api.resolver_prologue, api.expected_resolver);
  capture(api.constructor_prologue, api.expected_constructor);
  capture(api.free_prologue, api.expected_free);
  capture(api.game_mode_class_prologue, api.expected_game_mode_class);
  capture(api.cheat_manager_class_prologue, api.expected_cheat_manager_class);
  capture(api.manager_exec_prologue, api.expected_manager_exec);
  capture(api.create_weak_prologue, api.expected_create_weak);
  capture(api.exec_prologue, api.expected_exec);

  const Key key{0, 10};
  auto result = execute(world, key, "ServerChat hello", api);
  assert(result.status == Status::handled && result.output.empty());
  assert(manager_calls == 1 && engine_calls == 0 && last_command == L"ServerChat hello");
  int32_t mode_serial = 0, manager_serial = 0;
  std::memcpy(&mode_serial, chunk + 16 + 8, sizeof(mode_serial));
  std::memcpy(&manager_serial, chunk + 32 + 8, sizeof(manager_serial));
  assert(mode_serial == 11 && manager_serial == 12); // assigned on first weak construction
  behavior = Behavior::text;
  manager_handles = false; engine_handles = true;
  result = execute(world, key, "SetTimeOfDay 12:00", api);
  assert(result.status == Status::handled && result.output == "manager \xF0\x9F\x98\x80" "engine");
  assert(manager_calls == 2 && engine_calls == 1);
  behavior = Behavior::empty;
  manager_handles = true;
  result = execute(world, key, "ServerChat again", api);
  assert(result.status == Status::handled && result.output.empty()); // fresh output
  assert(engine_calls == 1);
  manager_handles = false; engine_handles = false;
  result = execute(world, key, "NoSuchCommand", api);
  assert(result.status == Status::unhandled && result.output.empty());
  behavior = Behavior::truncate;
  manager_handles = true;
  result = execute(world, key, "ServerChat huge", api);
  assert(result.status == Status::output_truncated && result.output.empty());
  behavior = Behavior::empty;

  set_pointer(mode, 0x7D0, nullptr);
  engine_handles = true;
  const int previous_manager = manager_calls;
  result = execute(world, key, "GetAll", api);
  assert(result.status == Status::handled && manager_calls == previous_manager);
  set_pointer(mode, 0x7D0, manager);

  const int previous_engine = engine_calls;
  assert(execute(world, key, "bad\ncommand", api).status == Status::rejected);
  assert(execute(world, key, "\xC0\xAF", api).status == Status::rejected);
  assert(execute(world, key, std::string(4097, 'x'), api).status == Status::rejected);
  assert(execute(nullptr, key, "GetAll", api).status == Status::rejected);
  assert(execute(world, {0, 11}, "GetAll", api).status == Status::rejected);
  Api wrong = api; wrong.game_thread_tid++;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  wrong = api; wrong.manager_exec_prologue[0] ^= 1;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  wrong = api; wrong.game_mode_class_prologue[0] ^= 1;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  wrong = api; wrong.cheat_manager_class_prologue[0] ^= 1;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  wrong = api; wrong.create_weak_prologue[0] ^= 1;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  wrong = api; wrong.exec_prologue[0] ^= 1;
  assert(execute(world, key, "GetAll", wrong).status == Status::rejected);
  manager_table[0x200 / 8] = reinterpret_cast<uintptr_t>(&engine_exec);
  assert(execute(world, key, "GetAll", api).status == Status::rejected);
  manager_table[0x200 / 8] = reinterpret_cast<uintptr_t>(&manager_exec);
  set_pointer(manager, 0x10, other_class);
  assert(execute(world, key, "GetAll", api).status == Status::rejected);
  set_pointer(manager, 0x10, cheat_class);
  set_pointer(mode, 0x10, other_class);
  assert(execute(world, key, "GetAll", api).status == Status::rejected);
  set_pointer(mode, 0x10, mode_class);
  const int32_t stale_serial = 99;
  std::memcpy(chunk + 16 * 2 + 8, &stale_serial, sizeof(stale_serial));
  assert(execute(world, key, "GetAll", api).status == Status::rejected);
  assert(engine_calls == previous_engine);
}
