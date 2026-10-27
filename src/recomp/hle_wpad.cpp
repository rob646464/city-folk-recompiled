// HLE: Wii Remote (WPAD), boot stand-in. The real KPAD/WPAD HLE (buttons, pointer from the
// mouse/right stick/gyro, Nunchuk, rumble, speaker) is the next phase (docs/WII.md "Input").
// Until then the Bluetooth stack (WUD/BTE, which waits for the Bluetooth USB device through IOS)
// is never started: WPADInit does nothing and every channel reads "no controller".
#include "recomp/hle.h"

namespace recomp {

// WPAD_STATE_SETUP: the library is up (games wait for it before reading).
constexpr u32 kWpadStateSetup = 3;
// WPAD_ERR_NO_CONTROLLER
constexpr s32 kWpadErrNoController = -1;

HLE(WPADInit) {}
HLE(WPADGetStatus) { c.r[3] = kWpadStateSetup; }
// WPADProbe(chan, u32* type): no controller on any channel.
HLE(WPADProbe) {
    if (c.r[4]) wr32(c.r[4], 0xFD);  // WPAD_DEV_NOT_FOUND
    c.r[3] = u32(kWpadErrNoController);
}

}  // namespace recomp
