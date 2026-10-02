// Shared helpers for the Takaro Conan Exiles native library (Linux LD_PRELOAD .so).
// Logging and JSON follow the Takaro VEIN plugin (games/vein/mod/src/common.h).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#define TAKARO_CONAN_NATIVE_VERSION "1.1.0" // x-release-please-version

// Enqueues a bounded record. Hooks never open files or wait for disk I/O.
void NativeLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Background threads only: appends the queued records to the log file.
void FlushNativeLogs();
// Where FlushNativeLogs writes. Empty (the default) keeps records queued and drops the oldest.
void SetNativeLogPath(const std::string& path);

// Environment value, or `def` when unset or empty.
std::string EnvOr(const char* name, const std::string& def);

uint64_t NowMs();  // monotonic milliseconds

// ---- minimal JSON (same parser as the VEIN plugin) ----
std::string JsonEscape(const std::string& s);
inline std::string JsonStr(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;  // string value, or the raw source text of a Number
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const;
    bool isStr() const { return type == String; }
};
// Returns false on malformed input.
bool JsonParse(const std::string& text, JsonValue& out);
