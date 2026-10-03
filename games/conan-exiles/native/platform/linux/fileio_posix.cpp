// POSIX file primitives for core/takaro/fileio.h (durable atomic replace: tmp, fsync, rename).
#include "takaro/fileio.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace takaro {


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

}  // namespace takaro
