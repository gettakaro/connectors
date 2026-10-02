#include "common.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace {
struct LogRecord { timespec time; long tid; std::string text; };
// Function-local and leaked on purpose: the library constructor logs before other translation
// units are initialized, and other threads may still log while the process exits.
struct LogState {
    std::mutex lock, writeLock;
    std::deque<LogRecord> queue;
    size_t bytes = 0;
    uint64_t dropped = 0;
    std::string path;
};
LogState& L() {
    static LogState* s = new LogState;
    return *s;
}
}  // namespace

void SetNativeLogPath(const std::string& path) {
    std::lock_guard<std::mutex> g(L().writeLock);
    L().path = path;
}

void NativeLog(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    std::string line(msg);
    LogState& l = L();
    std::lock_guard<std::mutex> g(l.lock);
    while (!l.queue.empty() && (l.queue.size() >= 512 || l.bytes + line.size() > 256 * 1024)) {
        l.bytes -= l.queue.front().text.size();
        l.queue.pop_front();
        ++l.dropped;
    }
    l.bytes += line.size();
    l.queue.push_back({ts, (long)syscall(SYS_gettid), std::move(line)});
}

void FlushNativeLogs() {
    LogState& l = L();
    std::lock_guard<std::mutex> writer(l.writeLock);
    if (l.path.empty()) return;
    std::deque<LogRecord> batch;
    uint64_t dropped;
    {
        std::lock_guard<std::mutex> g(l.lock);
        batch.swap(l.queue);
        l.bytes = 0;
        dropped = l.dropped;
        l.dropped = 0;
    }
    if (batch.empty() && !dropped) return;
    FILE* f = fopen(l.path.c_str(), "a");
    if (!f) return;
    if (dropped) fprintf(f, "(%llu log records dropped)\n", (unsigned long long)dropped);
    for (const auto& r : batch) {
        struct tm tmv;
        gmtime_r(&r.time.tv_sec, &tmv);
        fprintf(f, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%ld] %s\n", tmv.tm_year + 1900, tmv.tm_mon + 1,
                tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, r.time.tv_nsec / 1000000, r.tid, r.text.c_str());
    }
    fclose(f);
}

std::string EnvOr(const char* name, const std::string& def) {
    const char* v = getenv(name);
    return (v && *v) ? std::string(v) : def;
}

uint64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------------------------------------
// JSON, copied from games/vein/mod/src/common.cpp.

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += (char)c;
                }
        }
    }
    return o;
}

const JsonValue* JsonValue::get(const std::string& key) const {
    if (type != Object) return nullptr;
    for (auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    int depth = 0;
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& o) {
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': o += '"'; break;
                case '\\': o += '\\'; break;
                case '/': o += '/'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(o, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool val(JsonValue& v) {
        if (++depth > 64) return false;
        ws();
        if (i >= s.size()) { depth--; return false; }
        char c = s[i];
        bool ok = false;
        if (c == '{') {
            v.type = JsonValue::Object;
            i++;
            ws();
            if (i < s.size() && s[i] == '}') { i++; ok = true; }
            else {
                for (;;) {
                    ws();
                    std::string k;
                    if (!str(k)) break;
                    ws();
                    if (i >= s.size() || s[i] != ':') break;
                    i++;
                    JsonValue child;
                    if (!val(child)) break;
                    v.obj.emplace_back(std::move(k), std::move(child));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == '}') { i++; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            v.type = JsonValue::Array;
            i++;
            ws();
            if (i < s.size() && s[i] == ']') { i++; ok = true; }
            else {
                for (;;) {
                    JsonValue child;
                    if (!val(child)) break;
                    v.arr.push_back(std::move(child));
                    ws();
                    if (i < s.size() && s[i] == ',') { i++; continue; }
                    if (i < s.size() && s[i] == ']') { i++; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            v.type = JsonValue::String;
            ok = str(v.str);
        } else if (s.compare(i, 4, "true") == 0) { v.type = JsonValue::Bool; v.b = true; i += 4; ok = true; }
        else if (s.compare(i, 5, "false") == 0) { v.type = JsonValue::Bool; i += 5; ok = true; }
        else if (s.compare(i, 4, "null") == 0) { v.type = JsonValue::Null; i += 4; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const char* start = s.c_str() + i;
            char* end = nullptr;
            v.type = JsonValue::Number;
            v.num = strtod(start, &end);
            if (end && end > start) { v.str.assign(start, (size_t)(end - start)); i += (size_t)(end - start); ok = true; }
        }
        depth--;
        return ok;
    }
};
}  // namespace

bool JsonParse(const std::string& text, JsonValue& out) {
    Parser p{text};
    out = JsonValue();
    if (!p.val(out)) return false;
    p.ws();
    return p.i == text.size();
}

