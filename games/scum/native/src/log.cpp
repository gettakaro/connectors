#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {
std::mutex g_mu;
std::string g_path;
FILE* g_file = nullptr;
size_t g_size = 0;
constexpr size_t kMaxBytes = 8u << 20;  // then rotate to <file>.1

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

void OpenLocked() {
    if (g_path.empty() || g_file) return;
    g_file = _wfopen(Wide(g_path).c_str(), L"ab");
    if (g_file) {
        fseek(g_file, 0, SEEK_END);
        g_size = (size_t)ftell(g_file);
    }
}

void RotateLocked() {
    if (!g_file || g_size < kMaxBytes) return;
    fclose(g_file);
    g_file = nullptr;
    std::wstring w = Wide(g_path), old = w + L".1";
    DeleteFileW(old.c_str());
    MoveFileW(w.c_str(), old.c_str());
    OpenLocked();
}
}  // namespace

void LogSetFile(const std::string& path) {
    std::lock_guard<std::mutex> g(g_mu);
    if (g_file) fclose(g_file);
    g_file = nullptr;
    g_path = path;
    OpenLocked();
}

void LogLine(const std::string& line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[40];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d:%02d.%03d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
    std::string out = ts + line;
    if (out.empty() || out.back() != '\n') out += '\n';
    std::lock_guard<std::mutex> g(g_mu);
    OutputDebugStringA(out.c_str());
    if (!g_file) {
        fputs(out.c_str(), stderr);
        return;
    }
    fwrite(out.data(), 1, out.size(), g_file);
    fflush(g_file);
    g_size += out.size();
    RotateLocked();
}

void PluginLog(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LogLine(buf);
}
