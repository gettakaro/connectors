#include "../src/console_bindings.hpp"
#include <cassert>
#include <cstring>
#include <limits>

using namespace ark_console;

namespace {
int calls = 0;
int frees = 0;
bool bad_output = false;
bool astral_output = false;

void command(FString* out, void*, const FString* input, bool write_to_log) {
  ++calls;
  assert(write_to_log && input && input->count == 5);
  assert(std::wstring_view(input->data) == L"List");
  const std::wstring value = bad_output ? std::wstring{static_cast<wchar_t>(0xD800)} :
      astral_output ? std::wstring{static_cast<wchar_t>(0x1F600)} : L"OK é";
  auto* buffer = new wchar_t[value.size() + 1];
  std::memcpy(buffer, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
  *out = FString{buffer, static_cast<int32_t>(value.size() + 1), static_cast<int32_t>(value.size() + 1)};
}
void game_free(const void* data) { ++frees; delete[] static_cast<const wchar_t*>(data); }
}

int main() {
  alignas(8) uintptr_t vtable[0x1070 / 8]{};
  vtable[0x1068 / 8] = reinterpret_cast<uintptr_t>(&command);
  alignas(8) uintptr_t controller[2]{reinterpret_cast<uintptr_t>(vtable)};
  Api api{game_free, syscall(SYS_gettid), reinterpret_cast<uintptr_t>(&command),
      reinterpret_cast<uintptr_t>(&command) + 1};
  assert(execute(controller, "List", api).status == Status::invalid); // no exact target binding
  api.expected_target = reinterpret_cast<uintptr_t>(&command);
  std::memcpy(api.expected_prologue.data(), reinterpret_cast<const void*>(&command), api.expected_prologue.size());
  const Result normal = execute(controller, "List", api);
  assert(normal.status == Status::called_with_output && normal.output == "OK \xC3\xA9");
  assert(calls == 1 && frees == 1);
  bad_output = true;
  const Result bad = execute(controller, "List", api);
  assert(bad.status == Status::malformed_output && bad.output.empty());
  assert(calls == 2 && frees == 2);
  bad_output = false;
  astral_output = true;
  const Result emoji = execute(controller, "List", api);
  assert(emoji.status == Status::called_with_output && emoji.output == "\xF0\x9F\x98\x80");
  assert(calls == 3 && frees == 3);
  Api wrong_target = api; wrong_target.expected_target += 1;
  assert(execute(controller, "List", wrong_target).status == Status::invalid);
  Api wrong_prologue = api; wrong_prologue.expected_prologue[0] ^= 0xFF;
  assert(execute(controller, "List", wrong_prologue).status == Status::invalid);
  assert(execute(controller, "List\n", api).status == Status::invalid);
  Api wrong_thread = api; wrong_thread.game_thread_tid = -1;
  assert(execute(controller, "List", wrong_thread).status == Status::invalid);
  assert(calls == 3);
}
