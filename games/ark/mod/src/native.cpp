// Exact-build ARK native gate. Game access and RPCs stay on the engine thread;
// authenticated loopback HTTP only reads copied snapshots and queues actions.
#include "action_bindings.hpp"
#include "console_bindings.hpp"
#include "catalog_bindings.hpp"
#include "moderation_bindings.hpp"
#include "death_bindings.hpp"
#include "entity_bindings.hpp"
#include "engine_exec_capture.hpp"
#include "general_console_bindings.hpp"
#include "gate_http.hpp"
#include "inventory_bindings.hpp"
#include "log_tail.hpp"
#include "save_bindings.hpp"
#include "shutdown_request_bindings.hpp"
#include "location_bindings.hpp"
#include "list_bans_bindings.hpp"
#include "world_bootstrap.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr char kExpectedHash[] = "7e7ded49e0c658e74199801d79630bd33da407d3e468e039837bf61a9de7c520";
constexpr uintptr_t kExecThunk = 0x154a600; // reflected ServerSendChatMessage, verified by derive_bindings.py
constexpr uintptr_t kTick = 0x81fce0; // only call at 0x8206ae in the main loop, build-specific
constexpr uintptr_t kLogout = 0xe72ab0; // ShooterGameMode Logout vtable +0xc18, before controller removal
constexpr uintptr_t kPostLogin = 0xe709e0; // ShooterGameMode PostLogin vtable +0xc10
constexpr uintptr_t kGameModeVtable = 0x4062fc0;
constexpr uintptr_t kGetUniqueIdString = 0xfcf060; // ShooterPlayerState reflected native method
constexpr uintptr_t kGetActorLocation = 0x2521760; // AActor::K2_GetActorLocation native, pawn only
constexpr uintptr_t kGiveItem = 0xf71e50; // ShooterPlayerController grant, bool means any item created
constexpr uintptr_t kTeleport = 0x25177f0; // AActor::K2_TeleportTo native, bool then verify position
constexpr uintptr_t kDeath = ark_death::kBaseDeath; // base PrimalCharacter OnDied path
constexpr uintptr_t kGameFree = 0x1af8e30; // same FString cleanup used by GetUniqueIdString exec thunk
constexpr uintptr_t kResolveWeakController = 0xea29d0; // UE weak object serial validation
constexpr uintptr_t kResolveWeakObject = 0x1d56c70; // generic FWeakObjectPtr Get, no class filter
constexpr uintptr_t kCreateWeakObject = 0x1d56970; // FWeakObjectPtr(UObject*) assigns serial if absent
constexpr uintptr_t kClientChatRpc = 0x136ec60; // generated ClientServerChatDirectMessage ProcessEvent wrapper
constexpr uintptr_t kTextStart = 0x648400;
constexpr uintptr_t kTextEnd = 0x3f6170f;
constexpr size_t kChatVtableOffset = 0x1360; // from thunk's call [vtable+0x1360]
constexpr unsigned char kExecPrologue[14] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53,
    0x48, 0x83, 0xec, 0x20, 0x48, 0x89, 0xf3,
};
constexpr unsigned char kTickPrologue[15] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x20,
};
constexpr unsigned char kLogoutPrologue[20] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0xc8, 0x02, 0x00, 0x00,
};
constexpr unsigned char kPostLoginPrologue[20] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
    0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
    0x48, 0x01, 0x00, 0x00,
};

struct FString {
  const wchar_t* data;
  int32_t count;
  int32_t capacity;
};
static_assert(sizeof(FString) == 16);
using ExecFn = void (*)(void*, void*, void*);
using TickFn = void (*)(void*);
using LogoutFn = void (*)(void*, void*);
using PostLoginFn = void (*)(void*, void*);
using DeathFn = ark_death::OriginalFn;
using ChatFn = void (*)(void*, const FString*, uint8_t);
struct LinearColor { float r, g, b, a; };
static_assert(sizeof(LinearColor) == 16);
struct FVector { float x, y, z; };
static_assert(sizeof(FVector) == 12);
using ClientChatFn = void (*)(void*, const FString*, LinearColor, uint8_t);
ExecFn g_original_exec = nullptr;
TickFn g_original_tick = nullptr;
LogoutFn g_original_logout = nullptr;
PostLoginFn g_original_postlogin = nullptr;
DeathFn g_original_death = nullptr;
ChatFn g_original_chat = nullptr;
void** g_chat_slot = nullptr;
void* g_trampoline = nullptr;
void* g_tick_trampoline = nullptr;
void* g_logout_trampoline = nullptr;
void* g_postlogin_trampoline = nullptr;
void* g_death_trampoline = nullptr;
bool g_exec_installed = false;
bool g_tick_installed = false;
bool g_logout_installed = false;
bool g_postlogin_installed = false;
bool g_death_installed = false;
unsigned char g_original_prologue[sizeof(kExecPrologue)]{};
unsigned char g_original_tick_prologue[sizeof(kTickPrologue)]{};
unsigned char g_original_logout_prologue[sizeof(kLogoutPrologue)]{};
unsigned char g_original_postlogin_prologue[sizeof(kPostLoginPrologue)]{};
unsigned char g_original_death_prologue[ark_death::kPrologue.size()]{};
struct State {
  std::mutex queue_mutex;
  std::vector<std::string> queue;
  std::thread worker;
};
State* g_state = nullptr; // allocated after the executable hash gate; intentionally freed by process exit
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_sequence{0};
std::atomic<bool> g_identity_attempted{false};
std::atomic<uint64_t> g_ticks{0};
gate::Server* g_gate = nullptr;
std::atomic<void*> g_latest_controller{nullptr};
std::atomic<void*> g_world{nullptr};
std::atomic<uint64_t> g_world_weak{0}; // validated UObject index/serial, not a lifetime claim
std::mutex g_latest_id_mutex;
std::string g_latest_id;
std::atomic<long> g_game_thread_tid{0};
std::atomic<bool> g_hooks_ready{false};
bool g_position_logged = false; // game-thread only
bool g_inventory_logged = false; // game-thread only
struct PendingKick {
  std::shared_ptr<gate::Action> action;
  std::chrono::steady_clock::time_point started;
};
std::vector<PendingKick> g_pending_kicks; // only touched on the verified game thread
struct PendingBan {
  std::shared_ptr<gate::Action> action;
  ark_moderation::BanStatus native_result;
  std::chrono::steady_clock::time_point started;
  std::chrono::steady_clock::time_point next_check;
  bool kick_attempted = false;
};
std::vector<PendingBan> g_pending_bans; // only touched on the verified game thread
std::shared_ptr<gate::Action> g_pending_shutdown; // game thread only; response_sent is atomic
std::chrono::steady_clock::time_point g_shutdown_staged_at{};

long thread_id() { return syscall(SYS_gettid); }

void enqueue(const char* label, const wchar_t* text = nullptr, size_t length = 0) {
  if (!g_state) return;
  char prefix[128];
  const int n = snprintf(prefix, sizeof(prefix), "ARK_NATIVE_DIAG seq=%llu tid=%ld ",
                         (unsigned long long)++g_sequence, thread_id());
  if (n < 0) return;
  const size_t written = static_cast<size_t>(n) < sizeof(prefix)
      ? static_cast<size_t>(n) : sizeof(prefix) - 1;
  std::string line(prefix, written);
  if (label) line.append(label, strnlen(label, 4096));
  line.push_back(' ');
  if (text && length <= 512) {
    for (size_t i = 0; i < length; ++i) {
      uint32_t c = static_cast<uint32_t>(text[i]);
      if (c == '\n' || c == '\r') c = ' ';
      if (c < 0x80) line.push_back(static_cast<char>(c));
      else if (c < 0x800) {
        line.push_back(static_cast<char>(0xc0 | (c >> 6)));
        line.push_back(static_cast<char>(0x80 | (c & 0x3f)));
      } else if (c <= 0xffff) {
        line.push_back(static_cast<char>(0xe0 | (c >> 12)));
        line.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        line.push_back(static_cast<char>(0x80 | (c & 0x3f)));
      }
    }
  }
  line.push_back('\n');
  std::lock_guard<std::mutex> lock(g_state->queue_mutex);
  if (g_state->queue.size() < 8192) g_state->queue.push_back(std::move(line));
}

