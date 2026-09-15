#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static std::string g_baseDir;
static SrwLock g_logLock;

extern HMODULE g_selfModule;

const std::string& PluginBaseDir() {
    if (g_baseDir.empty()) {
        char p[MAX_PATH] = {0};
        GetModuleFileNameA(g_selfModule, p, MAX_PATH);
        char* sl = strrchr(p, '\\');
        if (sl) *sl = 0;
        g_baseDir = p;
    }
    return g_baseDir;
}

void PluginLog(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Guard g(g_logLock);
    std::string path = PluginBaseDir() + "\\takaro\\plugin.log";
    FILE* f = fopen(path.c_str(), "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    fclose(f);
}

std::string IsoNowUtc() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, t.wMilliseconds);
    return b;
}

uint64_t NowMs() { return GetTickCount64(); }

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
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
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
        if (i >= s.size()) return false;
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
