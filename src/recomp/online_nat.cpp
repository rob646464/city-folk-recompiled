// Router port mapping (UPnP-IGD, NAT-PMP), local addresses and STUN for online rooms
// (online_nat.h). Written from the UPnP IGD:1 / WANIPConnection:1 specification, RFC 6886
// (NAT-PMP) and RFC 5389 (STUN).
#include "recomp/online_nat.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <fstream>
#include <net/if.h>
#endif

namespace online::nat {
namespace {

using namespace online::sock;

bool isPrivateV4(uint32_t v) {
    return (v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 22) == (100u << 2 | 1);  // 10/8, 172.16/12, 192.168/16, 100.64/10
}

std::mutex sUpnpMutex;
std::string sControlUrl, sServiceType;  // the router that accepted our mapping (for unmapPort)
uint32_t sGatewayV4 = 0;                // NAT-PMP gateway (for unmapPort)
std::string sMethod;

uint32_t gatewayV4() {
#if defined(_WIN32)
    ULONG len = 16 * 1024;
    std::vector<uint8_t> buf(len);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &len) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    }
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &len) != NO_ERROR) return 0;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            if (g->Address.lpSockaddr->sa_family != AF_INET) continue;
            const uint32_t v = ntohl(reinterpret_cast<sockaddr_in*>(g->Address.lpSockaddr)->sin_addr.s_addr);
            if (v) return v;
        }
    }
    return 0;
#else
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line);
    while (std::getline(f, line)) {
        char ifn[64];
        unsigned dest = 0, gw = 0, flags = 0;
        if (std::sscanf(line.c_str(), "%63s %x %x %x", ifn, &dest, &gw, &flags) == 4 && dest == 0 && gw)
            return ntohl(gw);
    }
    return 0;
#endif
}

// Blocking TCP request with timeouts; returns the whole response.
bool httpRequest(const std::string& url, const std::string& method, const std::string& extraHeaders,
                 const std::string& body, std::string* response) {
    if (url.rfind("http://", 0) != 0) return false;
    const size_t hs = 7, pe = url.find('/', hs);
    const std::string hostPort = url.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    const std::string path = pe == std::string::npos ? "/" : url.substr(pe);
    Addr a;
    if (!resolve(hostPort, 80, &a)) return false;
    Socket s = socket(a.family(), SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) return false;
#if defined(_WIN32)
    DWORD tmo = 3000;
#else
    timeval tmo{3, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    if (connect(s, reinterpret_cast<const sockaddr*>(&a.ss), a.len) != 0) {
        closeSocket(s);
        return false;
    }
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + hostPort + "\r\nConnection: close\r\n" + extraHeaders;
    if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    if (send(s, req.data(), int(req.size()), 0) != int(req.size())) {
        closeSocket(s);
        return false;
    }
    response->clear();
    char buf[4096];
    while (response->size() < 256 * 1024) {
        const int n = int(recv(s, buf, sizeof buf, 0));
        if (n <= 0) break;
        response->append(buf, size_t(n));
    }
    closeSocket(s);
    return !response->empty();
}

std::string tagValue(const std::string& xml, const std::string& tag, size_t from = 0, size_t* at = nullptr) {
    // Matches <tag> and <ns:tag>.
    size_t p = from;
    while (true) {
        p = xml.find(tag + ">", p);
        if (p == std::string::npos) return {};
        if (p > 0 && (xml[p - 1] == '<' || xml[p - 1] == ':')) break;
        p++;
    }
    const size_t start = p + tag.size() + 1;
    const size_t end = xml.find('<', start);
    if (end == std::string::npos) return {};
    if (at) *at = end;
    return xml.substr(start, end - start);
}

std::string soapCall(const std::string& control, const std::string& service, const std::string& action,
                     const std::string& args, bool* ok) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + service +
        "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string hdr = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action + "\"\r\n";
    std::string resp;
    *ok = httpRequest(control, "POST", hdr, body, &resp) && resp.find(" 200 ") != std::string::npos;
    return resp;
}

std::string ipText(uint32_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
    return b;
}

