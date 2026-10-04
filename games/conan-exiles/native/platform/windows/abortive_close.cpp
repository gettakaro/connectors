#include "abortive_close.h"

#include <windows.h>

#include <winhttp.h>

#include <cstdio>
#include <cstring>

namespace takaro {

namespace {

// The IPv4 address an IPv4-mapped IPv6 address (::ffff:a.b.c.d) carries, else false.
bool MappedV4(const sockaddr_in6& a6, sockaddr_in& out) {
    static const unsigned char prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(&a6.sin6_addr, prefix, sizeof prefix) != 0) return false;
    memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = a6.sin6_port;
    memcpy(&out.sin_addr, reinterpret_cast<const unsigned char*>(&a6.sin6_addr) + 12, 4);
    return true;
}

sockaddr_storage Normal(const sockaddr_storage& a) {
    sockaddr_storage n = a;
    if (a.ss_family == AF_INET6) {
        sockaddr_in v4;
        if (MappedV4(reinterpret_cast<const sockaddr_in6&>(a), v4)) {
            memset(&n, 0, sizeof n);
            memcpy(&n, &v4, sizeof v4);
        }
    }
    return n;
}

}  // namespace

bool SameEndpoint(const sockaddr_storage& x, const sockaddr_storage& y) {
    sockaddr_storage a = Normal(x), b = Normal(y);
    if (a.ss_family != b.ss_family) return false;
    if (a.ss_family == AF_INET) {
        auto& p = reinterpret_cast<const sockaddr_in&>(a);
        auto& q = reinterpret_cast<const sockaddr_in&>(b);
        return p.sin_port == q.sin_port && p.sin_addr.s_addr == q.sin_addr.s_addr;
    }
    if (a.ss_family == AF_INET6) {
        auto& p = reinterpret_cast<const sockaddr_in6&>(a);
        auto& q = reinterpret_cast<const sockaddr_in6&>(b);
        return p.sin6_port == q.sin6_port && memcmp(&p.sin6_addr, &q.sin6_addr, sizeof p.sin6_addr) == 0 &&
               p.sin6_scope_id == q.sin6_scope_id;
    }
    return false;
}

bool EndpointsFromRequest(void* req, TcpEndpoints& out) {
    WINHTTP_CONNECTION_INFO info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    DWORD len = sizeof info;
    out.valid = false;
    if (!WinHttpQueryOption((HINTERNET)req, WINHTTP_OPTION_CONNECTION_INFO, &info, &len)) return false;
    memcpy(&out.local, &info.LocalAddress, sizeof out.local);
    memcpy(&out.remote, &info.RemoteAddress, sizeof out.remote);
    out.valid = out.local.ss_family != 0 && out.remote.ss_family != 0;
    return out.valid;
}

bool EndpointsFromSocket(SOCKET s, TcpEndpoints& out) {
    int ll = (int)sizeof out.local, rl = (int)sizeof out.remote;
    out.valid = getsockname(s, (sockaddr*)&out.local, &ll) == 0 && getpeername(s, (sockaddr*)&out.remote, &rl) == 0;
    return out.valid;
}

AbortResult MakeCloseAbortive(const TcpEndpoints& ep, uintptr_t maxHandle) {
    AbortResult r;
    if (!ep.valid) {
        r.error = "connection addresses unknown";
        return r;
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        r.error = "WSAStartup failed";
        return r;
    }
    for (uintptr_t h = 4; h <= maxHandle; h += 4) {
        SOCKET s = (SOCKET)h;
        r.handlesScanned++;
        sockaddr_storage local{};
        int ll = (int)sizeof local;
        // A handle that is not a socket fails here with WSAENOTSOCK and is never touched.
        if (getsockname(s, (sockaddr*)&local, &ll) != 0 || !SameEndpoint(local, ep.local)) continue;
        sockaddr_storage remote{};
        int rl = (int)sizeof remote;
        if (getpeername(s, (sockaddr*)&remote, &rl) != 0 || !SameEndpoint(remote, ep.remote)) continue;
        linger lg;
        lg.l_onoff = 1;
        lg.l_linger = 0;
        if (setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof lg) != 0) {
            r.error = "setsockopt(SO_LINGER) failed: " + std::to_string(WSAGetLastError());
            continue;
        }
        r.matched++;
        break;  // a TCP address pair names one connection; stop scanning (about 5 s for all of it under Wine)
    }
    WSACleanup();
    if (!r.matched && r.error.empty()) r.error = "no socket with these addresses";
    if (r.matched) r.error.clear();
    return r;
}

std::string EndpointText(const sockaddr_storage& a) {
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (a.ss_family == AF_INET) {
        auto& p = reinterpret_cast<const sockaddr_in&>(a);
        inet_ntop(AF_INET, (void*)&p.sin_addr, host, sizeof host);
        port = ntohs(p.sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }
    if (a.ss_family == AF_INET6) {
        auto& p = reinterpret_cast<const sockaddr_in6&>(a);
        inet_ntop(AF_INET6, (void*)&p.sin6_addr, host, sizeof host);
        port = ntohs(p.sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }
    return "?";
}

}  // namespace takaro
