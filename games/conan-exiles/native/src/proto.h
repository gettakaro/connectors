// Pure logic with no game or socket access, so the unit tests cover it without a server:
// the bridge /mod/poll protocol, the ChatRpcData layout, HTTP framing and the ELF build-id.
#pragma once

#include <cstdint>
#include <string>

constexpr const char* kModSource = "TakaroConan-native";
constexpr const char* kDefaultSender = "Takaro";
constexpr size_t kMaxMessageChars = 1024;

// ---- bridge protocol (games/conan-exiles/bridge/src/mod/commandBridge.ts) ----
struct PollCommand {
    bool has = false;
    std::string requestId, action;
    std::string message, recipient, sender;  // sendMessage args; sender defaults to kDefaultSender
};
// Parses a GET /mod/poll body. Returns false (with `err`) on a malformed body.
bool ParsePoll(const std::string& body, PollCommand& out, std::string& err);

struct ChatOutcome {
    bool success = false;
    std::string error;
    int delivered = 0;  // controllers the line was sent to
};
// The POST /mod/result body for one command.
std::string ResultBody(const std::string& requestId, const ChatOutcome& outcome);

// `steam:7656...` / `platform:steam:7656...` -> `7656...`; anything else is returned trimmed.
std::string NormalizeRecipient(const std::string& recipient);
bool IsSteam64(const std::string& s);

// ---- text ----
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

// ---- HTTP framing ----
struct HttpUrl { std::string host; int port = 80; std::string basePath; };
bool ParseHttpUrl(const std::string& url, HttpUrl& out);
// Parses a complete HTTP/1.1 response (Content-Length, chunked, or close-delimited).
bool ParseHttpResponse(const std::string& raw, int& status, std::string& body);

// ---- ELF ----
// The hex GNU build-id of an ELF64 file, or "" when it has none or cannot be read.
std::string ReadElfBuildId(const std::string& path);