void drain() {
  char executable[4096]{};
  const ssize_t path_size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  std::unique_ptr<ark_log_tail::Tail> log_tail;
  if (path_size > 0) {
    std::string path(executable, static_cast<size_t>(path_size));
    constexpr std::string_view suffix = "/Binaries/Linux/ShooterGameServer";
    if (path.size() > suffix.size() && path.ends_with(suffix)) {
      path.resize(path.size() - suffix.size());
      log_tail = std::make_unique<ark_log_tail::Tail>(path + "/Saved/Logs/ShooterGame.log");
      (void)log_tail->prime(); // historical lines before preload are never fresh events
    }
  }
  for (;;) {
    std::vector<std::string> batch;
    {
      std::lock_guard<std::mutex> lock(g_state->queue_mutex);
      batch.swap(g_state->queue);
    }
    for (const auto& s : batch) (void)write(STDERR_FILENO, s.data(), s.size());
    if (log_tail && g_gate) {
      const auto result = log_tail->poll();
      for (const auto& message : result.messages) {
        g_gate->record_log(message);
      }
      if (result.dropped) enqueue("native-safe-log-tail-dropped-lines");
      if (result.more) enqueue("native-safe-log-tail-backlog-more");
    }
    if (!g_running.load(std::memory_order_relaxed) && batch.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool writable(void* address, size_t length) {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  const auto page = reinterpret_cast<uintptr_t>(address) & ~(uintptr_t(page_size) - 1);
  const size_t page_count = ((reinterpret_cast<uintptr_t>(address) + length - page + page_size - 1) / page_size);
  return mprotect(reinterpret_cast<void*>(page), page_count * page_size,
                  PROT_READ | PROT_WRITE) == 0;
}

void readonly(void* address) {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return;
  const auto page = reinterpret_cast<uintptr_t>(address) & ~(uintptr_t(page_size) - 1);
  (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ);
}

void jump_absolute(unsigned char* at, void* destination) {
  // movabs rax, destination; jmp rax (12 bytes, no relative range restriction).
  at[0] = 0x48; at[1] = 0xb8;
  memcpy(at + 2, &destination, sizeof(destination));
  at[10] = 0xff; at[11] = 0xe0;
}

std::string steam_id(void* controller, std::string* name = nullptr) {
  if (!controller) return {};
  void* state = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x488);
  if (reinterpret_cast<uintptr_t>(state) < 0x10000 || (reinterpret_cast<uintptr_t>(state) & 7)) return {};
  void* online_id = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(state) + 0x4c8);
  if (reinterpret_cast<uintptr_t>(online_id) < 0x10000) return {};
  FString unique{};
  reinterpret_cast<void (*)(FString*, void*)>(kGetUniqueIdString)(&unique, state);
  std::string id;
  if (unique.data && unique.count > 0 && unique.count <= 128 &&
      unique.capacity >= unique.count && unique.capacity <= 1024) {
    size_t count = static_cast<size_t>(unique.count);
    if (unique.data[count - 1] == 0) --count;
    id = gate::utf32_to_utf8(unique.data, count);
    if (!g_identity_attempted.exchange(true)) enqueue("online-unique-id", unique.data, count);
  }
  if (unique.data) reinterpret_cast<void (*)(const void*)>(kGameFree)(unique.data);
  if (id.size() != 17 || id.rfind("7656", 0) != 0 ||
      id.find_first_not_of("0123456789") != std::string::npos) return {};
  if (name) {
    const auto* player_name = reinterpret_cast<const FString*>(reinterpret_cast<uintptr_t>(state) + 0x478);
    if (player_name->data && player_name->count > 0 && player_name->count <= 128 &&
        player_name->capacity >= player_name->count && player_name->capacity <= 1024) {
      *name = gate::utf32_to_utf8(player_name->data, static_cast<size_t>(player_name->count));
    }
    if (name->empty()) *name = id;
  }
  return id;
}

void* resolve_weak_object(ark_death::ObjectKey key) {
  if (key.index < 0 || key.serial <= 0) return nullptr;
  constexpr unsigned char resolve_prologue[] = {0x8b, 0x4f, 0x04, 0x31, 0xc0, 0x85, 0xc9, 0x74, 0x7a, 0x8b, 0x17};
  if (memcmp(reinterpret_cast<const void*>(kResolveWeakObject), resolve_prologue,
             sizeof(resolve_prologue)) != 0) return nullptr;
  return reinterpret_cast<void* (*)(const ark_death::ObjectKey*, bool)>(kResolveWeakObject)(&key, false);
}

void* resolve_live_world() {
  const uint64_t packed = g_world_weak.load(std::memory_order_acquire);
  if (!packed) return nullptr;
  const ark_death::ObjectKey key{static_cast<int32_t>(packed >> 32),
                                  static_cast<int32_t>(packed & 0xffffffffu)};
  void* world = resolve_weak_object(key);
  return world == g_world.load(std::memory_order_acquire) ? world : nullptr;
}

void* resolve_live_controller(const std::string& id) {
  void* world = resolve_live_world();
  if (!world || id.empty()) return nullptr;
  const uintptr_t base = reinterpret_cast<uintptr_t>(world) + 0x488;
  auto* entries = *reinterpret_cast<void**>(base);
  const int32_t count = *reinterpret_cast<int32_t*>(base + 8);
  const int32_t capacity = *reinterpret_cast<int32_t*>(base + 12);
  if (!entries || count < 0 || count > 128 || capacity < count || capacity > 4096) return nullptr;
  auto resolve = reinterpret_cast<void* (*)(const void*)>(kResolveWeakController);
  for (int32_t i = 0; i < count; ++i) {
    void* live = resolve(reinterpret_cast<const char*>(entries) + static_cast<size_t>(i) * 8);
    if (live && steam_id(live) == id) return live;
  }
  return nullptr;
}

void capture_world(void* controller) {
  if (!controller || resolve_live_world()) return;
  auto* vtable = *reinterpret_cast<void***>(controller);
  if (!vtable) return;
  const uintptr_t fn = reinterpret_cast<uintptr_t>(vtable[0x110 / sizeof(void*)]);
  if (fn < kTextStart || fn >= kTextEnd) return;
  void* world = reinterpret_cast<void* (*)(void*)>(fn)(controller);
  if (reinterpret_cast<uintptr_t>(world) < 0x10000) return;
  const uintptr_t base = reinterpret_cast<uintptr_t>(world) + 0x488;
  const int32_t count = *reinterpret_cast<int32_t*>(base + 8);
  const int32_t capacity = *reinterpret_cast<int32_t*>(base + 12);
  if (count < 0 || count > 128 || capacity < count || capacity > 4096) return;
  constexpr unsigned char weak_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
  if (memcmp(reinterpret_cast<const void*>(kCreateWeakObject), weak_prologue, sizeof(weak_prologue)) != 0) return;
  ark_death::ObjectKey created{-1, 0};
  reinterpret_cast<void (*)(ark_death::ObjectKey*, void*)>(kCreateWeakObject)(&created, world);
  const auto key = ark_death::object_key(world);
  if (!key || *key != created) return;
  if (resolve_weak_object(created) != world) return;
  g_world.store(world, std::memory_order_release);
  g_world_weak.store(ark_death::packed(*key), std::memory_order_release);
  enqueue("native-world-weak-roster-captured");
}

void chat_hook(void* controller, const FString* message, uint8_t flag) {
  if (!g_hooks_ready.load(std::memory_order_acquire) ||
      thread_id() != g_game_thread_tid.load(std::memory_order_acquire)) {
    enqueue("player-chat-wrong-thread-or-incomplete-hooks");
    g_original_chat(controller, message, flag);
    return;
  }
  std::string chat;
  if (message && message->data && message->count > 0 && message->count <= 512 &&
      message->capacity >= message->count && message->capacity <= 65536) {
    size_t count = static_cast<size_t>(message->count);
    if (message->data[count - 1] == 0) --count;
    enqueue("player-chat", message->data, count);
    chat = gate::utf32_to_utf8(message->data, count);
  } else {
    enqueue("player-chat-invalid-layout");
  }
  g_original_chat(controller, message, flag);
  if (controller) {
    std::string name;
    const std::string id = steam_id(controller, &name);
    if (!id.empty()) {
      { std::lock_guard<std::mutex> lock(g_latest_id_mutex); g_latest_id = id; }
      g_latest_controller.store(controller, std::memory_order_release);
      capture_world(controller);
      if (g_gate && !chat.empty()) g_gate->record_chat(id, name, chat);
    } else enqueue("online-unique-id-invalid-layout");
  }
}

void postlogin_hook(void* game_mode, void* controller) {
  g_original_postlogin(game_mode, controller);
  if (!controller || !g_hooks_ready.load(std::memory_order_acquire) ||
      thread_id() != g_game_thread_tid.load(std::memory_order_acquire)) return;
  std::string name;
  const std::string id = steam_id(controller, &name);
  if (id.empty()) { enqueue("native-postlogin-no-steam-id"); return; }
  { std::lock_guard<std::mutex> lock(g_latest_id_mutex); g_latest_id = id; }
  g_latest_controller.store(controller, std::memory_order_release);
  capture_world(controller);
  if (g_gate) g_gate->record_login(id, name);
  enqueue("native-postlogin-player-registered");
}

void logout_hook(void* game_mode, void* controller) {
  if (!controller || !g_hooks_ready.load(std::memory_order_acquire) ||
      thread_id() != g_game_thread_tid.load(std::memory_order_acquire)) {
    g_original_logout(game_mode, controller);
    return;
  }
  if (controller) {
    // Logout runs before the engine releases this actor. Never retain or use it afterwards.
    const std::string id = steam_id(controller);
    if (g_gate && !id.empty()) {
      void* pawn = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x490);
      if (reinterpret_cast<uintptr_t>(pawn) >= 0x10000 &&
          (reinterpret_cast<uintptr_t>(pawn) & 7) == 0) {
        const FVector final_position = reinterpret_cast<FVector (*)(void*)>(kGetActorLocation)(pawn);
        g_gate->record_departure_position(id, final_position.x, final_position.y, final_position.z);
      }
    }
    if (g_gate && !id.empty()) g_gate->record_logout(id);
    if (g_latest_controller.load(std::memory_order_acquire) == controller) {
      g_latest_controller.store(nullptr, std::memory_order_release);
      std::lock_guard<std::mutex> lock(g_latest_id_mutex);
      g_latest_id.clear();
    }
    // The world/GameMode survives a player's Logout on this single-map
    // dedicated server. Keep only that map pointer for offline unban; every
    // moderation call still validates the exact ShooterGameMode class on the
    // verified game thread. No controller or pawn pointer survives Logout.
    enqueue(id.empty() ? "native-player-logout-no-id" : "native-player-logout-invalidate");
  }
  g_original_logout(game_mode, controller);
}

