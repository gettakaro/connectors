#include "instance_guard.h"

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace InstanceGuard {

std::vector<std::string> OtherCopies(const std::string& maps, const std::string& self) {
    std::vector<std::string> out;
    static const std::string kName = "/libtakaro-vein.so";
    size_t start = 0;
    while (start < maps.size()) {
        size_t end = maps.find('\n', start);
        if (end == std::string::npos) end = maps.size();
        std::string line = maps.substr(start, end - start);
        start = end + 1;
        size_t slash = line.find('/');
        if (slash == std::string::npos) continue;
        std::string path = line.substr(slash);
        if (path.size() >= kName.size() && path.compare(path.size() - kName.size(), kName.size(), kName) == 0 &&
            path != self && (out.empty() || out.back() != path))
            out.push_back(path);
    }
    return out;
}

bool IsGuardedCopy(const std::string& path) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!h) return false;
    bool guarded = dlsym(h, "takaro_vein_instance") != nullptr;
    dlclose(h);  // drops only the reference RTLD_NOLOAD added
    return guarded;
}

bool ClaimProcess(std::string* why) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (why) *why = std::string("process lock unavailable: ") + strerror(errno);
        return true;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    char name[64];
    int n = snprintf(name, sizeof name, "takaro-vein-plugin-%d", static_cast<int>(getpid()));
    memcpy(addr.sun_path + 1, name, static_cast<size_t>(n));  // leading NUL: abstract namespace
    socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0) return true;  // fd stays open until exit
    int err = errno;
    close(fd);
    if (err == EADDRINUSE) {
        if (why) *why = "another copy of the connector already runs in this process";
        return false;
    }
    if (why) *why = std::string("process lock unavailable: ") + strerror(err);
    return true;
}

}  // namespace InstanceGuard
