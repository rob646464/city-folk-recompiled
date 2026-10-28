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
};

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
void accept(const std::string& code);
void decline(const std::string& code);
void block(const std::string& code);
void remove(const std::string& code);

// Pending incoming requests for notices (code, name).
std::vector<Friend> incoming();

}  // namespace recomp::online::friends
