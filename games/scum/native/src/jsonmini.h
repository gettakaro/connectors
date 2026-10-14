// Minimal JSON helpers for the transport: a flat object builder for status output and a
// type-frame check. The protocol's JSON lives in Lua (Scripts/takaro/json.lua).
#pragma once
#include <cstdio>
#include <string>

inline std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 2);
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

class ObjBuilder {
public:
    ObjBuilder& Raw(const char* k, const std::string& raw) {
        Key(k);
        s_ += raw;
        return *this;
    }
    ObjBuilder& S(const char* k, const std::string& v) { return Raw(k, "\"" + JsonEscape(v) + "\""); }
    ObjBuilder& B(const char* k, bool v) { return Raw(k, v ? "true" : "false"); }
    ObjBuilder& N(const char* k, double v) {
        char b[64];
        snprintf(b, sizeof b, "%.17g", v);
        return Raw(k, b);
    }
    std::string Done() { return s_ + "}"; }

private:
    void Key(const char* k) {
        s_ += first_ ? "" : ",";
        first_ = false;
        s_ += "\"";
        s_ += k;
        s_ += "\":";
    }
    std::string s_ = "{";
    bool first_ = true;
};

// True when `msg` is a JSON object whose "type" member is the string `type` (whitespace tolerant;
// only used on short control frames such as {"type":"pong"}).
inline bool IsTypeFrame(const std::string& msg, const char* type) {
    size_t k = msg.find("\"type\"");
    if (k == std::string::npos) return false;
    size_t i = k + 6;
    auto ws = [&] {
        while (i < msg.size() && (msg[i] == ' ' || msg[i] == '\t' || msg[i] == '\r' || msg[i] == '\n')) i++;
    };
    ws();
    if (i >= msg.size() || msg[i] != ':') return false;
    i++;
    ws();
    std::string want = std::string("\"") + type + "\"";
    return msg.compare(i, want.size(), want) == 0;
}