void death_hook(void* victim, void* context, void* killer_character, void* other, float damage) {
  const long tid = g_game_thread_tid.load(std::memory_order_acquire);
  if (!g_original_death) return;
  if (!g_hooks_ready.load(std::memory_order_acquire) || tid <= 0 || thread_id() != tid || !g_gate) {
    g_original_death(victim, context, killer_character, other, damage);
    return;
  }
  // Newly spawned actors need not have a weak serial yet. Create and verify
  // one while the victim is live, before the death helper records its key.
  constexpr unsigned char weak_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
  if (ark_death::plausible(victim) &&
      memcmp(reinterpret_cast<const void*>(kCreateWeakObject), weak_prologue,
             sizeof(weak_prologue)) == 0) {
    ark_death::ObjectKey created{-1, 0};
    reinterpret_cast<void (*)(ark_death::ObjectKey*, void*)>(kCreateWeakObject)(&created, victim);
    if (!ark_death::object_key(victim) || *ark_death::object_key(victim) != created)
      enqueue("native-death-victim-weak-key-unverified");
  }
  // This state is created only on the verified game thread and lives until
  // process exit; the detour is removed at shutdown before its trampoline.
  static auto* support = new ark_death::HookSupport(
      g_original_death,
      [](void* current_victim, void* killer, ark_death::ObjectKey key)
          -> std::optional<ark_death::Event> {
        if (!g_gate) return std::nullopt;
        std::string victim_id, victim_name, attacker_id;
        for (const auto& id : g_gate->player_ids()) {
          void* controller = resolve_live_controller(id);
          if (!controller) continue;
          void* pawn = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x490);
          if (pawn == current_victim) {
            victim_id = steam_id(controller, &victim_name);
          } else if (pawn == killer && killer) {
            attacker_id = steam_id(controller);
          }
        }
        if (!victim_id.empty()) {
          const FVector location = reinterpret_cast<FVector (*)(void*)>(kGetActorLocation)(current_victim);
          g_gate->record_death_position(victim_id, location.x, location.y, location.z,
                                        key.index, key.serial);
          return ark_death::Event{ark_death::Type::player_death,
                                  std::move(victim_id), std::move(victim_name),
                                  std::move(attacker_id), key};
        }
        if (attacker_id.empty()) return std::nullopt;
        ark_entity::Api api{};
        api.game_thread_tid = g_game_thread_tid.load(std::memory_order_acquire);
        std::string code;
        if (ark_entity::actor_code(current_victim, code, api) != ark_entity::Status::ok)
          return std::nullopt;
        std::string entity_name = g_gate->entity_name_for_code(code);
        if (entity_name.empty()) return std::nullopt;
        return ark_death::Event{ark_death::Type::entity_killed,
                                std::move(code), std::move(entity_name),
                                std::move(attacker_id), key};
      },
      [](const ark_death::Event& event) {
        if (g_gate && event.type == ark_death::Type::player_death) {
          g_gate->record_player_death(event.victim_id, event.victim_name, event.attacker_id);
          enqueue("native-player-death-on-died-verified");
        } else if (g_gate && event.type == ark_death::Type::entity_killed) {
          g_gate->record_entity_killed(event.attacker_id, event.victim_id, event.victim_name);
          enqueue("native-entity-killed-on-died-verified");
        }
      }, tid);
  support->invoke(victim, context, killer_character, other, damage);
}

bool send_queued_rpc(void* controller, const std::u32string& text) {
  if (!controller || text.empty() || text.size() > 512) return false;
    const uintptr_t candidate = kClientChatRpc;
    constexpr uint8_t rpc_prologue[] = {0x55, 0x48, 0x89, 0xe5};
    if (candidate >= kTextStart && candidate < kTextEnd &&
        memcmp(reinterpret_cast<const void*>(candidate), rpc_prologue, sizeof(rpc_prologue)) == 0) {
      static_assert(sizeof(wchar_t) == sizeof(char32_t));
      FString outgoing{reinterpret_cast<const wchar_t*>(text.c_str()),
                       static_cast<int32_t>(text.size() + 1), static_cast<int32_t>(text.size() + 1)};
      auto send = reinterpret_cast<ClientChatFn>(candidate);
      enqueue("outbound-direct-rpc-call-start");
      send(controller, &outgoing, LinearColor{1.f, 1.f, 0.f, 1.f}, 0);
      enqueue("outbound-direct-rpc-call-returned");
      return true;
    }
    enqueue("outbound-direct-rpc-call-rejected");
    return false;
}

