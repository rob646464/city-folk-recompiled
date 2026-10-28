#pragma once
// Reaching a room host behind a home router (docs/ONLINE.md "Reachability"): local addresses,
// UPnP-IGD and NAT-PMP port mappings on the router, STUN (public address as seen from the
// internet, from a public STUN server). No server of ours is involved.
#include <cstdint>
#include <string>
#include <vector>

#include "recomp/online_sock.h"

namespace online::nat {

struct LocalAddr {
    std::string ip;
    bool v6 = false;
    bool global = false;  // IPv6 global unicast / IPv4 not loopback, not link-local
    uint32_t v4 = 0;
    uint8_t v6bytes[16] = {};
};
std::vector<LocalAddr> localAddresses();
uint32_t defaultLanV4();  // first private IPv4 (host order), 0 if none

struct MapResult {
    bool ok = false;
    std::string method;      // "UPnP" / "NAT-PMP"
    std::string externalIp;  // as the router reports it
    uint16_t externalPort = 0;
    std::string error;
};
// Maps UDP `port` on the router to this PC (lease renewed by calling again); blocking, a few
// seconds at most.
MapResult mapPort(uint16_t port, uint32_t lanIp);
void unmapPort(uint16_t port);

// STUN (RFC 5389) binding request/response, sent on the caller's own socket so the answer is
// the mapping of that socket.
std::vector<uint8_t> stunRequest(uint8_t txid[12]);
bool stunParse(const uint8_t* p, size_t n, const uint8_t txid[12], sock::Addr* mapped);
// Public STUN servers tried in order (not ours; any RFC 5389 server works).
const std::vector<std::string>& stunServers();

}  // namespace online::nat
