// Windows test of platform/windows/abortive_close: the dead Takaro connection's socket is found by its
// address pair and closes with a reset (unsent bytes discarded) instead of a graceful FIN, while every
// other socket keeps its graceful close. Built by platform/windows/build.sh --tests; runs on Windows
// or under Wine (CI).
#include "abortive_close.h"

#include <windows.h>

#include <wincrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace takaro;

static int failures = 0;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            printf("ok   %s\n", what);                                      \
        } else {                                                            \
            printf("FAIL %s (line %d, WSA %d)\n", what, __LINE__, WSAGetLastError()); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static sockaddr_storage V4(const char* ip, unsigned short port) {
    sockaddr_storage s{};
    auto& a = reinterpret_cast<sockaddr_in&>(s);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return s;
}

static sockaddr_storage V6(const char* ip, unsigned short port) {
    sockaddr_storage s{};
    auto& a = reinterpret_cast<sockaddr_in6&>(s);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return s;
}

static void TestSameEndpoint() {
    CHECK(SameEndpoint(V4("127.0.0.1", 443), V4("127.0.0.1", 443)), "same v4 address and port");
    CHECK(!SameEndpoint(V4("127.0.0.1", 443), V4("127.0.0.1", 444)), "different port");
    CHECK(!SameEndpoint(V4("127.0.0.1", 443), V4("127.0.0.2", 443)), "different v4 address");
    CHECK(SameEndpoint(V6("::ffff:10.1.2.3", 443), V4("10.1.2.3", 443)), "v4-mapped v6 equals its v4 form");
    CHECK(!SameEndpoint(V6("::1", 443), V4("127.0.0.1", 443)), "::1 is not 127.0.0.1");
    CHECK(SameEndpoint(V6("2001:db8::1", 8443), V6("2001:db8::1", 8443)), "same v6 address and port");
    sockaddr_storage none{};
    CHECK(!SameEndpoint(none, none), "an empty address matches nothing");
}

static SOCKET Listen(unsigned short& port) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_storage a = V4("127.0.0.1", 0);
    bind(l, (sockaddr*)&a, sizeof(sockaddr_in));
    listen(l, 8);
    int len = sizeof a;
    getsockname(l, (sockaddr*)&a, &len);
    port = ntohs(reinterpret_cast<sockaddr_in&>(a).sin_port);
    return l;
}

static SOCKET Connect(unsigned short port) {
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_storage a = V4("127.0.0.1", port);
    connect(c, (sockaddr*)&a, sizeof(sockaddr_in));
    return c;
}

// How the peer saw the close: "reset", "fin", or "timeout".
static std::string PeerSees(SOCKET s) {
    DWORD ms = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    char buf[4096];
    for (;;) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n > 0) continue;  // drain whatever arrived before the close
        if (n == 0) return "fin";
        int e = WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAECONNABORTED) return "reset";
        return "timeout (" + std::to_string(e) + ")";
    }
}

static void TestRawSockets() {
    unsigned short port = 0;
    SOCKET l = Listen(port);
    SOCKET cDead = Connect(port), sDead = accept(l, nullptr, nullptr);
    SOCKET cLive = Connect(port), sLive = accept(l, nullptr, nullptr);
    CHECK(cDead != INVALID_SOCKET && sDead != INVALID_SOCKET && cLive != INVALID_SOCKET && sLive != INVALID_SOCKET,
          "loopback connections");

    TcpEndpoints dead;
    CHECK(EndpointsFromSocket(cDead, dead), "endpoints of the dead connection");
    AbortResult r = MakeCloseAbortive(dead);
    printf("     scanned %d handles, matched %d, error '%s'\n", r.handlesScanned, r.matched, r.error.c_str());
    CHECK(r.matched == 1 && r.error.empty(), "exactly the dead connection's socket is found");

    const char data[] = "{\"type\":\"gameEvent\"}";
    send(cDead, data, sizeof data - 1, 0);
    closesocket(cDead);
    closesocket(cLive);
    CHECK(PeerSees(sDead) == "reset", "the dead connection closes with a reset");
    CHECK(PeerSees(sLive) == "fin", "another connection to the same port still closes gracefully");

    // The same addresses again, now that no socket holds them: nothing is changed.
    AbortResult gone = MakeCloseAbortive(dead);
    CHECK(gone.matched == 0 && !gone.error.empty(), "a closed connection matches no socket");
    TcpEndpoints unknown;
    CHECK(MakeCloseAbortive(unknown).matched == 0, "unknown endpoints change nothing");
    closesocket(sDead);
    closesocket(sLive);
    closesocket(l);
}