// Item grants can split across stacks. Compare the sum for the exact native
// class path, never the number of rows or the native "any item" boolean.
int64_t inventory_quantity(const std::vector<ark_inventory::Item>& items,
                           const std::string& code) {
  const std::string path = code.rfind("Blueprint'", 0) == 0 && code.back() == '\''
      ? code.substr(10, code.size() - 11) : code;
  int64_t total = 0;
  for (const auto& item : items) {
    if (item.code != path || item.amount < 1) continue;
    if (total > std::numeric_limits<int64_t>::max() - item.amount) return -1;
    total += item.amount;
  }
  return total;
}

void publish_inventory(const std::string& id, std::vector<ark_inventory::Item> items) {
  if (!g_gate) return;
  std::vector<gate::InventoryItem> copied;
  copied.reserve(items.size());
  for (auto& item : items)
    copied.push_back(gate::InventoryItem{std::move(item.code), std::move(item.name), item.amount});
  g_gate->record_inventory(id, std::move(copied));
}

void exec_hook(void* controller, void* frame, void* result) {
  if (!g_original_chat && controller) {
    auto* vtable = *reinterpret_cast<void***>(controller);
    if (vtable) {
      void** slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(vtable) + kChatVtableOffset);
      const uintptr_t original = reinterpret_cast<uintptr_t>(*slot);
      if (original >= kTextStart && original < kTextEnd && writable(slot, sizeof(void*))) {
        g_original_chat = reinterpret_cast<ChatFn>(original);
        g_chat_slot = slot;
        __atomic_store_n(slot, reinterpret_cast<void*>(&chat_hook), __ATOMIC_RELEASE);
        readonly(slot);
        if (g_gate) g_gate->set_chat_ready();
        enqueue("chat-vtable-installed");
      } else enqueue("chat-vtable-rejected");
    }
  }
  g_original_exec(controller, frame, result);
}

