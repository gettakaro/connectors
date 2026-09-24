#include "../src/world_bootstrap.hpp"
#include <array>
#include <cassert>
#include <cstring>

namespace {
alignas(8) std::array<uint8_t, 0x940> engine{};
alignas(8) std::array<uint8_t, 0x500> world{}, second_world{};
alignas(8) std::array<uint8_t, 0x740> mode{}, second_mode{};
alignas(8) std::array<uint8_t, 0x268> context{}, second_context{};
alignas(8) std::array<uint8_t, 0x40> world_class{}, derived_world_class{},
    mode_class{}, derived_mode_class{};
alignas(8) std::array<uint8_t, 48> object_items{};
void* engine_pointer = engine.data();
void* context_ptrs[2]{context.data(), second_context.data()};
void* chunks[1]{object_items.data()};
void* const* chunks_ptr = chunks;
int32_t object_count = 3;
bool weak_valid = true;
std::array<uint8_t, 10> engine_reference{}, context_lookup{}, game_creation{};

template <class T> void put(uint8_t* bytes, size_t offset, T value) {
  std::memcpy(bytes + offset, &value, sizeof(value));
}
void* get_world_class() { return world_class.data(); }
void* get_mode_class() { return mode_class.data(); }
void create_weak(ark_death::ObjectKey* out, void* value) {
  const int32_t index = value == world.data() ? 1 : 2;
  put(object_items.data(), static_cast<size_t>(index) * 16 + 8, int32_t{7});
  *out = {index, 7};
}
void* resolve_weak(const ark_death::ObjectKey* key, bool) {
  if (!weak_valid || key->serial != 7) return nullptr;
  return key->index == 1 ? world.data() : key->index == 2 ? second_world.data() : nullptr;
}

template <size_t N> void mock_binding(uintptr_t& expected, std::array<uint8_t, N>& bytes,
                                     uintptr_t actual) {
  expected = actual;
  std::memcpy(bytes.data(), reinterpret_cast<const void*>(actual), N);
}
} // namespace

int main() {
  using namespace ark_world_bootstrap;
  const Api exact_build{};
  assert(exact_build.expected_world_class == 0x2D1E730);
  assert(exact_build.world_class_bytes ==
      (std::array<uint8_t, 14>{0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x05,
                               0x4D, 0x97, 0xC8, 0x02, 0x48, 0x85, 0xC0}));
  put(engine.data(), 0x920, static_cast<void*>(context_ptrs));
  put<int32_t>(engine.data(), 0x928, 1);
  put<int32_t>(engine.data(), 0x92C, 2);
  context[0] = 1; // native game context; preview/editor contexts are rejected
  put(context.data(), 0x260, static_cast<void*>(world.data()));
  put(world.data(), 0x10, static_cast<void*>(derived_world_class.data()));
  put(derived_world_class.data(), 0x30, static_cast<void*>(world_class.data()));
  put(world.data(), 0x250, static_cast<void*>(mode.data()));
  put(mode.data(), 0x10, static_cast<void*>(derived_mode_class.data()));
  put(derived_mode_class.data(), 0x30, static_cast<void*>(mode_class.data()));
  put<int32_t>(world.data(), 0x0C, 1);
  put<int32_t>(world.data(), 0x490, 0);
  put<int32_t>(world.data(), 0x494, 16);
  put(object_items.data(), 16, static_cast<void*>(world.data()));

  Api api{};
  api.engine_global = &engine_pointer;
  api.expected_engine_global = reinterpret_cast<uintptr_t>(&engine_pointer);
  api.game_thread_tid = syscall(SYS_gettid);
  api.objects = {&chunks_ptr, &object_count};
  api.world_class = get_world_class;
  api.game_mode_class = get_mode_class;
  api.create_weak = create_weak;
  api.resolve_weak = resolve_weak;
  mock_binding(api.expected_world_class, api.world_class_bytes,
               reinterpret_cast<uintptr_t>(get_world_class));
  mock_binding(api.expected_mode_class, api.mode_class_bytes,
               reinterpret_cast<uintptr_t>(get_mode_class));
  mock_binding(api.expected_create_weak, api.create_weak_bytes,
               reinterpret_cast<uintptr_t>(create_weak));
  mock_binding(api.expected_resolve_weak, api.resolve_weak_bytes,
               reinterpret_cast<uintptr_t>(resolve_weak));
  api.engine_reference = api.expected_engine_reference = reinterpret_cast<uintptr_t>(engine_reference.data());
  api.context_lookup = api.expected_context_lookup = reinterpret_cast<uintptr_t>(context_lookup.data());
  api.game_context_creation = api.expected_game_context_creation = reinterpret_cast<uintptr_t>(game_creation.data());
  const auto delta = static_cast<intptr_t>(reinterpret_cast<uintptr_t>(&engine_pointer)) -
      static_cast<intptr_t>(api.engine_reference + 7);
  assert(delta >= INT32_MIN && delta <= INT32_MAX);
  engine_reference[0] = 0x48; engine_reference[1] = 0x8B; engine_reference[2] = 0x3D;
  put(engine_reference.data(), 3, static_cast<int32_t>(delta));
  std::memcpy(api.engine_reference_bytes.data(), engine_reference.data(), 7);
  api.context_lookup_bytes.fill(0);
  api.game_context_bytes.fill(0);

  Result result = discover(api);
  assert(result.status == Status::ready && result.world == world.data());
  assert(result.key == (Key{1, 7}));
  context[0] = 4;
  assert(discover(api).status == Status::not_ready);
  context[0] = 1;
  put(context.data(), 0x260, static_cast<void*>(nullptr));
  assert(discover(api).status == Status::not_ready);
  put(context.data(), 0x260, static_cast<void*>(world.data()));
  put(world.data(), 0x250, static_cast<void*>(nullptr));
  assert(discover(api).status == Status::not_ready);
  put(world.data(), 0x250, static_cast<void*>(mode.data()));
  weak_valid = false;
  assert(discover(api).status == Status::not_ready);
  weak_valid = true;
  api.game_thread_tid = -1;
  assert(discover(api).status == Status::invalid_layout);
  api.game_thread_tid = syscall(SYS_gettid);
  api.world_class_bytes[0] ^= 0xff;
  assert(discover(api).status == Status::invalid_layout);
  api.world_class_bytes[0] ^= 0xff;
  put<int32_t>(engine.data(), 0x928, 9);
  assert(discover(api).status == Status::invalid_layout);
  put<int32_t>(engine.data(), 0x928, 1);

  second_context[0] = 1;
  put(second_context.data(), 0x260, static_cast<void*>(second_world.data()));
  put(second_world.data(), 0x10, static_cast<void*>(derived_world_class.data()));
  put(second_world.data(), 0x250, static_cast<void*>(second_mode.data()));
  put(second_mode.data(), 0x10, static_cast<void*>(derived_mode_class.data()));
  put<int32_t>(second_world.data(), 0x0C, 2);
  put<int32_t>(second_world.data(), 0x490, 0);
  put<int32_t>(second_world.data(), 0x494, 16);
  put(object_items.data(), 32, static_cast<void*>(second_world.data()));
  put<int32_t>(engine.data(), 0x928, 2);
  assert(discover(api).status == Status::ambiguous);
}
