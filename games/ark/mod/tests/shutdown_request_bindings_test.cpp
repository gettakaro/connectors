#include "../src/shutdown_request_bindings.hpp"

#include <cassert>

namespace {
volatile uint8_t exit_flag = 0;
bool force_seen = true;
bool set_flag = true;
void mock_request(bool force) {
  force_seen = force;
  if (set_flag) exit_flag = 1;
}
}

int main() {
  using namespace ark_shutdown_request;
  Api api{};
  api.request = &mock_request;
  api.entry = reinterpret_cast<const uint8_t*>(&mock_request);
  api.exit_requested = &exit_flag;
  api.exact_binding = false;
  api.game_thread_tid = syscall(SYS_gettid);
  assert(ready_to_stage(api));
  assert(request_after_ack(false, true, api) == Status::guarded);
  assert(request_after_ack(true, false, api) == Status::guarded);
  api.game_thread_tid += 1;
  assert(request_after_ack(true, true, api) == Status::guarded);
  api.game_thread_tid -= 1;
  assert(request_after_ack(true, true, api) == Status::requested);
  assert(!force_seen && exit_flag == 1);
  assert(!ready_to_stage(api));
  assert(request_after_ack(true, true, api) == Status::already_requested);
  exit_flag = 0;
  set_flag = false;
  assert(request_after_ack(true, true, api) == Status::postcondition_failed);
  api.exact_binding = true;
  assert(!signature_matches(api));
  assert(request_after_ack(true, true, api) == Status::guarded);

  assert(is_termination_command("Exit"));
  assert(is_termination_command("  qUiT now"));
  assert(is_termination_command("\tEXIT\t"));
  assert(is_termination_command("GetAll Foo; Quit"));
  assert(is_termination_command("DoExit"));
  assert(is_termination_command("admincheat doexit"));
  assert(is_termination_command(" CHEAT EXIT now"));
  assert(is_termination_command("GetAll Foo; AdminCheat Quit"));
  assert(!is_termination_command("exitfoo"));
  assert(!is_termination_command("doexitfoo"));
  assert(!is_termination_command("admincheat doexitfoo"));
  assert(!is_termination_command("GetAll ShooterPlayerState PlayerName"));
  assert(!is_termination_command("ListPlayers"));
  assert(contains_unicode_space("\xc2\xa0" "EXIT"));
  assert(contains_unicode_space("\xe2\x80\x80" "QUIT"));
  assert(contains_unicode_space("\xe2\x80\xaf" "EXIT"));
  assert(contains_unicode_space("\xe2\x81\x9f" "EXIT"));
  assert(contains_unicode_space("\xe3\x80\x80" "EXIT"));
  assert(contains_unicode_space("\xef\xbb\xbf" "EXIT"));
  assert(!contains_unicode_space("GetAll ShooterPlayerState PlayerName"));
}