void tick_hook(void* loop) {
  g_original_tick(loop);
  const long tid = thread_id();
  long zero = 0;
  g_game_thread_tid.compare_exchange_strong(zero, tid, std::memory_order_acq_rel);
  if (g_game_thread_tid.load(std::memory_order_acquire) != tid) {
    enqueue("main-loop-tick-wrong-thread");
    return;
  }
  static auto next_world_probe = std::chrono::steady_clock::time_point{};
  const auto world_probe_now = std::chrono::steady_clock::now();
  if (!resolve_live_world() && world_probe_now >= next_world_probe) {
    next_world_probe = world_probe_now + std::chrono::seconds(1);
    ark_world_bootstrap::Api api{};
    api.game_thread_tid = tid;
    const auto found = ark_world_bootstrap::discover(api);
    if (found.status == ark_world_bootstrap::Status::ready) {
      g_world.store(found.world, std::memory_order_release);
      g_world_weak.store(ark_death::packed(found.key), std::memory_order_release);
      enqueue("native-world-bootstrapped-before-player");
    }
  }
  if (g_pending_shutdown) {
    const auto now = std::chrono::steady_clock::now();
    if (g_pending_shutdown->response_sent.load(std::memory_order_acquire) &&
        now - g_shutdown_staged_at >= std::chrono::milliseconds(200)) {
      ark_shutdown_request::Api api{};
      api.game_thread_tid = tid;
      const auto outcome = ark_shutdown_request::request_after_ack(
          true, g_pending_shutdown->response_sent.load(std::memory_order_acquire), api);
      const char* marker = outcome == ark_shutdown_request::Status::requested
          ? "ARK_NATIVE_SHUTDOWN native-exit-requested\n"
          : "ARK_NATIVE_SHUTDOWN native-exit-request-rejected\n";
      (void)write(STDERR_FILENO, marker, strlen(marker));
      g_pending_shutdown.reset();
      return;
    } else if (now - g_shutdown_staged_at > std::chrono::seconds(5)) {
      enqueue("native-shutdown-ack-not-flushed-or-expired");
      g_pending_shutdown.reset();
      return;
    }
    // The saved checkpoint precedes the HTTP ACK. Do not run another queued
    // connector mutation while the acknowledged exit is pending.
    return;
  }
  if (g_gate && g_hooks_ready.load(std::memory_order_acquire)) {
    g_gate->set_game_ready();
    static auto next_position_sample = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_position_sample) {
      next_position_sample = now + std::chrono::milliseconds(250);
      for (const std::string& id : g_gate->player_ids()) {
        void* controller = resolve_live_controller(id);
        void* pawn = controller
            ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x490) : nullptr;
        if (reinterpret_cast<uintptr_t>(pawn) >= 0x10000 &&
            (reinterpret_cast<uintptr_t>(pawn) & 7) == 0) {
          const FVector position = reinterpret_cast<FVector (*)(void*)>(kGetActorLocation)(pawn);
          if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
              std::abs(position.x) < 1e9f && std::abs(position.y) < 1e9f &&
              std::abs(position.z) < 1e9f) {
            ark_death::ObjectKey pawn_key{-1, 0};
            if (auto existing = ark_death::object_key(pawn)) {
              pawn_key = *existing;
            } else {
              constexpr unsigned char weak_prologue[] =
                  {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
              if (memcmp(reinterpret_cast<const void*>(kCreateWeakObject), weak_prologue,
                         sizeof(weak_prologue)) == 0) {
                ark_death::ObjectKey created{-1, 0};
                reinterpret_cast<void (*)(ark_death::ObjectKey*, void*)>(kCreateWeakObject)(&created, pawn);
                if (auto verified = ark_death::object_key(pawn); verified && *verified == created)
                  pawn_key = created;
              }
            }
            g_gate->record_position(id, position.x, position.y, position.z,
                                    pawn_key.index, pawn_key.serial);
            if (!g_position_logged) {
              char label[160];
              snprintf(label, sizeof(label), "native-pawn-location x=%.3f y=%.3f z=%.3f",
                       static_cast<double>(position.x), static_cast<double>(position.y),
                       static_cast<double>(position.z));
              enqueue(label);
              g_position_logged = true;
            }
            continue;
          }
        }
        g_gate->clear_position(id);
      }
    }
    // Enumerate the exact build's loaded MasterItemList in bounded pages.
    // No asset is loaded here; an absent class default object leaves the
    // catalog unavailable rather than returning a partial platform list.
    static ark_catalog::Identity catalog_identity{};
    static std::vector<gate::CatalogItem> catalog_items;
    static int32_t catalog_offset = 0;
    static bool catalog_published = false;
    static auto catalog_retry_at = std::chrono::steady_clock::time_point{};
    if (!catalog_published && now >= catalog_retry_at) {
      ark_catalog::Api api{};
      api.game_thread_tid = tid;
      ark_catalog::NamedPage page;
      const auto* expected = catalog_offset ? &catalog_identity : nullptr;
      const auto result = ark_catalog::named_page(catalog_offset, 128, page, api,
          reinterpret_cast<ark_inventory::FString* (*)(ark_inventory::FString*, void*)>(0xB8CF40),
          expected);
      if (result == ark_catalog::Status::ok) {
        if (!catalog_offset) catalog_identity = page.identity;
        for (auto& item : page.items)
          catalog_items.push_back({std::move(item.code), std::move(item.name)});
        catalog_offset += std::min<int32_t>(128, page.identity.count - catalog_offset);
        if (catalog_offset == page.identity.count) {
          std::sort(catalog_items.begin(), catalog_items.end(),
              [](const auto& left, const auto& right) { return left.code < right.code; });
          catalog_items.erase(std::unique(catalog_items.begin(), catalog_items.end(),
              [](const auto& left, const auto& right) { return left.code == right.code; }),
              catalog_items.end());
          g_gate->record_catalog(std::move(catalog_items));
          catalog_published = true;
          enqueue("native-masteritemlist-named-snapshot-ready");
        }
      } else {
        g_gate->clear_catalog();
        catalog_items.clear();
        catalog_offset = 0;
        catalog_identity = {};
        catalog_retry_at = now + std::chrono::seconds(10);
      }
    }
    static ark_entity::Identity entity_identity{};
    static std::vector<gate::CatalogItem> entity_items;
    static int32_t entity_offset = 0;
    static bool entities_published = false;
    static auto entity_retry_at = std::chrono::steady_clock::time_point{};
    if (!entities_published && now >= entity_retry_at) {
      ark_entity::Api api{};
      api.game_thread_tid = tid;
      ark_entity::Page page;
      const auto* expected = entity_offset ? &entity_identity : nullptr;
      const auto result = ark_entity::page(entity_offset, 128, page, api, expected);
      if (result == ark_entity::Status::ok) {
        if (!entity_offset) entity_identity = page.identity;
        for (auto& entity : page.entities)
          entity_items.push_back({std::move(entity.code), std::move(entity.name)});
        entity_offset += std::min<int32_t>(128, page.identity.count - entity_offset);
        if (entity_offset == page.identity.count) {
          std::sort(entity_items.begin(), entity_items.end(),
              [](const auto& left, const auto& right) { return left.code < right.code; });
          const auto duplicate = std::adjacent_find(entity_items.begin(), entity_items.end(),
              [](const auto& left, const auto& right) { return left.code == right.code; });
          if (duplicate == entity_items.end()) {
            g_gate->record_entities(std::move(entity_items));
            entities_published = true;
            enqueue("native-dinoentries-named-snapshot-ready");
          } else {
            g_gate->clear_entities();
            entity_items.clear();
            entity_offset = 0;
            entity_retry_at = now + std::chrono::seconds(10);
          }
        }
      } else {
        g_gate->clear_entities();
        entity_items.clear();
        entity_offset = 0;
        entity_identity = {};
        entity_retry_at = now + std::chrono::seconds(10);
      }
    }
    static bool locations_published = false;
    static auto location_retry_at = std::chrono::steady_clock::time_point{};
    if (!locations_published && now >= location_retry_at) {
      ark_location::Api api{};
      api.game_thread_tid = tid;
      std::vector<ark_location::Location> locations;
      if (ark_location::snapshot(resolve_live_world(), locations, api) == ark_location::Status::ok) {
        std::vector<gate::LocationItem> copied;
        copied.reserve(locations.size());
        for (auto& location : locations)
          copied.push_back({std::move(location.code), std::move(location.name),
                            location.position.x, location.position.y, location.position.z,
                            location.size.x, location.size.y, location.size.z});
        g_gate->record_locations(std::move(copied));
        locations_published = true;
        enqueue("native-playerstart-location-snapshot-ready");
      } else {
        g_gate->clear_locations();
        location_retry_at = now + std::chrono::seconds(10);
      }
    }
    static auto next_ban_sample = std::chrono::steady_clock::time_point{};
    if (now >= next_ban_sample) {
      next_ban_sample = now + std::chrono::seconds(2);
      ark_list_bans::Api api{};
      api.game_thread_tid = tid;
      std::vector<std::string> ids;
      if (ark_list_bans::snapshot(resolve_live_world(), ids, api) == ark_list_bans::Status::ok)
        g_gate->record_bans(std::move(ids));
      else
        g_gate->clear_bans();
    }
    static auto next_inventory_sample = std::chrono::steady_clock::time_point{};
    if (now >= next_inventory_sample) {
      next_inventory_sample = now + std::chrono::seconds(1);
      for (const std::string& id : g_gate->player_ids()) {
        void* controller = resolve_live_controller(id);
        if (!controller) { g_gate->clear_inventory(id); continue; }
        std::vector<ark_inventory::Item> items;
        const auto status = ark_inventory::snapshot(controller, items);
        if (status != ark_inventory::Status::ok) {
          g_gate->clear_inventory(id);
          if (!g_inventory_logged) enqueue("native-inventory-unavailable-or-unverified");
          continue;
        }
        if (!g_inventory_logged) {
          char label[100];
          snprintf(label, sizeof(label), "native-physical-inventory-snapshot item-count=%zu", items.size());
          enqueue(label);
          g_inventory_logged = true;
        }
        publish_inventory(id, std::move(items));
      }
    }
    for (auto it = g_pending_kicks.begin(); it != g_pending_kicks.end();) {
      const bool absent = !resolve_live_controller(it->action->player_id);
      const bool expired = std::chrono::steady_clock::now() - it->started >
          std::chrono::milliseconds(2500);
      if (absent || expired) {
        gate::Server::complete(it->action, absent);
        enqueue(absent ? "native-kick-roster-verified" : "native-kick-effect-unverified");
        it = g_pending_kicks.erase(it);
      } else ++it;
    }
    for (auto it = g_pending_bans.begin(); it != g_pending_bans.end();) {
      const auto checked_at = std::chrono::steady_clock::now();
      if (checked_at < it->next_check) { ++it; continue; }
      it->next_check = checked_at + std::chrono::milliseconds(100);
      ark_list_bans::Api list_api{};
      list_api.game_thread_tid = tid;
      std::vector<std::string> ids;
      const bool snapshot_ok = ark_list_bans::snapshot(resolve_live_world(), ids, list_api) ==
          ark_list_bans::Status::ok;
      const bool online = resolve_live_controller(it->action->player_id) != nullptr;
      const auto effect = snapshot_ok
          ? ark_moderation::ban_effect(it->native_result, true, ids, it->action->player_id, online)
          : ark_moderation::BanEffect::invalid;
      if (snapshot_ok) g_gate->record_bans(std::move(ids));
      else g_gate->clear_bans();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          checked_at - it->started).count();
      const auto progress = ark_moderation::ban_progress(effect, elapsed_ms, it->kick_attempted);
      if (progress == ark_moderation::BanProgress::attempt_kick) {
        it->kick_attempted = true;
        ark_moderation::Api api{};
        api.game_thread_tid = tid;
        const auto result = ark_moderation::kick(resolve_live_world(), it->action->player_id, api);
        enqueue(result == ark_moderation::Status::dispatched
            ? "native-ban-online-kick-dispatched" : "native-ban-online-kick-unavailable");
      }
      if (progress == ark_moderation::BanProgress::confirm ||
          progress == ark_moderation::BanProgress::reject) {
        const bool verified = progress == ark_moderation::BanProgress::confirm;
        gate::Server::complete(it->action, verified);
        enqueue(verified ? "native-ban-set-and-departure-verified" : "native-ban-effect-unverified");
        it = g_pending_bans.erase(it);
      } else ++it;
    }
    if (auto action = g_gate->take_action()) {
      bool execute = false;
      { std::lock_guard<std::mutex> lock(action->mutex);
        if (!action->cancelled) { action->executing = true; execute = true; }
      }
      if (execute) {
        bool success = false;
        bool pending = false;
        if (action->kind == gate::Action::Kind::shutdown) {
          if (!g_pending_shutdown && ark_shutdown_request::ready_to_stage()) {
            ark_save::Api api{};
            api.game_thread_tid = tid;
            const auto outcome = ark_save::save_world(resolve_live_world(), api);
            success = outcome == ark_save::Status::completed;
            if (success) {
              g_pending_shutdown = action;
              g_shutdown_staged_at = std::chrono::steady_clock::now();
              enqueue("native-shutdown-synchronous-save-completed-before-ack");
            } else enqueue(outcome == ark_save::Status::guarded
                ? "native-shutdown-saveworld-guarded" : "native-shutdown-saveworld-unavailable");
          }
        } else if (action->kind == gate::Action::Kind::message) {
          if (!action->player_id.empty()) {
            // Resolve at dispatch time. Never fall back to broadcast when the
            // intended recipient has disconnected or respawned.
            void* controller = resolve_live_controller(action->player_id);
            success = controller && send_queued_rpc(controller, action->text);
          } else {
            success = true;
            const auto ids = g_gate->player_ids();
            for (const auto& id : ids) {
              void* controller = resolve_live_controller(id);
              if (!send_queued_rpc(controller, action->text)) success = false;
            }
          }
        } else if (action->kind == gate::Action::Kind::console) {
          void* world = resolve_live_world();
          const uint64_t packed = g_world_weak.load(std::memory_order_acquire);
          ark_general_console::Status console_status = ark_general_console::Status::rejected;
          if (ark_shutdown_request::is_termination_command(action->command)) {
            enqueue("native-console-termination-requires-shutdown-action");
          } else if (ark_shutdown_request::contains_unicode_space(action->command)) {
            enqueue("native-console-unicode-space-rejected");
          } else if (ark_save::is_console_save_world(action->command)) {
            // Dedicated-server Engine Exec reports this command unhandled.
            // The exact GameMode binding returns only after native SaveWorld
            // completes; keep rawResult empty because no console text was captured.
            ark_save::Api api{};
            api.game_thread_tid = tid;
            const auto result = world && packed
                ? ark_save::save_world(world, api) : ark_save::Status::invalid;
            success = result == ark_save::Status::completed;
            console_status = success ? ark_general_console::Status::handled
                                     : ark_general_console::Status::rejected;
            enqueue(success ? "native-console-saveworld-synchronous-completed"
                            : result == ark_save::Status::guarded
                            ? "native-console-saveworld-guarded"
                            : "native-console-saveworld-unavailable");
          } else if (world && packed) {
            ark_general_console::Api api{};
            api.game_thread_tid = tid;
            const ark_engine_exec::WeakWorld key{
                static_cast<int32_t>(packed >> 32), static_cast<int32_t>(packed & 0xffffffffu)};
            const auto result = ark_general_console::execute(world, key, action->command, api);
            console_status = result.status;
            action->output = result.output;
            success = result.status == ark_general_console::Status::handled;
          }
          const char* status_name = console_status == ark_general_console::Status::handled
              ? "handled" : console_status == ark_general_console::Status::unhandled
              ? "unhandled" : console_status == ark_general_console::Status::output_truncated
              ? "output-truncated" : "invalid";
          char console_label[192];
          std::snprintf(console_label, sizeof(console_label),
              "native-console-dispatch status=%s output-bytes=%zu",
              status_name, action->output.size());
          enqueue(console_label);
          enqueue(success ? "native-console-handled-effect-unverified" : "native-console-unhandled-or-rejected");
        } else if (action->kind == gate::Action::Kind::kick ||
                   action->kind == gate::Action::Kind::ban ||
                   action->kind == gate::Action::Kind::unban) {
          ark_moderation::Api api{};
          api.game_thread_tid = tid;
          void* world = resolve_live_world();
          if (action->kind == gate::Action::Kind::kick) {
            if (resolve_live_controller(action->player_id)) {
              const auto result = ark_moderation::kick(world, action->player_id, api);
              if (result == ark_moderation::Status::dispatched) {
                g_pending_kicks.push_back({action, std::chrono::steady_clock::now()});
                pending = true;
              }
            }
            if (!pending) enqueue("native-kick-dispatch-rejected");
          } else {
            const bool should_ban = action->kind == gate::Action::Kind::ban;
            const auto result = ark_moderation::change_ban(world, action->player_id,
                should_ban, api);
            ark_list_bans::Api list_api{};
            list_api.game_thread_tid = tid;
            std::vector<std::string> ids;
            if (ark_list_bans::snapshot(resolve_live_world(), ids, list_api) == ark_list_bans::Status::ok) {
              const auto effect = ark_moderation::ban_effect(result, should_ban, ids,
                  action->player_id, resolve_live_controller(action->player_id) != nullptr);
              success = effect == ark_moderation::BanEffect::verified;
              if (effect == ark_moderation::BanEffect::pending_departure &&
                  g_pending_bans.size() < 128) {
                const auto started = std::chrono::steady_clock::now();
                g_pending_bans.push_back({action, result, started, started});
                pending = true;
              }
              g_gate->record_bans(std::move(ids));
            } else {
              g_gate->clear_bans();
            }
            enqueue(success ? "native-ban-memory-state-verified" : pending
                ? "native-ban-awaiting-departure" : "native-ban-memory-state-unverified");
          }
        } else {
          void* controller = resolve_live_controller(action->player_id);
          void* pawn = controller
              ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(controller) + 0x490) : nullptr;
          if (controller && pawn) {
            ark_actions::Api api{};
            api.game_thread_tid = tid;
            if (action->kind == gate::Action::Kind::give_item) {
              std::vector<ark_inventory::Item> before, after;
              if (ark_inventory::snapshot(controller, before) == ark_inventory::Status::ok) {
                const int64_t prior = inventory_quantity(before, action->item_code);
                const auto dispatched = ark_actions::dispatch_give_item(controller, pawn,
                    action->item_code, action->amount, action->quality, action->blueprint, api);
                const bool after_ok = ark_inventory::snapshot(controller, after) == ark_inventory::Status::ok;
                if (after_ok) {
                  const int64_t current = inventory_quantity(after, action->item_code);
                  success = dispatched && *dispatched && prior >= 0 && current >= prior &&
                      current - prior == action->amount;
                  publish_inventory(action->player_id, std::move(after));
                } else g_gate->clear_inventory(action->player_id);
                enqueue(success ? "native-give-item-quantity-verified" : "native-give-item-unverified-or-rejected");
              } else { g_gate->clear_inventory(action->player_id); enqueue("native-give-item-preinventory-unavailable"); }
            } else if (action->kind == gate::Action::Kind::teleport) {
              const ark_actions::FVector target{action->x, action->y, action->z};
              const auto outcome = ark_actions::teleport_player(controller, pawn, target, api);
              success = outcome == ark_actions::Result::verified;
              if (outcome != ark_actions::Result::invalid) {
                const FVector actual = reinterpret_cast<FVector (*)(void*)>(kGetActorLocation)(pawn);
                if (std::isfinite(actual.x) && std::isfinite(actual.y) && std::isfinite(actual.z))
                  g_gate->record_position(action->player_id, actual.x, actual.y, actual.z);
                else g_gate->clear_position(action->player_id);
              }
              enqueue(success ? "native-teleport-position-verified" : "native-teleport-unverified-or-rejected");
            }
          }
        }
        if (!pending) gate::Server::complete(action, success);
      }
    }
  }
  const uint64_t count = ++g_ticks;
  if (count == 1 || count == 1000) {
    char label[80];
    snprintf(label, sizeof(label), "main-loop-tick count=%llu", (unsigned long long)count);
    enqueue(label);
  }
}

