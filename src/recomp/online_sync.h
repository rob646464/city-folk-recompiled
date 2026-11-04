#pragma once
// Settings while visiting (docs/ONLINE.md "Settings while visiting"): a visitor's game uses the
// host's world settings (hemisphere, region content, weather place, grass wear, Nintendo events)
// for the visit and gets its own back when the visit ends; settings.ini never holds the host's.
// Skin tones travel both ways so each player's own tone shows on every game.
#include <cstdint>
#include <string>

namespace recomp::online::sync {

// Format version of the blob (first line "cfsync <version>").
constexpr int kVersion = 1;

// Whether an ini key follows the host while visiting.
bool synced(const std::string& section, const std::string& key);

// The host's blob for a visitor (synced settings + tones), and a visitor's (tones only).
std::string hostBlob();
std::string toneBlob();

// A blob arrived from room member `member` (`fromServer`: the host of the match we joined).
void onBlob(uint8_t member, const std::string& text, bool fromServer, const std::string& senderName);
// The match ended (we were the visitor: own settings back; tones of others dropped).
void matchEnded();
// A member left the match: its tone no longer needed.
void memberLeft(uint8_t member);

// F1: the synced options are locked while visiting.
bool locked();
std::string lockNote();  // "Using <host>'s settings while visiting"
// Draws the note and starts a disabled block when locked; returns whether it did (pass to lockEnd).
bool lockBegin();
void lockEnd(bool began);

}  // namespace recomp::online::sync
