// Pure text and chat-packing logic with no game or socket access, so the unit tests cover it
// without a server: UTF-8/UTF-16 conversion, recipient normalisation and the ChatRpcData layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace conan {

constexpr const char* kDefaultSender = "Takaro";
constexpr size_t kMaxMessageChars = 1024;

// `steam:7656...` / `platform:steam:7656...` -> `7656...`; anything else is returned trimmed.
std::string NormalizeRecipient(const std::string& recipient);
bool IsSteam64(const std::string& s);

// UTF-8 -> UTF-16. Invalid sequences become U+FFFD; CR/LF/TAB become spaces, other control
// characters are dropped, and the result is capped at `maxChars` code units.
std::u16string ChatText(const std::string& utf8, size_t maxChars = kMaxMessageChars);
std::string Utf16To8(const std::u16string& s);
// Case-insensitive ASCII comparison, for the character-name fallback.
bool EqualsIgnoreCase(const std::string& a, const std::string& b);

// ---- ChatRpcData (ConanPlayerController::ClientReceiveChatMessage, build 25639945) ----
namespace ChatRpc {
constexpr size_t kSize = 0x80;
constexpr size_t kTimestamp = 0x00;  // u64 FILETIME (100 ns since 1601-01-01 UTC)
constexpr size_t kUserName = 0x48;   // FString
constexpr size_t kChannel = 0x58;    // FString
constexpr size_t kMessage = 0x68;    // FString
constexpr size_t kGenerated = 0x78;  // bool
}  // namespace ChatRpc

// The ChatRpcData timestamp for a Unix time: Windows FILETIME, 100 ns since 1601-01-01 UTC
// (a real player message captured on 25639945 carried 0x01dd529b80c4e16f).
uint64_t FileTimeTicks(int64_t unixSec, long nsec);
// Fills a zeroed ChatRpcData. The FStrings point into the given buffers, which must outlive
// the ProcessEvent call; the engine copies them while serializing the RPC.
void PackChatRpc(uint8_t* out, uint64_t ticks, const std::u16string& user, const std::u16string& channel,
                 const std::u16string& message);

}  // namespace conan
