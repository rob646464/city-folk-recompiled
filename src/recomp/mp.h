#pragma once
// DS wireless for Wii games: the RevoEX MP (multi-player data, like the DS's MP) and MPDL (DS
// Download Play) libraries answered on the host (hle_mp.cpp; docs/MOVE_IN.md "DS wireless").
// The PC has no DS radio, so the DS side is a virtual child the game code provides
// (src/game/cf_move_in.cpp: the Moving Van, City Folk's Wild World move-in program).
#include "recomp/ctx.h"

#include <vector>

namespace recomp::mp {

// One DS at a time, aid 1 (the parent is aid 0).
struct Child {
    virtual ~Child() = default;
    // MPDL started: the Wii offers the program with this game group id (ggid).
    virtual void downloadStarted(u32 ggid) = 0;
    virtual void downloadEnded() = 0;
    // A DS has received the offered program (MPDL then boots it).
    virtual bool hasProgram(u32 ggid) = 0;
    // MP started with ggid: the booted program connects as aid 1 when this is true.
    virtual bool connects(u32 ggid) = 0;
    // Data the parent sent to the child.
    virtual void received(const u8* data, u32 len) = 0;
    // Next packet to the parent (false: none now). Called a few times per frame.
    virtual bool nextPacket(std::vector<u8>& out) = 0;
    // MP ended (cleanup or disconnect).
    virtual void disconnected() = 0;
};
void setChild(Child* child);

// Delivers pending MP indications (connect, data, send completion) to the game's callback.
// Called on a guest thread at a point where the game expects them (City Folk: its MP link poll).
void deliver(Ctx& c);

// Logging (CF_MP_LOG=1).
bool logOn();

}  // namespace recomp::mp