MapResult upnpMap(uint16_t port, uint32_t lanIp) {
    MapResult r;
    r.method = "UPnP";
    startup();
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        r.error = "no socket";
        return r;
    }
    // Send from the LAN interface so the router answers there.
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(lanIp);
    bind(s, reinterpret_cast<sockaddr*>(&me), sizeof me);
    setNonBlocking(s);
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANPPPConnection:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    const Addr mc = makeV4(0xEFFFFFFAu, 1900);
    for (const char* st : targets) {
        const std::string q = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: ") + st +
                              "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n";
        sendto(s, q.data(), int(q.size()), 0, reinterpret_cast<const sockaddr*>(&mc.ss), mc.len);
    }
    std::vector<std::string> locations;
    const auto t0 = std::chrono::steady_clock::now();
    char buf[2048];
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2500)) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{0, 100000};
        if (select(int(s + 1), &fds, nullptr, nullptr, &tv) <= 0) continue;
        const int n = int(recv(s, buf, sizeof buf - 1, 0));
        if (n <= 0) continue;
        buf[n] = 0;
        std::string resp(buf);
        for (auto& c : resp) c = char(std::tolower(static_cast<unsigned char>(c)));
        const size_t p = resp.find("\nlocation:");
        if (p == std::string::npos) continue;
        size_t v = p + 10;
        while (v < resp.size() && resp[v] == ' ') v++;
        const size_t e = resp.find('\r', v);
        std::string loc(buf + v, e == std::string::npos ? std::strlen(buf + v) : e - v);  // original case
        bool seen = false;
        for (auto& l : locations) seen |= l == loc;
        if (!seen) locations.push_back(loc);
        if (locations.size() >= 4) break;
    }
    closeSocket(s);
    if (locations.empty()) {
        r.error = "no UPnP router answered (UPnP off or not supported)";
        return r;
    }
    for (const auto& loc : locations) {
        std::string desc;
        if (!httpRequest(loc, "GET", "", "", &desc)) continue;
        const size_t bodyAt = desc.find("\r\n\r\n");
        const std::string xml = bodyAt == std::string::npos ? desc : desc.substr(bodyAt + 4);
        std::string base = tagValue(xml, "URLBase");
        if (base.empty()) {
            const size_t pe = loc.find('/', 7);
            base = pe == std::string::npos ? loc : loc.substr(0, pe);
        }
        while (!base.empty() && base.back() == '/') base.pop_back();
        const char* services[] = {"urn:schemas-upnp-org:service:WANIPConnection:1",
                                  "urn:schemas-upnp-org:service:WANIPConnection:2",
                                  "urn:schemas-upnp-org:service:WANPPPConnection:1"};
        for (const char* svc : services) {
            const size_t sp = xml.find(svc);
            if (sp == std::string::npos) continue;
            std::string control = tagValue(xml, "controlURL", sp);
            if (control.empty()) continue;
            if (control.rfind("http", 0) != 0) control = base + (control[0] == '/' ? "" : "/") + control;
            const std::string ps = std::to_string(port);
            bool ok = false;
            std::string resp;
            for (int lease : {14400, 0}) {
                resp = soapCall(control, svc, "AddPortMapping",
                                "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + ps +
                                    "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + ps +
                                    "</NewInternalPort><NewInternalClient>" + ipText(lanIp) +
                                    "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>"
                                    "City Folk Recompiled</NewPortMappingDescription><NewLeaseDuration>" +
                                    std::to_string(lease) + "</NewLeaseDuration>",
                                &ok);
                if (ok) break;
            }
            if (!ok) {
                const std::string code = tagValue(resp, "errorCode");
                r.error = "the router refused the port mapping" + (code.empty() ? std::string() : " (UPnP error " + code + ")");
                continue;
            }
            bool ok2 = false;
            const std::string ipResp = soapCall(control, svc, "GetExternalIPAddress", "", &ok2);
            r.externalIp = tagValue(ipResp, "NewExternalIPAddress");
            r.externalPort = port;
            r.ok = true;
            std::lock_guard<std::mutex> g(sUpnpMutex);
            sControlUrl = control;
            sServiceType = svc;
            sMethod = "UPnP";
            if (r.externalIp.empty()) r.externalIp = "?";
            else {
                // A router behind another router (carrier-grade NAT) reports a private address.
                Addr ea;
                if (resolve(r.externalIp + ":1", 1, &ea) && isPrivateV4(ea.v4()))
                    r.error = "the router's own internet address " + r.externalIp + " is private (double NAT / CGNAT)";
            }
            return r;
        }
    }
    if (r.error.empty()) r.error = "no WANIPConnection service on the UPnP router";
    return r;
}

