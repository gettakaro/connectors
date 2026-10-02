#include "http.h"

#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace {
constexpr size_t kMaxResponse = 1 << 20;

int ConnectWithTimeout(const HttpUrl& url, int timeoutMs, std::string& err) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port = std::to_string(url.port);
    int rc = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        err = std::string("resolve: ") + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int so = 0;
            socklen_t len = sizeof so;
            if (poll(&p, 1, timeoutMs) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &len) == 0 && so == 0) break;
            err = so ? strerror(so) : "connect timeout";
        } else {
            err = strerror(errno);
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}
}  // namespace

HttpResult HttpRequest(const HttpUrl& url, const char* method, const std::string& path, const std::string& body,
                       int timeoutMs) {
    HttpResult r;
    int fd = ConnectWithTimeout(url, timeoutMs, r.error);
    if (fd < 0) return r;
    std::string req = std::string(method) + " " + url.basePath + path + " HTTP/1.1\r\nHost: " + url.host + ":" +
                      std::to_string(url.port) + "\r\nX-Takaro-Mod-Source: " + kModSource +
                      "\r\nUser-Agent: " + kModSource + "/" TAKARO_CONAN_NATIVE_VERSION +
                      "\r\nAccept: application/json\r\nConnection: close\r\n";
    if (!body.empty() || strcmp(method, "POST") == 0)
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;

    const uint64_t deadline = NowMs() + (uint64_t)timeoutMs;
    auto remaining = [&] { uint64_t now = NowMs(); return now >= deadline ? 0 : (int)(deadline - now); };
    for (size_t sent = 0; sent < req.size();) {
        ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        pollfd p{fd, POLLOUT, 0};
        if (n < 0 && errno == EAGAIN && remaining() > 0 && poll(&p, 1, remaining()) == 1) continue;
        r.error = "send failed";
        close(fd);
        return r;
    }
    std::string raw;
    char buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
            raw.append(buf, (size_t)n);
            if (raw.size() > kMaxResponse) { r.error = "response too large"; break; }
            continue;
        }
        if (n == 0) break;
        if (errno == EINTR) continue;
        pollfd p{fd, POLLIN, 0};
        if (errno == EAGAIN && remaining() > 0 && poll(&p, 1, remaining()) == 1) continue;
        r.error = errno == EAGAIN ? "response timeout" : strerror(errno);
        break;
    }
    close(fd);
    if (r.error.empty()) {
        r.ok = ParseHttpResponse(raw, r.status, r.body);
        if (!r.ok) r.error = "malformed response";
    }
    return r;
}
