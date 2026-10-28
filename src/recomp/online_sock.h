#pragma once
// Minimal portable UDP/TCP socket helpers for online play (online_net.cpp, online_nat.cpp).
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace online::sock {
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
inline int lastError() { return WSAGetLastError(); }
inline void closeSocket(Socket s) { closesocket(s); }
inline bool wouldBlock(int e) { return e == WSAEWOULDBLOCK || e == WSAECONNRESET || e == WSAEMSGSIZE; }
inline bool setNonBlocking(Socket s) {
    u_long one = 1;
    return ioctlsocket(s, FIONBIO, &one) == 0;
}
}  // namespace online::sock
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
namespace online::sock {
using Socket = int;
constexpr Socket kInvalid = -1;
inline int lastError() { return errno; }
inline void closeSocket(Socket s) { ::close(s); }
inline bool wouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == ECONNREFUSED; }
inline bool setNonBlocking(Socket s) { return fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) == 0; }
}  // namespace online::sock
#endif

namespace online::sock {

bool startup();  // WSAStartup once (online_net.cpp)

struct Addr {
    sockaddr_storage ss{};
    socklen_t len = 0;
    bool valid() const { return len != 0; }
    int family() const { return ss.ss_family; }
    // IPv4 address of an IPv4 or v4-mapped IPv6 address (host order), else 0.
    uint32_t v4() const;
    uint16_t port() const;
    bool operator==(const Addr& o) const { return len == o.len && std::memcmp(&ss, &o.ss, len) == 0; }
    std::string text() const;  // "1.2.3.4:5" / "[::1]:5"
    std::string ipText() const;
    // Same address as seen by an IPv6 dual-stack socket (v4 -> v4-mapped), or unchanged.
    Addr forSocket(int sockFamily) const;
    Addr normalized() const;  // v4-mapped IPv6 -> IPv4 (for comparisons and display)
};

Addr makeV4(uint32_t ipHostOrder, uint16_t port);
Addr makeV6(const uint8_t ip[16], uint16_t port);
// "1.2.3.4:5", "[v6]:5", "host:5" (DNS), port defaulting to defPort.
bool resolve(const std::string& text, uint16_t defPort, Addr* out);

}  // namespace online::sock
