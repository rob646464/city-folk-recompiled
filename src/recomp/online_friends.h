#pragma once
// F1 friends (docs/ONLINE.md "Friends and mail"): an Ed25519 key per install, a friend code from
// its public key, friend requests between room members (keys come over the room, proved by a
// signature), accept / decline / block, a friend list in <data>/online/friends.txt.
// Host thread (F1 / overlay); thread-safe.
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::online::friends {

enum class State { Friend, RequestSent, RequestReceived, Blocked };

struct Friend {
    std::string code;  // friend code (from the public key)
    std::string name;
    uint8_t pub[32] = {};
    uint32_t pid = 0;  // the DWC profile id the key gives (the game's friend code)
    State state = State::Friend;
    std::vector<uint8_t> gameInfo;
    uint8_t xpub[32] = {};  // its X25519 letters key (proved with its Ed25519 key)
    bool haveX = false;
    std::string email;      // its own mail address ("Always online mail"), learned in a room
    std::string endpoints;  // its letters link "ip:port;..." (learned in a room)  // the friend's own roster entry (0x78 bytes, kGameInfoSize) or empty
};

// The game's roster entry for this player (save friend info, 0x78 bytes: names, town; built by
// online.cpp from the loaded save). Sent to room members; friends' entries go into their rosters.
constexpr size_t kGameInfoSize = 0x78;
void setMyGameInfo(const std::vector<uint8_t>& info);

// This install's keys (online.cpp's identity seed) and codes.
const uint8_t* myPublicKey();
std::string myCode();
// Profile id for a public key (also this install's DWC profile id).
uint32_t pidOf(const uint8_t pub[32]);
std::string codeOf(const uint8_t pub[32]);

// Every F1 / overlay frame: room messages (identities, proofs, requests), new members.
void tick();

// A room member as seen by the friends module.
enum class Rel { Unknown, Verified, Friend, RequestSent, RequestReceived, Blocked };
Rel relation(uint8_t memberId);
std::string memberCode(uint8_t memberId);
void requestFriend(uint8_t memberId);

std::vector<Friend> list();
// Room members whose key is proved and whose roster entry (INFO) arrived, F1 friends and blocked
// players left out: the game gets them as friends while the room lasts (online.cpp rosterSync),
// which is what its gate needs for a visit. They do not become F1 friends.
std::vector<Friend> sessionMembers();
// A session member went into the game's roster: taken out again (pendingRosterRemovals) once
// that player is no longer in a room with us, unless an F1 friend by then.
void noteSessionRoster(uint32_t pid);
// The friend is in the room now with a proved key (its INFO may still be on its way).
bool inRoom(const std::string& code);
// Room member id of a friend's proved key (0xFF: not in the room).
uint8_t memberOf(const uint8_t pub[32]);
// A friend (state Friend) with this key / this Wii number.
bool isFriend(const uint8_t pub[32]);
bool friendByWiiNumber(uint64_t number, uint8_t pubOut[32]);
// Wii numbers (letters, docs/ONLINE.md "Letters"): 16 decimal digits from the key.
uint64_t wiiNumberOf(const uint8_t pub[32]);
uint64_t myWiiNumber();
// Letters through a relay: a friend's X25519 key, and the key two friends share for their letters
// (HMAC("cf-letter", X25519(mine, theirs))).
// Letters link (online_mail.cpp): this game's addresses, sent to friends in a room; a friend's.
void setMyEndpoints(const std::string& eps);
std::string myEndpointsText();
std::string friendEndpoints(const uint8_t pub[32]);
// A friend's letters link addresses learned from the link itself (an authenticated HELLO).
void setFriendEndpoints(const uint8_t pub[32], const std::string& eps);
bool friendXPub(const uint8_t pub[32], uint8_t out[32]);
void letterKey(const uint8_t theirX[32], uint8_t out[32]);
const uint8_t* myXPub();
// Friends' email addresses (online email letters): exchanged in rooms when email is on.
std::string friendEmail(const uint8_t pub[32]);
void setMyEmail(const std::string& address);
void accept(const std::string& code);
void decline(const std::string& code);
void block(const std::string& code);
void remove(const std::string& code);
// Friends removed or blocked in F1 whose entry the game's roster still has to lose (kept in
// friends.txt until done; online.cpp takes them out when a town save is loaded and offline).
std::vector<uint32_t> pendingRosterRemovals();
void rosterRemovalDone(uint32_t pid);

// Pending incoming requests for notices (code, name).
std::vector<Friend> incoming();

}  // namespace recomp::online::friends
