// Shared helpers for the Takaro Enshrouded plugin (dbghelp.dll proxy).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef TAKARO_DEBUG_CORRUPT_SIG
#define TAKARO_PLUGIN_VERSION "0.4.2-debug-corrupt-" TAKARO_DEBUG_CORRUPT_SIG
#else
#define TAKARO_PLUGIN_VERSION "0.4.2"
#endif

// Random per-process id (hex), so clients can tell a server restart from a cursor that merely lags.
const std::string& BootId();

// ---- locking (Win32 SRW; avoids depending on libc++ thread support) ----
class SrwLock {
public:
    void lock() { AcquireSRWLockExclusive(&l_); }
    void unlock() { ReleaseSRWLockExclusive(&l_); }
private:
    SRWLOCK l_ = SRWLOCK_INIT;
};
class Guard {
public:
    explicit Guard(SrwLock& l) : l_(l) { l_.lock(); }
    ~Guard() { l_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    SrwLock& l_;
};

// ---- plugin paths / logging ----
const std::string& PluginBaseDir();  // directory containing enshrouded_server.exe / dbghelp.dll
void PluginLog(const char* fmt, ...);  // appends to <base>\takaro\plugin.log (never calls into the game)

// ---- time ----
std::string IsoNowUtc();
uint64_t NowMs();  // monotonic

// ---- minimal JSON ----
std::string JsonEscape(const std::string& s);
inline std::string JsonStr(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;  // string value, or the raw source text of a Number (keeps 64-bit integers exact)
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const;
    bool isNum() const { return type == Number; }
    bool isStr() const { return type == String; }
};
// Returns false on malformed input.
bool JsonParse(const std::string& text, JsonValue& out);
