#include "native/fileio.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace native {

bool IsAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
}

static char SepFor(const std::string& dir) { return dir.find('\\') != std::string::npos ? '\\' : '/'; }

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + SepFor(dir) + name;
}

std::string DirName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s);
}

std::string ResolvePath(const std::string& baseDir, const std::string& p) {
    if (p.empty() || IsAbsolutePath(p)) return p;
    std::string rel = p;
    if (SepFor(baseDir) == '\\')
        for (auto& c : rel)
            if (c == '/') c = '\\';
    return JoinPath(baseDir, rel);
}

#ifdef _WIN32

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

#else  // POSIX (host tests)

bool ReadWholeFile(const std::string& path, std::string& text, bool& exists, std::string& err) {
    text.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (errno == ENOENT || errno == ENOTDIR) {
            exists = false;
            return true;
        }
        err = "open '" + path + "' failed: " + strerror(errno);
        return false;
    }
    exists = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) err = "read '" + path + "' failed";
    return ok;
}

bool AtomicWriteFile(const std::string& path, const std::string& text, std::string& err) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        err = "create '" + tmp + "' failed: " + strerror(errno);
        return false;
    }
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            err = "write '" + tmp + "' failed: " + strerror(errno);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        off += (size_t)n;
    }
    if (fsync(fd) != 0) {
        err = "fsync '" + tmp + "' failed: " + strerror(errno);
        close(fd);
        unlink(tmp.c_str());
        return false;
    }
    close(fd);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        err = "rename '" + path + "' failed: " + strerror(errno);
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool EnsureDirectory(const std::string& dir, std::string& err) {
    if (dir.empty()) return true;
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i < dir.size() && dir[i] != '/') continue;
        std::string part = dir.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            err = "mkdir '" + part + "' failed: " + strerror(errno);
            return false;
        }
    }
    return true;
}

bool RemoveFileIfExists(const std::string& path) { return unlink(path.c_str()) == 0 || errno == ENOENT; }

FileStat StatFile(const std::string& path) {
    FileStat st;
    struct stat s;
    if (stat(path.c_str(), &s) != 0) return st;
    st.exists = true;
    st.size = (uint64_t)s.st_size;
    st.identity = std::to_string((unsigned long long)s.st_dev) + ":" + std::to_string((unsigned long long)s.st_ino);
    return st;
}

bool ReadFileRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out) {
    out.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    std::vector<char> buf(64 * 1024);
    while (out.size() < length) {
        size_t want = (size_t)std::min<uint64_t>(buf.size(), length - out.size());
        size_t got = fread(buf.data(), 1, want, f);
        if (!got) break;
        out.append(buf.data(), got);
    }
    fclose(f);
    return true;
}

#endif

}  // namespace native
