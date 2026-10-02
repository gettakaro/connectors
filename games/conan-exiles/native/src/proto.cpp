#include "proto.h"

#include "common.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string StrField(const JsonValue* obj, const char* key) {
    if (!obj) return "";
    const JsonValue* v = obj->get(key);
    return v && v->isStr() ? v->str : "";
}
}  // namespace

bool ParsePoll(const std::string& body, PollCommand& out, std::string& err) {
    out = PollCommand();
    JsonValue root;
    if (!JsonParse(body, root) || root.type != JsonValue::Object) {
        err = "poll body is not a JSON object";
        return false;
    }
    const JsonValue* has = root.get("hasCommand");
    if (!has || has->type != JsonValue::Bool) {
        err = "poll body has no hasCommand";
        return false;
    }
    if (!has->b) return true;
    const JsonValue* cmd = root.get("command");
    if (!cmd || cmd->type != JsonValue::Object) {
        err = "poll body has no command";
        return false;
    }
    out.requestId = StrField(cmd, "requestId");
    out.action = StrField(cmd, "action");
    if (out.requestId.empty() || out.action.empty()) {
        err = "command has no requestId or action";
        return false;
    }
    out.has = true;
    const JsonValue* args = cmd->get("args");
    out.message = StrField(args, "message");
    out.recipient = NormalizeRecipient(StrField(args, "recipient"));
    out.sender = Trim(StrField(args, "senderNameOverride"));
    if (out.sender.empty()) out.sender = kDefaultSender;
    return true;
}

std::string ResultBody(const std::string& requestId, const ChatOutcome& o) {
    std::string r = "{\"requestId\":" + JsonStr(requestId) + ",\"result\":{\"success\":" +
                    (o.success ? "true" : "false");
    if (!o.success) r += ",\"error\":" + JsonStr(o.error);
    r += ",\"delivered\":" + std::to_string(o.delivered) + ",\"transport\":" + JsonStr(kModSource) + "}}";
    return r;
}

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

bool ParseHttpUrl(const std::string& url, HttpUrl& out) {
    out = HttpUrl();
    const std::string scheme = "http://";
    if (url.compare(0, scheme.size(), scheme) != 0) return false;
    std::string rest = url.substr(scheme.size());
    size_t slash = rest.find('/');
    std::string hostPort = rest.substr(0, slash);
    out.basePath = slash == std::string::npos ? "" : rest.substr(slash);
    while (!out.basePath.empty() && out.basePath.back() == '/') out.basePath.pop_back();
    size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        std::string p = hostPort.substr(colon + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        out.port = atoi(p.c_str());
        hostPort = hostPort.substr(0, colon);
    }
    if (hostPort.empty() || out.port <= 0 || out.port > 65535) return false;
    out.host = hostPort;
    return true;
}

bool ParseHttpResponse(const std::string& raw, int& status, std::string& body) {
    size_t eoh = raw.find("\r\n\r\n");
    if (eoh == std::string::npos || raw.compare(0, 5, "HTTP/") != 0) return false;
    size_t sp = raw.find(' ');
    if (sp == std::string::npos || sp > eoh) return false;
    status = atoi(raw.c_str() + sp + 1);
    if (status < 100 || status > 599) return false;
    std::string headers = raw.substr(0, eoh);
    for (auto& c : headers) c = (char)tolower((unsigned char)c);
    std::string rest = raw.substr(eoh + 4);
    if (headers.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
        body.clear();
        size_t i = 0;
        for (;;) {
            size_t eol = rest.find("\r\n", i);
            if (eol == std::string::npos) return false;
            size_t digits = i;
            while (digits < eol && isxdigit((unsigned char)rest[digits])) digits++;
            // hex size, then nothing or a ;chunk-extension
            if (digits == i || digits - i > 8 || (digits < eol && rest[digits] != ';')) return false;
            unsigned long n = strtoul(rest.c_str() + i, nullptr, 16);
            i = eol + 2;
            if (n == 0) return rest.find("\r\n", i) != std::string::npos;  // trailers end with CRLF
            if (i + n + 2 > rest.size() || rest.compare(i + n, 2, "\r\n") != 0) return false;
            body.append(rest, i, n);
            i += n + 2;
        }
    }
    size_t cl = headers.find("\r\ncontent-length:");
    if (cl != std::string::npos) {
        unsigned long n = strtoul(headers.c_str() + cl + 17, nullptr, 10);
        if (rest.size() < n) return false;
        body = rest.substr(0, n);
        return true;
    }
    body = rest;
    return true;
}

std::string ReadElfBuildId(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    std::string id;
    Elf64_Ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) == (ssize_t)sizeof eh && memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_phentsize == sizeof(Elf64_Phdr) && eh.e_phnum < 256) {
        std::vector<Elf64_Phdr> ph(eh.e_phnum);
        ssize_t want = (ssize_t)(ph.size() * sizeof(Elf64_Phdr));
        if (pread(fd, ph.data(), want, (off_t)eh.e_phoff) == want) {
            for (auto& p : ph) {
                if (p.p_type != PT_NOTE || p.p_filesz > 65536 || !id.empty()) continue;
                std::vector<uint8_t> buf(p.p_filesz);
                if (pread(fd, buf.data(), buf.size(), (off_t)p.p_offset) != (ssize_t)buf.size()) continue;
                for (size_t off = 0; off + sizeof(Elf64_Nhdr) <= buf.size();) {
                    Elf64_Nhdr nh;
                    memcpy(&nh, buf.data() + off, sizeof nh);
                    size_t name = off + sizeof nh, desc = name + ((nh.n_namesz + 3) & ~3u);
                    size_t next = desc + ((nh.n_descsz + 3) & ~3u);
                    if (next > buf.size()) break;
                    if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && memcmp(buf.data() + name, "GNU", 4) == 0) {
                        static const char* hex = "0123456789abcdef";
                        for (size_t k = 0; k < nh.n_descsz; k++) {
                            id += hex[buf[desc + k] >> 4];
                            id += hex[buf[desc + k] & 15];
                        }
                        break;
                    }
                    off = next;
                }
            }
        }
    }
    close(fd);
    return id;
}