bool exact_build() {
  char path[4096];
  const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n < 0) return false;
  path[n] = 0;
  const char* base = strrchr(path, '/');
  if (!base || strcmp(base + 1, "ShooterGameServer") != 0) return false;
  const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size != 86269536) { close(fd); return false; }
  void* bytes = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (bytes == MAP_FAILED) return false;
  void* crypto = dlopen("libcrypto.so.3", RTLD_NOW | RTLD_LOCAL);
  if (!crypto) { munmap(bytes, st.st_size); return false; }
  using Sha256 = unsigned char* (*)(const unsigned char*, size_t, unsigned char*);
  auto sha256 = reinterpret_cast<Sha256>(dlsym(crypto, "SHA256"));
  unsigned char digest[32];
  bool okay = sha256 && sha256(reinterpret_cast<const unsigned char*>(bytes), st.st_size, digest);
  munmap(bytes, st.st_size);
  dlclose(crypto);
  if (!okay) return false;
  char hex[65];
  for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  return strcmp(hex, kExpectedHash) == 0;
}

bool preflight_bindings() {
  constexpr uint8_t rpc_prologue[] = {0x55, 0x48, 0x89, 0xe5};
  constexpr uint8_t location_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf8};
  constexpr uint8_t inventory_getter[] = {0x48, 0x8b, 0x8f, 0x90, 0x04, 0x00, 0x00};
  constexpr uint8_t short_name_getter[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50};
  constexpr uint8_t class_path_getter[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50};
  constexpr uint8_t give_item_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec, 0x18};
  constexpr uint8_t teleport_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x83, 0xec, 0x20};
  return memcmp(reinterpret_cast<const void*>(kExecThunk), kExecPrologue, sizeof(kExecPrologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kTick), kTickPrologue, sizeof(kTickPrologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kPostLogin), kPostLoginPrologue, sizeof(kPostLoginPrologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kLogout), kLogoutPrologue, sizeof(kLogoutPrologue)) == 0 &&
         ark_death::prologue_matches(reinterpret_cast<const void*>(kDeath)) &&
         reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(kGameModeVtable + 0xc10)) == kPostLogin &&
         reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(kGameModeVtable + 0xc18)) == kLogout &&
         *reinterpret_cast<const uint64_t*>(kResolveWeakController) == 0x56415741e5894855ULL &&
         memcmp(reinterpret_cast<const void*>(kGetActorLocation), location_prologue,
                sizeof(location_prologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(0xf75500), inventory_getter,
                sizeof(inventory_getter)) == 0 &&
         memcmp(reinterpret_cast<const void*>(0xb8cf40), short_name_getter,
                sizeof(short_name_getter)) == 0 &&
         memcmp(reinterpret_cast<const void*>(0x1293230), class_path_getter,
                sizeof(class_path_getter)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kGiveItem), give_item_prologue,
                sizeof(give_item_prologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kTeleport), teleport_prologue,
                sizeof(teleport_prologue)) == 0 &&
         memcmp(reinterpret_cast<const void*>(kClientChatRpc), rpc_prologue, sizeof(rpc_prologue)) == 0 &&
         ark_shutdown_request::signature_matches();
}

