// Shared helpers for the Takaro VEIN plugin (Linux LD_PRELOAD .so).
// Our own code, ported from the Takaro Enshrouded plugin skeleton to POSIX.
#pragma once

#include <pthread.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#ifdef TAKARO_DEBUG_CORRUPT_SIG
#define TAKARO_PLUGIN_VERSION "0.1.0-debug-corrupt-" TAKARO_DEBUG_CORRUPT_SIG
#else
#define TAKARO_PLUGIN_VERSION "0.2.3" // x-release-please-version
#endif

// Random per-process id (hex), so clients can tell a server restart from a cursor that merely lags.
const std::string& BootId();

// ---- locking ----
class Mutex {
public:
    Mutex() { pthread_mutex_init(&m_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t* raw() { return &m_; }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
private:
    pthread_mutex_t m_;
};
class Guard {
public:
    explicit Guard(Mutex& l) : l_(l) { l_.lock(); }
    ~Guard() { l_.unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    Mutex& l_;
};

// ---- plugin paths / logging / config ----
const std::string& ExePath();      // /proc/self/exe
const std::string& ExeDir();       // directory holding VeinServer-Linux-Test
const std::string& PluginDataDir();  // <ExeDir>/takaro, created on first use (TAKARO_PLUGIN_DATA_DIR overrides)

// Appends to <PluginDataDir>/plugin.log. Never calls into the game. Output is redacted.
void PluginLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool DebugEnabled();  // TAKARO_PLUGIN_DEBUG=1

// Config: env first, then <PluginDataDir>/plugin.json. Returns `def` when neither has it.
std::string ConfigValue(const char* envName, const char* jsonKey, const std::string& def = "");

// ---- redaction ----
// Masks WorldPassword / AdminPassword / Password= values (ini, JSON and CLI forms).
std::string Redact(const std::string& s);

// ---- time ----
std::string IsoNowUtc();
uint64_t NowMs();  // monotonic milliseconds

// ---- minimal JSON ----
std::string JsonEscape(const std::string& s);
inline std::string JsonStr(const std::string& s) { return "\"" + JsonEscape(s) + "\""; }
std::string JsonNum(double v);

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

// Reads a whole file; returns false if it cannot be opened.
bool ReadFile(const std::string& path, std::string& out);
bool WriteFileAtomic(const std::string& path, const std::string& content);
