#pragma once
// Abortive close of the TCP connection under a WinHTTP WebSocket.
//
// When the heartbeat declares the Takaro link dead, frames already handed to the socket can still
// sit in the kernel send buffer. A graceful close lets the kernel deliver them once the route comes
// back, while the outbox replays the same events on the next connection, so Takaro stores them
// twice. WinHTTP exposes no socket and no linger option, so the socket is found by its exact
// local/remote address pair (WINHTTP_OPTION_CONNECTION_INFO) and given SO_LINGER {1, 0}: the close
// that follows is a reset and the unsent bytes are discarded. The replay then delivers them once.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

namespace takaro {

struct TcpEndpoints {
    sockaddr_storage local{}, remote{};
    bool valid = false;
};

// Same address family, address and port. An IPv4-mapped IPv6 address equals its IPv4 form.
bool SameEndpoint(const sockaddr_storage& a, const sockaddr_storage& b);

// The connection's addresses from a WinHTTP request handle, after its response has been received.
bool EndpointsFromRequest(void* winhttpRequest, TcpEndpoints& out);

// The endpoints of a connected socket (getsockname / getpeername).
bool EndpointsFromSocket(SOCKET s, TcpEndpoints& out);

struct AbortResult {
    int matched = 0;          // sockets whose local and remote addresses equal the endpoints
    int handlesScanned = 0;
    std::string error;        // empty on success
};

// Scans this process's handle values (4 .. maxHandle, step 4) for the socket whose local and remote
// addresses equal `ep` and sets SO_LINGER {1, 0} on it, so its next closesocket sends a reset. Stops at
// the first match.
// Read-only for every other handle: getsockname/getpeername only.
AbortResult MakeCloseAbortive(const TcpEndpoints& ep, uintptr_t maxHandle = 0x100000);

std::string EndpointText(const sockaddr_storage& a);

}  // namespace takaro