MapResult natpmpMap(uint16_t port, uint32_t gw, uint32_t lifetime) {
    MapResult r;
    r.method = "NAT-PMP";
    if (!gw) {
        r.error = "no default gateway";
        return r;
    }
    Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) return r;
    setNonBlocking(s);
    const Addr to = makeV4(gw, 5351);
    auto ask = [&](const std::vector<uint8_t>& req, uint8_t* resp, size_t want) -> int {
        for (int tries = 0, wait = 250; tries < 4; tries++, wait *= 2) {
            sendto(s, reinterpret_cast<const char*>(req.data()), int(req.size()), 0, reinterpret_cast<const sockaddr*>(&to.ss), to.len);
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            timeval tv{0, wait * 1000};
            if (select(int(s + 1), &fds, nullptr, nullptr, &tv) <= 0) continue;
            const int n = int(recv(s, reinterpret_cast<char*>(resp), int(want), 0));
            if (n >= 8) return n;
        }
        return -1;
    };
    uint8_t resp[16] = {};
    const std::vector<uint8_t> extReq = {0, 0};
    uint32_t ext = 0;
    if (ask(extReq, resp, 12) >= 12 && resp[1] == 128 && resp[2] == 0 && resp[3] == 0)
        ext = uint32_t(resp[8]) << 24 | uint32_t(resp[9]) << 16 | uint32_t(resp[10]) << 8 | resp[11];
    const std::vector<uint8_t> mapReq = {0, 1, 0, 0, uint8_t(port >> 8), uint8_t(port), uint8_t(port >> 8), uint8_t(port),
                                         uint8_t(lifetime >> 24), uint8_t(lifetime >> 16), uint8_t(lifetime >> 8), uint8_t(lifetime)};
    const int n = ask(mapReq, resp, 16);
    closeSocket(s);
    if (n < 16 || resp[1] != 129) {
        r.error = "no NAT-PMP / PCP answer from the router";
        return r;
    }
    const int result = resp[2] << 8 | resp[3];
    if (result != 0) {
        r.error = "NAT-PMP refused (result " + std::to_string(result) + ")";
        return r;
    }
    r.ok = true;
    r.externalPort = uint16_t(resp[10] << 8 | resp[11]);
    r.externalIp = ext ? ipText(ext) : "?";
    std::lock_guard<std::mutex> g(sUpnpMutex);
    sGatewayV4 = gw;
    sMethod = "NAT-PMP";
    return r;
}

}  // namespace

std::vector<LocalAddr> localAddresses() {
    startup();
    std::vector<LocalAddr> out;
#if defined(_WIN32)
    ULONG len = 16 * 1024;
    std::vector<uint8_t> buf(len);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, aa, &len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, aa, &len);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = aa; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const bool hasGateway = a->FirstGatewayAddress != nullptr;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            const sockaddr* sa = u->Address.lpSockaddr;
            LocalAddr la;
            if (sa->sa_family == AF_INET) {
                la.v4 = ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr);
                la.ip = ipText(la.v4);
                la.global = (la.v4 >> 16) != 0xA9FE && hasGateway;  // not 169.254/16
            } else if (sa->sa_family == AF_INET6) {
                la.v6 = true;
                std::memcpy(la.v6bytes, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, 16);
                la.global = (la.v6bytes[0] & 0xE0) == 0x20 && u->DadState == IpDadStatePreferred;  // 2000::/3
                char b[64] = {};
                inet_ntop(AF_INET6, la.v6bytes, b, sizeof b);
                la.ip = b;
            } else {
                continue;
            }
            out.push_back(la);
        }
    }
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        LocalAddr la;
        if (i->ifa_addr->sa_family == AF_INET) {
            la.v4 = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
            la.ip = ipText(la.v4);
            la.global = (la.v4 >> 16) != 0xA9FE;
        } else if (i->ifa_addr->sa_family == AF_INET6) {
            la.v6 = true;
            std::memcpy(la.v6bytes, &reinterpret_cast<sockaddr_in6*>(i->ifa_addr)->sin6_addr, 16);
            la.global = (la.v6bytes[0] & 0xE0) == 0x20;
            char b[64] = {};
            inet_ntop(AF_INET6, la.v6bytes, b, sizeof b);
            la.ip = b;
        } else {
            continue;
        }
        out.push_back(la);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

