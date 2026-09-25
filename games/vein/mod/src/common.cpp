#include "common.h"

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>

static Mutex g_logLock;
static Mutex g_logWriteLock;
struct LogRecord { timespec time; long tid; std::string text; };
static std::deque<LogRecord> g_logQueue;
static size_t g_logBytes = 0;
static uint64_t g_logDropped = 0;

static long TidNow() { return (long)syscall(SYS_gettid); }

const std::string& ExePath() {
    static const std::string p = [] {
        char buf[4096] = {0};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        return std::string(n > 0 ? buf : "");
    }();
    return p;
}

const std::string& ExeDir() {
    static const std::string d = [] {
        std::string p = ExePath();
        size_t s = p.rfind('/');
        return s == std::string::npos ? std::string(".") : p.substr(0, s);
    }();
    return d;
}

const std::string& PluginDataDir() {
    static const std::string d = [] {
        const char* env = getenv("TAKARO_PLUGIN_DATA_DIR");
        std::string dir = (env && *env) ? std::string(env) : ExeDir() + "/takaro";
        ::mkdir(dir.c_str(), 0775);
        return dir;
    }();
    return d;
}

bool DebugEnabled() {
    static const bool on = [] {
        std::string v = ConfigValue("TAKARO_PLUGIN_DEBUG", "debug", "");
        return v == "1" || v == "true";
    }();
    return on;
}

void PluginLog(const char* fmt, ...) {
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    std::string line(msg);
    Guard g(g_logLock);
    while (!g_logQueue.empty() && (g_logQueue.size() >= 1024 || g_logBytes + line.size() > 1024 * 1024)) {
        g_logBytes -= g_logQueue.front().text.size();
        g_logQueue.pop_front();
        ++g_logDropped;
    }
    g_logBytes += line.size();
    g_logQueue.push_back({ts, TidNow(), std::move(line)});
}

uint64_t PluginLogDropped() { Guard g(g_logLock); return g_logDropped; }

void FlushPluginLogs() {
    Guard writer(g_logWriteLock);
    std::deque<LogRecord> batch;
    {
        Guard g(g_logLock);
        batch.swap(g_logQueue);
        g_logBytes = 0;
    }
    if (batch.empty()) return;
    static const std::string path = PluginDataDir() + "/plugin.log";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) { Guard g(g_logLock); g_logDropped += batch.size(); return; }
    for (const auto& record : batch) {
        struct tm tmv;
        gmtime_r(&record.time.tv_sec, &tmv);
        const std::string line = Redact(record.text);
        if (fprintf(f, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%ld] %s\n", tmv.tm_year + 1900,
                    tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                    record.time.tv_nsec / 1000000, record.tid, line.c_str()) < 0) {
            Guard g(g_logLock); ++g_logDropped;
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// Redaction: the server config keeps `Password=` in cleartext and the plugin itself is configured
// with a bearer token, so neither may ever reach the log, /health or an evidence file. The longer
// keys are listed first so "Password" cannot shadow "WorldPassword".

static bool CiStartsWith(const std::string& s, size_t at, const char* p) {
    size_t n = strlen(p);
    if (at + n > s.size()) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[at + i]) != tolower((unsigned char)p[i])) return false;
    return true;
}

std::string Redact(const std::string& s) {
    static const char* kKeys[] = {"WorldPassword", "AdminPassword", "ServerPassword", "Password",
                                  "PluginToken", "AuthToken", "ApiToken", "Token"};
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const char* hit = nullptr;
        for (auto* k : kKeys)
            if (CiStartsWith(s, i, k)) {
                // "Password" must not shadow the longer keys; they are tried first.
                hit = k;
                break;
            }
        if (!hit) {
            o += s[i++];
            continue;
        }
        size_t j = i + strlen(hit);
        o.append(s, i, j - i);
        // optional closing quote of a JSON key
        if (j < s.size() && s[j] == '"') o += s[j++];
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) o += s[j++];
        if (j >= s.size() || (s[j] != '=' && s[j] != ':')) {
            i = j;
            continue;
        }
        o += s[j++];
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) o += s[j++];
        bool quoted = j < s.size() && s[j] == '"';
        if (quoted) j++;
        size_t start = j;
        while (j < s.size() && s[j] != '\n' && s[j] != '\r' && (!quoted || s[j] != '"') &&
               (quoted || (s[j] != ',' && s[j] != ' ' && s[j] != '}')))
            j++;
        if (j > start) o += quoted ? "\"***\"" : "***";
        else if (quoted) o += "\"\"";
        if (quoted && j < s.size() && s[j] == '"') j++;
        i = j;
    }
    return o;
}

// ---------------------------------------------------------------------------------------------

std::string IsoNowUtc() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tmv;
    gmtime_r(&ts.tv_sec, &tmv);
    char b[80];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
    return b;
}

uint64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

const std::string& BootId() {
    static const std::string id = [] {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t v = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^ ((uint64_t)getpid() << 17) ^
                     (uint64_t)(uintptr_t)&ts;
        v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL; v ^= v >> 33;
        char b[24];
        snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
        return std::string(b);
    }();
    return id;
}

// ---------------------------------------------------------------------------------------------

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

std::string JsonNum(double v) {
    if (v != v || v - v != 0) return "null";  // NaN / inf are not valid JSON
    char b[40];
    if (v == (double)(long long)v && v < 9e15 && v > -9e15) snprintf(b, sizeof b, "%lld", (long long)v);
    else snprintf(b, sizeof b, "%.10g", v);
    return b;
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

bool ReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> tmp(pattern.begin(), pattern.end()); tmp.push_back('\0');
    int fd = mkstemp(tmp.data());
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < content.size()) {
        ssize_t n = write(fd, content.data() + offset, content.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += static_cast<size_t>(n);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(tmp.data(), path.c_str()) != 0) ok = false;
    if (!ok) { unlink(tmp.data()); return false; }
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    int parent = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return false;
    ok = fsync(parent) == 0;
    if (close(parent) != 0) ok = false;
    return ok;
}

std::string ConfigValue(const char* envName, const char* jsonKey, const std::string& def) {
    if (envName) {
        const char* v = getenv(envName);
        if (v && *v) return v;
    }
    static const JsonValue cfg = [] {
        JsonValue parsed;
        std::string text;
        if (ReadFile(PluginDataDir() + "/plugin.json", text)) {
            if (!JsonParse(text, parsed)) PluginLog("config: plugin.json parse failed");
        }
        return parsed;
    }();
    if (jsonKey) {
        auto* v = cfg.get(jsonKey);
        if (v && v->isStr() && !v->str.empty()) return v->str;
        if (v && v->isNum()) return v->str;
    }
    return def;
}
