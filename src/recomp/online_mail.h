#pragma once
// Letters between F1 friends (docs/ONLINE.md "Letters"): the game's WiiConnect24 letters to Wii
// friends, carried between players of this port instead of Nintendo's mail servers.
// - Each install has a Wii number from its key (friends::wiiNumberOf); roster entries carry the
//   friend's (INFO +0x00), so the game's letter code addresses them as Wii friends.
// - NWC24CommitMsg to such a number puts the letter in this install's outbox
//   (<data>/online/mail/out); room messages MAIL / MAIL_ACK deliver it once the friend is in the
//   same room (sealed by the room transport), the recipient keeps it in its inbox
//   (<data>/online/mail/in) and its game's WiiConnect24 receive (NWC24SearchMsgs / reading the
//   attachment / NWC24DeleteMsg) takes it like a letter from WiiConnect24.
#include <cstdint>
#include <string>
#include <vector>

namespace recomp::online::mail {

// Room messages 0x46 / 0x47 from member `from` (friends.cpp hands them over; `pub` = its proved
// key, null when not proved).
void onRoomMessage(uint8_t from, uint8_t type, const std::vector<uint8_t>& data, const uint8_t* pub);
// Host thread, with friends::tick: sends outbox letters to friends in the room.
void tick();

struct Counts {
    int outbox = 0, inbox = 0;
};
Counts counts();
// For the email path (online_email.cpp): outbox letters sealed for their friend (only friends whose
// letters key is known), sent letters leave the outbox; a sealed letter from an email is taken in
// when it opens and is a friend's (self: a test letter, reported only).
struct Sealed {
    uint64_t id = 0;
    uint8_t peer[32] = {};
    std::vector<uint8_t> blob;
};
std::vector<Sealed> sealedOutbox();
void sentByEmail(uint64_t id);
enum class Import { Taken, Duplicate, Self, NotFromAFriend, Broken };
Import importSealed(const uint8_t from[32], const std::vector<uint8_t>& blob);
Sealed sealTestForSelf();

// Test aid (CF_ONLINE_TEST=<f>:mailtest): a 64-byte dummy letter to every friend (not a game letter).
int queueTestLetter();

}  // namespace recomp::online::mail