uint32_t defaultLanV4() {
    uint32_t any = 0;
    for (auto& a : localAddresses()) {
        if (a.v6 || !a.global) continue;
        if (isPrivateV4(a.v4)) return a.v4;
        if (!any) any = a.v4;
    }
    return any;
}

MapResult mapPort(uint16_t port, uint32_t lanIp) {
    if (!lanIp) {
        MapResult r;
        r.error = "no network connection found";
        return r;
    }
    MapResult u = upnpMap(port, lanIp);
    if (u.ok && u.error.empty()) return u;
    MapResult p = natpmpMap(port, gatewayV4(), 14400);
    if (p.ok) return p;
    if (u.ok) return u;  // mapped, but behind a second NAT (error says so)
    MapResult r;
    r.error = u.error + "; " + p.error;
    return r;
}

void unmapPort(uint16_t port) {
    std::string control, service, method;
    uint32_t gw = 0;
    {
        std::lock_guard<std::mutex> g(sUpnpMutex);
        control = sControlUrl;
        service = sServiceType;
        method = sMethod;
        gw = sGatewayV4;
    }
    if (method == "UPnP" && !control.empty()) {
        bool ok = false;
        soapCall(control, service, "DeletePortMapping",
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
                     "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 &ok);
        std::printf("[online] UPnP mapping of UDP %u %s\n", port, ok ? "removed" : "not removed (router did not answer)");
    } else if (method == "NAT-PMP" && gw) {
        natpmpMap(port, gw, 0);  // lifetime 0 deletes
    }
}

std::vector<uint8_t> stunRequest(uint8_t txid[12]) {
    std::vector<uint8_t> b = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    b.insert(b.end(), txid, txid + 12);
    return b;
}

bool stunParse(const uint8_t* p, size_t n, const uint8_t txid[12], sock::Addr* mapped) {
    if (n < 20 || p[0] != 0x01 || p[1] != 0x01 || std::memcmp(p + 8, txid, 12) != 0) return false;
    const size_t len = size_t(p[2] << 8 | p[3]);
    size_t i = 20;
    bool found = false;
    while (i + 4 <= n && i + 4 <= 20 + len) {
        const int type = p[i] << 8 | p[i + 1];
        const size_t alen = size_t(p[i + 2] << 8 | p[i + 3]);
        const uint8_t* v = p + i + 4;
        if (i + 4 + alen > n) break;
        if ((type == 0x0020 || (type == 0x0001 && !found)) && alen >= 8) {
            const bool x = type == 0x0020;
            const int fam = v[1];
            uint16_t port = uint16_t(v[2] << 8 | v[3]);
            if (x) port ^= 0x2112;
            if (fam == 1) {
                uint32_t ip = uint32_t(v[4]) << 24 | uint32_t(v[5]) << 16 | uint32_t(v[6]) << 8 | v[7];
                if (x) ip ^= 0x2112A442u;
                *mapped = makeV4(ip, port);
                found = true;
            } else if (fam == 2 && alen >= 20) {
                uint8_t ip[16];
                std::memcpy(ip, v + 4, 16);
                if (x) {
                    const uint8_t mask[16] = {0x21, 0x12, 0xA4, 0x42, txid[0], txid[1], txid[2], txid[3], txid[4], txid[5],
                                              txid[6], txid[7], txid[8], txid[9], txid[10], txid[11]};
                    for (int k = 0; k < 16; k++) ip[k] ^= mask[k];
                }
                *mapped = makeV6(ip, port);
                found = true;
            }
            if (x && found) return true;
        }
        i += 4 + ((alen + 3) & ~size_t(3));
    }
    return found;
}

const std::vector<std::string>& stunServers() {
    static const std::vector<std::string> s = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
    return s;
}

}  // namespace online::nat
