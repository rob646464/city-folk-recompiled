#pragma once
// A small TURN server inside the test process (RFC 5766 over UDP, long-term credentials), the
// stand-in for a real TURN service in tests of src/recomp/online_turn.* and of relayed rooms.
// Loopback use only: no rate limits, one user, at most 48 allocations.
#include <cstdint>
#include <memory>
#include <string>

#include "recomp/online_sock.h"

struct TurnTestServer {
    TurnTestServer();
    ~TurnTestServer();  // stop()
    TurnTestServer(const TurnTestServer&) = delete;
    TurnTestServer& operator=(const TurnTestServer&) = delete;

    // Binds UDP on bindIp:port (port 0 = any; e.g. "127.0.0.9") and starts its thread. Relay
    // sockets bind on the same IP.
    bool start(const std::string& bindIp, uint16_t port, const std::string& user, const std::string& pass, std::string* err);
    void stop();
    online::sock::Addr addr() const;     // where it listens
    uint64_t relayedBytes() const;       // payload bytes forwarded in both directions
    int allocations() const;             // live allocations
    void setLifetimeLimit(int seconds);  // max lifetime granted (tests of refresh), default 600

    // More knobs for tests.
    void setPermissionLifetime(int seconds);  // default 300
    void rotateNonce();                       // requests with the old nonce now get 438 Stale Nonce
    void dropRequests(int n);                 // the next n requests get no answer (retransmission tests)
    uint64_t requests() const;                // requests received (dropped ones included)
    uint64_t droppedDatagrams() const;        // peer or Send-indication data refused for a missing permission

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
