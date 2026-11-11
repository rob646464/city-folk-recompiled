#pragma once
// The lobby client (docs/ONLINE.md "The lobby"): the room list, a host's registration and the
// signaling between a joiner and a room's host, over the lobby's HTTP API (a Cloudflare Worker,
// tools/lobby_worker, or the self-hosted tools/lobby/cf_lobby.cpp). The lobby never carries game
// data: it lists rooms and passes the handshake of online_net.cpp along, which is opaque to it.
// Every network call runs on a thread of its own; nothing here blocks the caller.
#include "recomp/online_net.h"

#include <memory>
#include <string>
#include <vector>

namespace online::lobby {

using net::u32;
using net::u8;

// "https://host", "http://127.0.0.1:29920", "host" (https assumed): no trailing slash. Empty if unusable.
std::string normalizeUrl(const std::string& text);

struct Room {
    std::string id, name, host, town, game, ver, tag;
    int players = 0, max = 4, pingMs = -1, age = 0;
    bool lock = false, friends = false;
};

// What a host registers (names are cleaned by the lobby as well).
struct RoomInfo {
    std::string name, host, town, game, ver, tag;
    int players = 1, max = 4;
    bool lock = false, friends = false;
};

// The tag of a friends-only room: 4 random bytes + HMAC(host key, them)[0..4], as 16 hex. Only
// someone who knows the host's public key (a friend) can tell whose room it is.
std::string makeTag(const u8 hostPub[32]);
bool tagMatches(const std::string& tag, const u8 pub[32]);

// The room list. refresh() fetches in the background; state() is what the last fetch gave.
struct ListState {
    bool loading = false;
    bool everLoaded = false;
    double age = -1;       // seconds since the last answer (-1: none yet)
    std::string error;     // plain words when the last fetch failed
    std::string url;       // the lobby it came from
    bool relay = false;    // this lobby hands out relay credentials
    std::vector<Room> rooms;
};
void refresh(const std::string& url, const std::string& game);
ListState state();

// A host's connection to the lobby (one WebSocket): registers the room, keeps it listed, and is
// the Signaling the session uses for joiners' handshakes. Reconnects by itself.
class HostChannel : public net::Signaling {
public:
    static std::shared_ptr<HostChannel> start(const std::string& url, const RoomInfo& room);
    virtual void update(const RoomInfo& room) = 0;  // player count, name ... (sent when it differs)
    virtual void stop() = 0;                         // the room leaves the list at once
    // "Listed on <lobby>" / "Connecting to the lobby..." / why not.
    virtual std::string status() = 0;
    virtual bool listed() = 0;
};

// A joiner's signaling for one join attempt of room `roomId`.
std::shared_ptr<net::Signaling> joinChannel(const std::string& url, const std::string& roomId);

// Plain words for a lobby failure (HTTP status + its error JSON, or the transport error).
std::string errorText(int status, const std::string& body, const std::string& transportError);

}  // namespace online::lobby
