// Win32 file primitives for core/takaro/fileio.h, ported unchanged from the Enshrouded native
// connector (games/enshrouded/mod/src/native/fileio.cpp, proven under Proton). Not built yet:
// the Windows lane wires it into its build.
#include "takaro/fileio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace takaro {


static std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ErrText(const char* what, const std::string& path) {
    return std::string(what) + " '" + path + "' failed (Win32 error " + std::to_string(GetLastError()) + ")";
}

bool ReadWholeFile(const std::string& path, std::string& text, bool& exists, std::string& err) {
    text.clear();
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            exists = false;
            return true;
        }
        err = ErrText("open", path);
        return false;
    }
    exists = true;
    char buf[65536];
    DWORD got = 0;
    bool ok = true;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr)) {
            ok = false;
            break;
        }
        if (!got) break;
        text.append(buf, got);
    }
    CloseHandle(h);
    if (!ok) err = ErrText("read", path);
    return ok;
}

bool AtomicWriteFile(const std::string& path, const std::string& text, std::string& err) {
    std::string tmp = path + ".tmp";
    HANDLE h = CreateFileW(W(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = ErrText("create", tmp);
        return false;
    }
    size_t off = 0;
    while (off < text.size()) {
        DWORD put = 0;
        DWORD chunk = (DWORD)std::min<size_t>(text.size() - off, 1 << 20);
        if (!WriteFile(h, text.data() + off, chunk, &put, nullptr) || put == 0) {
            err = ErrText("write", tmp);
            CloseHandle(h);
            DeleteFileW(W(tmp).c_str());
            return false;
        }
        off += put;
    }
    if (!FlushFileBuffers(h)) {
        err = ErrText("flush", tmp);
        CloseHandle(h);
        DeleteFileW(W(tmp).c_str());
        return false;
    }
    CloseHandle(h);
    if (!MoveFileExW(W(tmp).c_str(), W(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = ErrText("replace", path);
        DeleteFileW(W(tmp).c_str());
        return false;
    }
    return true;
}

bool EnsureDirectory(const std::string& dir, std::string& err) {
    if (dir.empty()) return true;
    std::wstring w = W(dir);
    for (size_t i = 1; i <= w.size(); i++) {
        if (i < w.size() && w[i] != L'\\' && w[i] != L'/') continue;
        std::wstring part = w.substr(0, i);
        if (part.size() == 2 && part[1] == L':') continue;  // drive root
        if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            DWORD attrs = GetFileAttributesW(part.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                err = ErrText("mkdir", dir);
                return false;
            }
        }
    }
    return true;
}

bool RemoveFileIfExists(const std::string& path) { return DeleteFileW(W(path).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND; }

FileStat StatFile(const std::string& path) {
    FileStat st;
    HANDLE h = CreateFileW(W(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return st;
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info)) {
        st.exists = true;
        st.size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        char b[64];
        snprintf(b, sizeof b, "%lx:%lx:%lx", (unsigned long)info.dwVolumeSerialNumber,
                 (unsigned long)info.nFileIndexHigh, (unsigned long)info.nFileIndexLow);
        st.identity = b;
    }
    CloseHandle(h);
    return st;
}

bool ReadFileRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    bool ok = SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
    std::vector<char> buf(64 * 1024);
    while (ok && out.size() < length) {
        DWORD want = (DWORD)std::min<uint64_t>(buf.size(), length - out.size()), got = 0;
        if (!ReadFile(h, buf.data(), want, &got, nullptr)) ok = false;
        if (!got) break;
        out.append(buf.data(), got);
    }
    CloseHandle(h);
    return ok;
}

}  // namespace takaro
