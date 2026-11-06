#pragma once

// ShooterGameServer 21241282, SHA-256 7e7ded49...7c520 only.
// A chat line with a sender name: AShooterPlayerController::ClientChatMessage(FChatMessage),
// the client RPC that also delivers player chat. Its generated wrapper 0x136BE40 copies
// the caller's struct into its parameters (0x154D380), finds the function by the FName
// global 0x58EA9B8 (built from L"ClientChatMessage" at 0x7890DD), calls ProcessEvent
// (vtable +0x1E8) and destroys only its own copy (0xF215A0).
// FChatMessage (0x90 bytes): offsets from its reflection constructor (0x130BF00..0x130C79C,
// one property per field) and the wrapper's default initialization (SenderTeamIndex -1).
// The RPC goes to one client; it never runs ServerSendChatMessage, so it cannot come
// back as a player chat-message event.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ark_chat {

struct FString {
  const wchar_t* data;
  int32_t count;
  int32_t capacity;
};
static_assert(sizeof(FString) == 16);

struct FChatMessage {
  FString sender_name;        // 0x00 SenderName
  FString sender_steam_name;  // 0x10 SenderSteamName
  FString sender_tribe_name;  // 0x20 SenderTribeName
  uint32_t sender_id;         // 0x30 SenderId
  FString message;            // 0x38 Message
  FString receiver;           // 0x48 Receiver
  int32_t sender_team_index;  // 0x58 SenderTeamIndex
  double received_time;       // 0x60 ReceivedTime
  uint8_t send_mode;          // 0x68 SendMode (0 = GlobalChat)
  uint32_t radio_frequency;   // 0x6C RadioFrequency
  uint8_t chat_type;          // 0x70 ChatType (0 = GlobalChat)
  void* sender_icon;          // 0x78 SenderIcon
  FString user_id;            // 0x80 UserId
};
static_assert(offsetof(FChatMessage, sender_name) == 0x00);
static_assert(offsetof(FChatMessage, sender_steam_name) == 0x10);
static_assert(offsetof(FChatMessage, sender_tribe_name) == 0x20);
static_assert(offsetof(FChatMessage, sender_id) == 0x30);
static_assert(offsetof(FChatMessage, message) == 0x38);
static_assert(offsetof(FChatMessage, receiver) == 0x48);
static_assert(offsetof(FChatMessage, sender_team_index) == 0x58);
static_assert(offsetof(FChatMessage, received_time) == 0x60);
static_assert(offsetof(FChatMessage, send_mode) == 0x68);
static_assert(offsetof(FChatMessage, radio_frequency) == 0x6C);
static_assert(offsetof(FChatMessage, chat_type) == 0x70);
static_assert(offsetof(FChatMessage, sender_icon) == 0x78);
static_assert(offsetof(FChatMessage, user_id) == 0x80);
static_assert(sizeof(FChatMessage) == 0x90);

using ClientChatFn = void (*)(void*, const FChatMessage*);

struct Api {
  uintptr_t client_chat_message = 0x136BE40;
  uintptr_t text_start = 0x648400;
  uintptr_t text_end = 0x3F6170F;
  // push rbp; mov rbp,rsp; push r14; push rbx; sub rsp,0x90; mov rbx,rdi
  static constexpr unsigned char prologue[17] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x81,
                                                 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48, 0x89, 0xFB};
  bool check_prologue = true;
};

inline FString borrow(const std::u32string& text) {
  static_assert(sizeof(wchar_t) == sizeof(char32_t));
  if (text.empty()) return {nullptr, 0, 0};
  const auto count = static_cast<int32_t>(text.size() + 1);
  return {reinterpret_cast<const wchar_t*>(text.c_str()), count, count};
}

// The line one player's client shows as "<sender>: <text>" in global chat.
inline FChatMessage named_line(const std::u32string& sender, const std::u32string& text) {
  FChatMessage chat{};
  chat.sender_name = borrow(sender);
  chat.message = borrow(text);
  chat.sender_team_index = -1;
  return chat;
}

// Game thread only. False when the exact wrapper is not where this build has it.
inline bool send_named(void* controller, const std::u32string& sender, const std::u32string& text,
                       const Api& api = {}) {
  if (!controller || sender.empty() || sender.size() > 128 || text.empty() || text.size() > 512) return false;
  const uintptr_t fn = api.client_chat_message;
  if (api.check_prologue &&
      (fn < api.text_start || fn >= api.text_end ||
       std::memcmp(reinterpret_cast<const void*>(fn), Api::prologue, sizeof(Api::prologue)) != 0))
    return false;
  const FChatMessage chat = named_line(sender, text);
  reinterpret_cast<ClientChatFn>(fn)(controller, &chat);
  return true;
}

}  // namespace ark_chat