std::string boot_id() {
  const int fd = open("/proc/sys/kernel/random/uuid", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return {};
  char bytes[64]{};
  const ssize_t n = read(fd, bytes, sizeof(bytes) - 1);
  close(fd);
  if (n < 36) return {};
  std::string id(bytes, static_cast<size_t>(n));
  if (!id.empty() && id.back() == '\n') id.pop_back();
  return id.size() == 36 ? id : std::string{};
}

bool install() {
  auto* target = reinterpret_cast<unsigned char*>(kExecThunk);
  if (memcmp(target, kExecPrologue, sizeof(kExecPrologue)) != 0) return false;
  g_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_trampoline == MAP_FAILED) { g_trampoline = nullptr; return false; }
  auto* trampoline = reinterpret_cast<unsigned char*>(g_trampoline);
  memcpy(g_original_prologue, target, sizeof(g_original_prologue));
  memcpy(trampoline, target, sizeof(kExecPrologue));
  jump_absolute(trampoline + sizeof(kExecPrologue), target + sizeof(kExecPrologue));
  g_original_exec = reinterpret_cast<ExecFn>(trampoline);
  if (!writable(target, sizeof(kExecPrologue))) {
    munmap(g_trampoline, 4096); g_trampoline = nullptr; g_original_exec = nullptr;
    return false;
  }
  jump_absolute(target, reinterpret_cast<void*>(&exec_hook));
  target[12] = 0x90; target[13] = 0x90;
  __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + sizeof(kExecPrologue)));
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
    memcpy(target, g_original_prologue, sizeof(g_original_prologue));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(g_original_prologue)));
    (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
    munmap(g_trampoline, 4096); g_trampoline = nullptr; g_original_exec = nullptr;
    return false;
  }
  g_exec_installed = true;
  return true;
}

bool install_tick() {
  auto* target = reinterpret_cast<unsigned char*>(kTick);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  if (memcmp(target, kTickPrologue, sizeof(kTickPrologue)) != 0) return false;
  g_tick_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_tick_trampoline == MAP_FAILED) { g_tick_trampoline = nullptr; return false; }
  auto* trampoline = reinterpret_cast<unsigned char*>(g_tick_trampoline);
  memcpy(g_original_tick_prologue, target, sizeof(g_original_tick_prologue));
  memcpy(trampoline, target, sizeof(kTickPrologue));
  jump_absolute(trampoline + sizeof(kTickPrologue), target + sizeof(kTickPrologue));
  g_original_tick = reinterpret_cast<TickFn>(trampoline);
  if (!writable(target, sizeof(kTickPrologue))) {
    munmap(g_tick_trampoline, 4096); g_tick_trampoline = nullptr; g_original_tick = nullptr;
    return false;
  }
  jump_absolute(target, reinterpret_cast<void*>(&tick_hook));
  target[12] = 0x90; target[13] = 0x90; target[14] = 0x90;
  __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + sizeof(kTickPrologue)));
  const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
    memcpy(target, g_original_tick_prologue, sizeof(g_original_tick_prologue));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(g_original_tick_prologue)));
    (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
    munmap(g_tick_trampoline, 4096); g_tick_trampoline = nullptr; g_original_tick = nullptr;
    return false;
  }
  g_tick_installed = true;
  return true;
}

bool install_logout() {
  auto* target = reinterpret_cast<unsigned char*>(kLogout);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || memcmp(target, kLogoutPrologue, sizeof(kLogoutPrologue)) != 0) return false;
  g_logout_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_logout_trampoline == MAP_FAILED) { g_logout_trampoline = nullptr; return false; }
  auto* trampoline = reinterpret_cast<unsigned char*>(g_logout_trampoline);
  memcpy(g_original_logout_prologue, target, sizeof(g_original_logout_prologue));
  memcpy(trampoline, target, sizeof(kLogoutPrologue));
  jump_absolute(trampoline + sizeof(kLogoutPrologue), target + sizeof(kLogoutPrologue));
  g_original_logout = reinterpret_cast<LogoutFn>(trampoline);
  if (!writable(target, sizeof(kLogoutPrologue))) {
    munmap(g_logout_trampoline, 4096); g_logout_trampoline = nullptr; g_original_logout = nullptr;
    return false;
  }
  jump_absolute(target, reinterpret_cast<void*>(&logout_hook));
  for (size_t i = 12; i < sizeof(kLogoutPrologue); ++i) target[i] = 0x90;
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + sizeof(kLogoutPrologue)));
  const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
    memcpy(target, g_original_logout_prologue, sizeof(g_original_logout_prologue));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(g_original_logout_prologue)));
    (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
    munmap(g_logout_trampoline, 4096); g_logout_trampoline = nullptr; g_original_logout = nullptr;
    return false;
  }
  g_logout_installed = true;
  return true;
}