// The WebSocket key's accept value (RFC 6455): base64(SHA-1(key + GUID)).
static std::string AcceptFor(const std::string& key) {
    std::string in = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[20];
    DWORD len = sizeof digest;
    CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash);
    CryptHashData(hash, (const BYTE*)in.data(), (DWORD)in.size(), 0);
    CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    char out[64];
    DWORD outLen = sizeof out;
    CryptBinaryToStringA(digest, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &outLen);
    return std::string(out, outLen);
}

// A WinHTTP WebSocket to a loopback server, set up the way the transport does it. With `abortive`, the
// connection is marked through WINHTTP_OPTION_CONNECTION_INFO before the WebSocket handle is closed.
// Returns how the server saw the close.
static std::string WinHttpWebSocketClose(bool abortive) {
    unsigned short port = 0;
    SOCKET l = Listen(port);
    SOCKET served = INVALID_SOCKET;
    std::thread server([&] {
        served = accept(l, nullptr, nullptr);
        char buf[4096];
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            int n = recv(served, buf, sizeof buf, 0);
            if (n <= 0) return;
            req.append(buf, n);
        }
        std::string key;
        size_t k = req.find("Sec-WebSocket-Key:");
        if (k == std::string::npos) k = req.find("sec-websocket-key:");
        if (k != std::string::npos) {
            size_t b = req.find_first_not_of(' ', k + 18), e = req.find("\r\n", k);
            key = req.substr(b, e - b);
        }
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " + AcceptFor(key) + "\r\n\r\n";
        send(served, resp.data(), (int)resp.size(), 0);
    });
    HINTERNET sess = WinHttpOpen(L"abortive-close-test", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = sess ? WinHttpConnect(sess, L"127.0.0.1", port, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
                         : nullptr;
    bool ok = req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
              WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    server.join();
    CHECK(ok, abortive ? "WinHTTP WebSocket upgrade (abortive run)" : "WinHTTP WebSocket upgrade (control run)");
    TcpEndpoints ep;
    bool gotEp = ok && EndpointsFromRequest(req, ep);
    HINTERNET ws = ok ? WinHttpWebSocketCompleteUpgrade(req, 0) : nullptr;
    CHECK(ws != nullptr, "WebSocket handle");
    if (req) WinHttpCloseHandle(req);
    if (abortive) {
        CHECK(gotEp, "connection addresses from WinHTTP");
        sockaddr_storage peer{};
        int pl = sizeof peer;
        getpeername(served, (sockaddr*)&peer, &pl);
        CHECK(SameEndpoint(ep.local, peer), "WinHTTP's local address is the server's peer address");
        CHECK(SameEndpoint(ep.remote, V4("127.0.0.1", port)), "WinHTTP's remote address is the server");
        ULONGLONG t0 = GetTickCount64();
        AbortResult r = MakeCloseAbortive(ep);
        printf("     scanned %d handles in %llums, matched %d, error '%s'\n", r.handlesScanned,
               (unsigned long long)(GetTickCount64() - t0), r.matched, r.error.c_str());
        CHECK(r.matched == 1, "WinHTTP's WebSocket socket is found by its addresses");
    }
    // An unsent frame, as on a dead link; then the close the transport does.
    if (ws) WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID) "{}", 2);
    if (ws) WinHttpCloseHandle(ws);
    if (conn) WinHttpCloseHandle(conn);
    if (sess) WinHttpCloseHandle(sess);
    std::string seen = served != INVALID_SOCKET ? PeerSees(served) : "no connection";
    if (served != INVALID_SOCKET) closesocket(served);
    closesocket(l);
    return seen;
}

static void TestWinHttpWebSocket() {
    // Information only: Windows 11's WinHTTP already resets here (measured 2026-10-04); a WinHTTP that
    // closes gracefully (the duplicate-delivery case) is what the marking guards against.
    std::string control = WinHttpWebSocketClose(false);
    printf("     control: closing the WebSocket handle unmarked -> %s\n", control.c_str());
    CHECK(control == "fin" || control == "reset", "unmarked, the connection still closes");
    CHECK(WinHttpWebSocketClose(true) == "reset", "marked, closing the WebSocket handle resets the connection");
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    TestSameEndpoint();
    TestRawSockets();
    TestWinHttpWebSocket();
    WSACleanup();
    printf(failures ? "abortive_close_test: %d FAILED\n" : "abortive_close_test: all passed\n", failures);
    return failures ? 1 : 0;
}
