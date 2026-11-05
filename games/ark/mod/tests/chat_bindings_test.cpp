// Named chat line: FChatMessage layout and the ClientChatMessage wrapper call.
// The name is the native sender; the text stays the message; nothing else is set.
#include "../src/chat_bindings.hpp"
#include <cassert>
#include <cstring>
#include <string>

namespace {
void* seen_controller = nullptr;
std::u32string seen_sender, seen_text;
int calls = 0;
ark_chat::FChatMessage seen{};

std::u32string read(const ark_chat::FString& s) {
  if (!s.data) return {};
  assert(s.count >= 1 && s.capacity == s.count && s.data[s.count - 1] == 0);
  return std::u32string(reinterpret_cast<const char32_t*>(s.data), static_cast<size_t>(s.count - 1));
}

void fake_client_chat(void* controller, const ark_chat::FChatMessage* chat) {
  ++calls;
  seen_controller = controller;
  seen = *chat;
  seen_sender = read(chat->sender_name);
  seen_text = read(chat->message);
}
}  // namespace

int main() {
  ark_chat::Api api{};
  api.client_chat_message = reinterpret_cast<uintptr_t>(&fake_client_chat);
  api.check_prologue = false;
  int controller = 0;

  assert(ark_chat::send_named(&controller, U"Takaro", U"hello", api));
  assert(calls == 1 && seen_controller == &controller);
  assert(seen_sender == U"Takaro" && seen_text == U"hello");
  assert(!seen.sender_steam_name.data && !seen.sender_tribe_name.data && !seen.receiver.data && !seen.user_id.data);
  assert(seen.sender_id == 0 && seen.sender_team_index == -1 && seen.received_time == 0.0);
  assert(seen.send_mode == 0 && seen.chat_type == 0 && seen.radio_frequency == 0 && !seen.sender_icon);

  // Refused without a call: no controller, no name, no text, oversized name or text.
  assert(!ark_chat::send_named(nullptr, U"Takaro", U"hello", api));
  assert(!ark_chat::send_named(&controller, U"", U"hello", api));
  assert(!ark_chat::send_named(&controller, U"Takaro", U"", api));
  assert(!ark_chat::send_named(&controller, std::u32string(129, U'n'), U"hello", api));
  assert(!ark_chat::send_named(&controller, U"Takaro", std::u32string(513, U't'), api));
  assert(ark_chat::send_named(&controller, std::u32string(128, U'n'), std::u32string(512, U't'), api));
  assert(calls == 2);

  // The exact-build wrapper only: wrong bytes or outside .text means no call.
  alignas(16) unsigned char code[32]{};
  std::memcpy(code, ark_chat::Api::prologue, sizeof(ark_chat::Api::prologue));
  ark_chat::Api checked{};
  checked.client_chat_message = reinterpret_cast<uintptr_t>(code);
  checked.text_start = reinterpret_cast<uintptr_t>(code);
  checked.text_end = reinterpret_cast<uintptr_t>(code) + sizeof(code);
  code[10] = 0x80;  // sub rsp,0x80: a different function
  assert(!ark_chat::send_named(&controller, U"Takaro", U"hello", checked));
  checked.text_end = checked.text_start;  // outside the executable's .text
  code[10] = 0x90;
  assert(!ark_chat::send_named(&controller, U"Takaro", U"hello", checked));
  assert(calls == 2);
  return 0;
}