bool install_postlogin() {
  auto* target = reinterpret_cast<unsigned char*>(kPostLogin);
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || memcmp(target, kPostLoginPrologue, sizeof(kPostLoginPrologue)) != 0) return false;
  g_postlogin_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_postlogin_trampoline == MAP_FAILED) { g_postlogin_trampoline = nullptr; return false; }
  auto* trampoline = reinterpret_cast<unsigned char*>(g_postlogin_trampoline);
  memcpy(g_original_postlogin_prologue, target, sizeof(g_original_postlogin_prologue));
  memcpy(trampoline, target, sizeof(kPostLoginPrologue));
  jump_absolute(trampoline + sizeof(kPostLoginPrologue), target + sizeof(kPostLoginPrologue));
  g_original_postlogin = reinterpret_cast<PostLoginFn>(trampoline);
  if (!writable(target, sizeof(kPostLoginPrologue))) {
    munmap(g_postlogin_trampoline, 4096); g_postlogin_trampoline = nullptr; g_original_postlogin = nullptr;
    return false;
  }
  jump_absolute(target, reinterpret_cast<void*>(&postlogin_hook));
  for (size_t i = 12; i < sizeof(kPostLoginPrologue); ++i) target[i] = 0x90;
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + sizeof(kPostLoginPrologue)));
  const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
    memcpy(target, g_original_postlogin_prologue, sizeof(g_original_postlogin_prologue));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(g_original_postlogin_prologue)));
    (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
    munmap(g_postlogin_trampoline, 4096); g_postlogin_trampoline = nullptr; g_original_postlogin = nullptr;
    return false;
  }
  g_postlogin_installed = true;
  return true;
}

bool install_death() {
  auto* target = reinterpret_cast<unsigned char*>(kDeath);
  constexpr size_t length = ark_death::kPrologue.size();
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || !ark_death::prologue_matches(target)) return false;
  g_death_trampoline = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_death_trampoline == MAP_FAILED) { g_death_trampoline = nullptr; return false; }
  auto* trampoline = reinterpret_cast<unsigned char*>(g_death_trampoline);
  memcpy(g_original_death_prologue, target, length);
  memcpy(trampoline, target, length);
  jump_absolute(trampoline + length, target + length);
  g_original_death = reinterpret_cast<DeathFn>(trampoline);
  if (!writable(target, length)) {
    munmap(g_death_trampoline, 4096); g_death_trampoline = nullptr; g_original_death = nullptr;
    return false;
  }
  jump_absolute(target, reinterpret_cast<void*>(&death_hook));
  __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + length));
  const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
  if (mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC) != 0) {
    memcpy(target, g_original_death_prologue, length);
    __builtin___clear_cache(reinterpret_cast<char*>(target), reinterpret_cast<char*>(target + length));
    (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
    munmap(g_death_trampoline, 4096); g_death_trampoline = nullptr; g_original_death = nullptr;
    return false;
  }
  g_death_installed = true;
  return true;
}
} // namespace

extern "C" void SteamGameServer_RunCallbacks() {
  using Callback = void (*)();
  static Callback original = reinterpret_cast<Callback>(dlsym(RTLD_NEXT, "SteamGameServer_RunCallbacks"));
  static thread_local bool noted = false;
  if (g_running && !noted) { enqueue("steam-callback-thread"); noted = true; }
  if (original) original();
}

__attribute__((constructor)) static void ark_native_init() {
  if (!exact_build() || !preflight_bindings()) return;
  const char* token = getenv("ARK_NATIVE_TOKEN");
  const std::string id = boot_id();
  if (!token || id.empty()) return;
  g_gate = new gate::Server(token, id);
  if (!g_gate->start()) { delete g_gate; g_gate = nullptr; return; }
  g_state = new State();
  g_running = true;
  g_state->worker = std::thread(drain);
  const bool chat = install();
  const bool tick = install_tick();
  const bool postlogin = install_postlogin();
  const bool logout = install_logout();
  const bool death = install_death();
  g_hooks_ready.store(chat && tick && postlogin && logout && death, std::memory_order_release);
  enqueue(chat ? "exact-build-chat-hook-installed" : "exact-build-chat-hook-rejected");
  enqueue(tick ? "exact-build-tick-hook-installed" : "exact-build-tick-hook-rejected");
  enqueue(postlogin ? "exact-build-postlogin-hook-installed" : "exact-build-postlogin-hook-rejected");
  enqueue(logout ? "exact-build-logout-hook-installed" : "exact-build-logout-hook-rejected");
  enqueue(death ? "exact-build-death-hook-installed" : "exact-build-death-hook-rejected");
}

__attribute__((destructor)) static void ark_native_stop() {
  g_hooks_ready.store(false, std::memory_order_release);
  if (g_gate) g_gate->stop();
  g_running = false;
  if (g_state && g_state->worker.joinable()) g_state->worker.join();
  if (g_chat_slot && g_original_chat) {
    if (writable(g_chat_slot, sizeof(void*))) {
      __atomic_store_n(g_chat_slot, reinterpret_cast<void*>(g_original_chat), __ATOMIC_RELEASE);
      readonly(g_chat_slot);
    }
  }
  if (g_exec_installed) {
    auto* target = reinterpret_cast<unsigned char*>(kExecThunk);
    if (writable(target, sizeof(g_original_prologue))) {
      memcpy(target, g_original_prologue, sizeof(g_original_prologue));
      __builtin___clear_cache(reinterpret_cast<char*>(target),
                              reinterpret_cast<char*>(target + sizeof(g_original_prologue)));
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0) {
        const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
        (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
      }
    }
  }
  if (g_tick_installed) {
    auto* target = reinterpret_cast<unsigned char*>(kTick);
    if (writable(target, sizeof(g_original_tick_prologue))) {
      memcpy(target, g_original_tick_prologue, sizeof(g_original_tick_prologue));
      __builtin___clear_cache(reinterpret_cast<char*>(target),
                              reinterpret_cast<char*>(target + sizeof(g_original_tick_prologue)));
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0) {
        const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
        (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
      }
    }
  }
  if (g_logout_installed) {
    auto* target = reinterpret_cast<unsigned char*>(kLogout);
    if (writable(target, sizeof(g_original_logout_prologue))) {
      memcpy(target, g_original_logout_prologue, sizeof(g_original_logout_prologue));
      __builtin___clear_cache(reinterpret_cast<char*>(target),
                              reinterpret_cast<char*>(target + sizeof(g_original_logout_prologue)));
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0) {
        const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
        (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
      }
    }
  }
  if (g_postlogin_installed) {
    auto* target = reinterpret_cast<unsigned char*>(kPostLogin);
    if (writable(target, sizeof(g_original_postlogin_prologue))) {
      memcpy(target, g_original_postlogin_prologue, sizeof(g_original_postlogin_prologue));
      __builtin___clear_cache(reinterpret_cast<char*>(target),
                              reinterpret_cast<char*>(target + sizeof(g_original_postlogin_prologue)));
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0) {
        const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
        (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
      }
    }
  }
  if (g_death_installed) {
    auto* target = reinterpret_cast<unsigned char*>(kDeath);
    if (writable(target, sizeof(g_original_death_prologue))) {
      memcpy(target, g_original_death_prologue, sizeof(g_original_death_prologue));
      __builtin___clear_cache(reinterpret_cast<char*>(target),
                              reinterpret_cast<char*>(target + sizeof(g_original_death_prologue)));
      const long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0) {
        const auto page = reinterpret_cast<uintptr_t>(target) & ~(uintptr_t(page_size) - 1);
        (void)mprotect(reinterpret_cast<void*>(page), static_cast<size_t>(page_size), PROT_READ | PROT_EXEC);
      }
    }
  }
}
