#include "conan/text.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace conan {

namespace {
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

}  // namespace

std::string NormalizeRecipient(const std::string& recipient) {
    std::string r = Trim(recipient);
    size_t colon = r.rfind(':');
    if (colon != std::string::npos && IsSteam64(r.substr(colon + 1))) return r.substr(colon + 1);
    return r;
}

bool IsSteam64(const std::string& s) {
    if (s.size() != 17 || s.compare(0, 4, "7656") != 0) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

std::u16string ChatText(const std::string& s, size_t maxChars) {
    std::u16string o;
    auto push = [&](uint32_t cp) {
        if (cp == '\r' || cp == '\n' || cp == '\t') cp = ' ';
        else if (cp < 0x20 || cp == 0x7f) return;
        if (cp >= 0x10000) {
            if (o.size() + 2 > maxChars) return;
            cp -= 0x10000;
            o += (char16_t)(0xD800 + (cp >> 10));
            o += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else if (o.size() < maxChars) {
            o += (char16_t)cp;
        }
    };
    for (size_t i = 0; i < s.size() && o.size() < maxChars;) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + n >= s.size()) {
            push(0xFFFD);
            i++;
            continue;
        }
        uint32_t cp = n == 0 ? c : (c & (0x3F >> n));
        bool ok = true;
        for (int k = 1; k <= n; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        static const uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
            push(0xFFFD);
            i++;
            continue;
        }
        push(cp);
        i += n + 1;
    }
    return o;
}

std::string Utf16To8(const std::u16string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[++i] - 0xDC00);
        else if (cp >= 0xD800 && cp < 0xE000)
            cp = 0xFFFD;
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    return o;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

uint64_t FileTimeTicks(int64_t unixSec, long nsec) {
    return (uint64_t)unixSec * 10000000ULL + (uint64_t)(nsec / 100) + 116444736000000000ULL;
}

static void PutFString(uint8_t* at, const std::u16string& s) {
    const char16_t* data = s.c_str();
    int32_t num = (int32_t)s.size() + 1;  // FString counts the terminating NUL
    memcpy(at, &data, 8);
    memcpy(at + 8, &num, 4);
    memcpy(at + 12, &num, 4);
}

void PackChatRpc(uint8_t* out, uint64_t ticks, const std::u16string& user, const std::u16string& channel,
                 const std::u16string& message) {
    memset(out, 0, ChatRpc::kSize);
    memcpy(out + ChatRpc::kTimestamp, &ticks, 8);
    PutFString(out + ChatRpc::kUserName, user);
    PutFString(out + ChatRpc::kChannel, channel);
    PutFString(out + ChatRpc::kMessage, message);
    out[ChatRpc::kGenerated] = 0;
}

}  // namespace conan
