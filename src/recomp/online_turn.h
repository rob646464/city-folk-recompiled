#pragma once
// TURN client for online play: a relayed UDP address on a TURN server for players whose routers
// let nothing in (UDP transport, long-term credentials). Sans-IO: the caller owns the socket (the
// game's single UDP socket), feeds received datagrams in and sends the datagrams this produces.
// Written from RFC 8489 / 5389 (STUN), RFC 8656 / 5766 (TURN), RFC 1321 (MD5), RFC 3174 (SHA-1)
// and RFC 2104 (HMAC); no third-party code. Depends only on online_sock.h and online_crypto.h.
//
// Not thread safe: one thread, or the caller's lock around every call.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "recomp/online_sock.h"

namespace online::turn {

struct Creds {
    std::string username, password;
};

// Bytes a Send indication adds in front of / around a payload of n bytes (plus 0-3 bytes of
// padding to a multiple of 4): STUN header 20 + XOR-PEER-ADDRESS 12 + DATA header 4.
constexpr size_t kWrapOverhead = 36;
// Largest payload wrap() takes.
constexpr size_t kMaxPayload = 65000;

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Starts (or restarts) an allocation on `server` (UDP). Nothing is sent here: tick() produces
    // the datagrams. Permissions asked for with permit() are kept and installed again. An
    // allocation the server still holds for this socket (a restart without close(), or after a
    // crash) is released and replaced; when moving to another server call close() first.
    void start(const sock::Addr& server, const Creds& creds);
    bool started() const;  // between start() and close(), failed() included
    const sock::Addr& server() const;
    // Call often (every 10-50 ms) with a monotonic time in seconds. Appends datagrams to send to
    // server().
    void tick(double now, std::vector<std::vector<uint8_t>>& out);
    // Give it every datagram received from server(). Returns true when it was STUN/TURN traffic
    // of this client (consumed). When it carried data from a peer (Data indication), *peer is the
    // peer's address and *data its payload (else data is left empty). May append replies to out.
    // STUN answers to transactions that are not this client's return false (the caller's own
    // STUN binding request to the same host still reaches its parser).
    bool onDatagram(const uint8_t* p, size_t n, double now, sock::Addr* peer, std::vector<uint8_t>* data,
                    std::vector<std::vector<uint8_t>>& out);
    // Installs (and keeps refreshing, every 4 minutes) a permission for the peer's IP address.
    // Idempotent; may be called before ready(). IPv4 (or v4-mapped) peers only, others ignored.
    void permit(const sock::Addr& peer);
    // True once the server confirmed the permission for the peer's IP (and it has not run out).
    bool permitted(const sock::Addr& peer) const;
    // A datagram for server() that relays `data` to `peer` (Send indication; DONT-FRAGMENT not
    // used). Empty if not ready (or the peer is not IPv4, or n > kMaxPayload).
    std::vector<uint8_t> wrap(const sock::Addr& peer, const uint8_t* data, size_t n) const;
    bool ready() const;         // allocation succeeded and not expired
    bool failed() const;        // gave up (wrong credentials, no answer after retries, allocation lost)
    std::string error() const;  // plain words for the UI when failed()
    sock::Addr relayed() const; // XOR-RELAYED-ADDRESS (valid when ready())
    sock::Addr mapped() const;  // XOR-MAPPED-ADDRESS from the Allocate success (our public address as the TURN server sees it)
    // Refresh with lifetime 0 (appends the datagram); the client is then idle and its
    // permissions are forgotten.
    void close(std::vector<std::vector<uint8_t>>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// MD5, SHA-1 and HMAC-SHA1 known-answer tests + RFC 5769 sample checks (message integrity with
// short- and long-term credentials, fingerprint).
bool selfTest(std::string* failure = nullptr);

// The primitives STUN needs (also used by the test server, tools/tests/turn_standin.cpp). MD5
// and SHA-1 are here only because the protocol demands them; do not use them for anything new.
void md5(const void* data, size_t len, uint8_t out[16]);
void sha1(const void* data, size_t len, uint8_t out[20]);
void hmacSha1(const void* key, size_t keyLen, const void* data, size_t len, uint8_t out[20]);
// Two-part message without concatenating.
void hmacSha1(const void* key, size_t keyLen, const void* a, size_t aLen, const void* b, size_t bLen, uint8_t out[20]);
uint32_t crc32(const void* data, size_t len);  // ISO 3309 / zlib CRC-32
// Long-term credential key: MD5(username ":" realm ":" password).
void longTermKey(const std::string& username, const std::string& realm, const std::string& password, uint8_t out[16]);

}  // namespace online::turn
