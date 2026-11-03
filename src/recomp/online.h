#pragma once
// Online play (docs/ONLINE.md): rooms between players of this port, F1 > Online, and the glue
// between the room transport (online_net.h) and the game's DWC calls (online_dwc.cpp).
#include "recomp/ctx.h"

#include <string>

namespace recomp::online {

// F1 > Online tab (menu.cpp); true when a setting changed (saved by the menu).
bool drawTab();
// Notices over the game (join requests, room events): menu.cpp, inside the Aurora frame.
void drawOverlay();
// In a room (hosting or joined): the game may connect (SO/NWC24/DWC answer as online).
bool active();
// This install's DWC profile id (stable; derived from the identity file), and the game's
// 12-digit friend code for it once the game logged in (0 before).
u32 profileId();
// This install's Wii number (letters; from its identity key, stable): what nwc24msg.cfg holds.
u64 wiiNumber();
u64 friendKey();
void setFriendKey(u64 key);
// This install's 32-byte Ed25519 seed (from <data>/online/identity.bin; never leaves the PC).
const u8* identitySeed();
// A notice over the game for a few seconds (room / friend events).
void postNotice(const std::string& text);
// Player name shown in rooms (F1 setting, else "Player").
std::string playerName();
// Guest frame hook (cf_game.cpp onGuestFrame): CF_ONLINE_TEST actions on the game thread.
void onGuestFrame(Ctx& c);
// DWC state for the F1 tab (online_dwc.cpp).
std::string dwcStatusText();
// CF_ONLINE_LOG=1: DWC calls and room events.
bool logOn();

}  // namespace recomp::online
