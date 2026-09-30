// Minimal test harness for the native connector host tests.
#pragma once
#include "common.h"
#include "native/json_util.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace t {

struct Stats {
    int checks = 0, failures = 0;
    std::string group;
};
inline Stats& S() {
    static Stats s;
    return s;
}

inline void Check(bool ok, const char* expr, const char* file, int line, const std::string& detail = "") {
    S().checks++;
    if (ok) return;
    S().failures++;
    fprintf(stderr, "FAIL [%s] %s:%d: %s %s\n", S().group.c_str(), file, line, expr, detail.c_str());
}

#define CHECK(e) t::Check((e), #e, __FILE__, __LINE__)
#define CHECK_MSG(e, msg) t::Check((e), #e, __FILE__, __LINE__, (msg))
#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        auto _a = (a);                                                                                              \
        auto _b = (b);                                                                                              \
        t::Check(_a == _b, #a " == " #b, __FILE__, __LINE__, "got [" + t::Show(_a) + "] want [" + t::Show(_b) + "]"); \
    } while (0)

inline std::string Show(const std::string& s) { return s; }
inline std::string Show(const char* s) { return s; }
inline std::string Show(bool b) { return b ? "true" : "false"; }
template <typename T>
std::string Show(const T& v) {
    return std::to_string(v);
}

// Structural JSON equality: object key order ignored, numbers compared by value.
inline bool JsonEq(const JsonValue& a, const JsonValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case JsonValue::Null: return true;
        case JsonValue::Bool: return a.b == b.b;
        case JsonValue::Number: return a.num == b.num || (std::isnan(a.num) && std::isnan(b.num));
        case JsonValue::String: return a.str == b.str;
        case JsonValue::Array:
            if (a.arr.size() != b.arr.size()) return false;
            for (size_t i = 0; i < a.arr.size(); i++)
                if (!JsonEq(a.arr[i], b.arr[i])) return false;
            return true;
        case JsonValue::Object:
            if (a.obj.size() != b.obj.size()) return false;
            for (auto& kv : a.obj) {
                const JsonValue* o = b.get(kv.first);
                if (!o || !JsonEq(kv.second, *o)) return false;
            }
            return true;
    }
    return false;
}

inline JsonValue J(const std::string& text) {
    JsonValue v;
    if (!native::ParseJson(text, v)) {
        fprintf(stderr, "bad JSON in test: %s\n", text.c_str());
        abort();
    }
    return v;
}

inline bool JsonTextEq(const std::string& a, const std::string& b) {
    JsonValue x, y;
    return native::ParseJson(a, x) && native::ParseJson(b, y) && JsonEq(x, y);
}

inline std::string ReadFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

inline void WriteFile(const std::string& path, const std::string& text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

inline bool WaitFor(const std::function<bool()>& pred, int timeoutMs = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

inline int64_t SteadyMs() {
    return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string TempDir(const std::string& tag);

void Group(const char* name);

}  // namespace t
