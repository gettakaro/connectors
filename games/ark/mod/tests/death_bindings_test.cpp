#include "../src/death_bindings.hpp"
#include <cassert>
#include <vector>

using namespace ark_death;

namespace {
alignas(8) uint8_t victim[0x900]{};
alignas(8) uint8_t attacker[16]{};
HookSupport* hook = nullptr;
bool reenter = false;
int calls = 0;
int captures = 0;

void original(void* object, void* context, void* killer, void* other, float damage) {
  ++calls;
  assert(object == victim && context == reinterpret_cast<void*>(0x1234) &&
         killer == attacker && other == reinterpret_cast<void*>(0x5678) && damage == 4.5f);
  if (reenter) { reenter = false; hook->invoke(object, context, killer, other, damage); }
  victim[0x8A8] |= 0x20;
}
}

int main() {
  assert(!prologue_matches(victim));
  assert(prologue_matches(kPrologue.data()));
  *reinterpret_cast<int32_t*>(victim + 0x0C) = 7;
  alignas(8) uint8_t items[16 * 8]{};
  *reinterpret_cast<void**>(items + 7 * 16) = victim;
  *reinterpret_cast<int32_t*>(items + 7 * 16 + 8) = 42;
  void* chunk_table[1]{items};
  void** chunk_table_pointer = chunk_table;
  int32_t count = 8;
  ObjectArray objects{reinterpret_cast<void* const* const*>(&chunk_table_pointer), &count};
  assert(object_key(victim, objects) == (ObjectKey{7, 42}));
  *reinterpret_cast<int32_t*>(items + 7 * 16 + 8) = 0;
  assert(!object_key(victim, objects));
  *reinterpret_cast<int32_t*>(items + 7 * 16 + 8) = 42;

  std::vector<Event> events;
  HookSupport support(original,
      [&](void* object, void* killer, ObjectKey key) -> std::optional<Event> {
        ++captures;
        assert(object == victim && killer == attacker);
        return Event{Type::player_death, "76561198000000000", "Survivor", "76561198000000001", key};
      },
      [&](const Event& event) { events.push_back(event); }, syscall(SYS_gettid), objects);
  hook = &support;
  reenter = true;
  support.invoke(victim, reinterpret_cast<void*>(0x1234), attacker, reinterpret_cast<void*>(0x5678), 4.5f);
  assert(calls == 2 && captures == 1 && events.size() == 1);
  assert(events[0].victim_id == "76561198000000000" &&
         events[0].victim_name == "Survivor" &&
         events[0].attacker_id == "76561198000000001" &&
         events[0].victim_key == (ObjectKey{7, 42}));

  support.invoke(victim, reinterpret_cast<void*>(0x1234), attacker, reinterpret_cast<void*>(0x5678), 4.5f);
  assert(calls == 3 && captures == 1 && events.size() == 1); // already dead
  victim[0x8A8] &= ~0x20;
  support.invoke(victim, reinterpret_cast<void*>(0x1234), attacker, reinterpret_cast<void*>(0x5678), 4.5f);
  assert(calls == 4 && captures == 1 && events.size() == 1); // same weak object
  *reinterpret_cast<int32_t*>(items + 7 * 16 + 8) = 43;
  victim[0x8A8] &= ~0x20;
  support.invoke(victim, reinterpret_cast<void*>(0x1234), attacker, reinterpret_cast<void*>(0x5678), 4.5f);
  assert(calls == 5 && captures == 2 && events.size() == 2); // serial changed

  victim[0x8A8] &= ~0x20;
  HookSupport wrong_thread(original, {}, {}, -1, objects);
  wrong_thread.invoke(victim, reinterpret_cast<void*>(0x1234), attacker, reinterpret_cast<void*>(0x5678), 4.5f);
  assert(calls == 6 && events.size() == 2);
}
