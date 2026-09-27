#include "../src/save_bindings.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
alignas(8) std::array<unsigned char, 64> klass{};
int calls = 0;
void* game_mode_class() { return klass.data(); }
__attribute__((noinline)) void save_call(void*, int mode) { if (mode == 0) ++calls; }
void check(bool value, const char* label) { if (!value) throw std::runtime_error(label); }
}

int main() {
  try {
    for (const char* command : {"SaveWorld", "saveworld", "SAVEWORLD",
                                "  sAvEwOrLd  "}) {
      check(ark_save::is_console_save_world(command), "exact native SaveWorld command");
    }
    for (const char* command : {"", "SaveWorld now", "save world", "AdminCheat SaveWorld",
                                "Cheat SaveWorld", "SaveWorld;", "SaveWorld; Exit",
                                "SaveWorld\n", "SaveWorld\t", "SaveWorldX",
                                "  SaveWorld /Game/Test  ", "SaveWorld\xC2\xA0"}) {
      check(!ark_save::is_console_save_world(command),
            "arguments, aliases, controls and Unicode spaces are not native SaveWorld");
    }
    alignas(8) std::array<unsigned char, 0xD10> mode{};
    alignas(8) std::array<unsigned char, 0x260> world{};
    alignas(8) std::array<uintptr_t, 0xD80 / 8 + 1> vtable{};
    *reinterpret_cast<void**>(mode.data()) = vtable.data();
    *reinterpret_cast<void**>(mode.data() + 0x10) = klass.data();
    *reinterpret_cast<void**>(world.data() + 0x250) = mode.data();
    *reinterpret_cast<int32_t*>(mode.data() + 0x73C) = -1;
    const auto target = reinterpret_cast<uintptr_t>(&save_call);
    vtable[0xD80 / 8] = target;
    ark_save::Api api{};
    api.game_thread_tid = syscall(SYS_gettid);
    api.moderation.shooter_game_mode_class = &game_mode_class;
    api.moderation.expected_game_mode_class = reinterpret_cast<uintptr_t>(&game_mode_class);
    std::memcpy(api.moderation.game_mode_class_prologue.data(),
                reinterpret_cast<const void*>(&game_mode_class),
                api.moderation.game_mode_class_prologue.size());
    api.expected_target = target;
    api.text_min = target - 1;
    api.text_max = target + 4096;
    std::memcpy(api.prologue.data(), reinterpret_cast<const void*>(target), api.prologue.size());
    check(ark_save::save_world(world.data(), api) == ark_save::Status::completed && calls == 1,
          "verified direct save returns after native call");
    mode[0xCF8] = 1;
    check(ark_save::save_world(world.data(), api) == ark_save::Status::guarded && calls == 1,
          "first early guard refuses a false completion");
    mode[0xCF8] = 0;
    *reinterpret_cast<int32_t*>(mode.data() + 0x73C) = 0;
    check(ark_save::save_world(world.data(), api) == ark_save::Status::guarded && calls == 1,
          "second early guard refuses a false completion");
    *reinterpret_cast<int32_t*>(mode.data() + 0x73C) = -1;
    api.expected_target += 1;
    check(ark_save::save_world(world.data(), api) == ark_save::Status::invalid && calls == 1,
          "unknown vtable target fails closed");
    std::cout << "save bindings tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << "save bindings test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
