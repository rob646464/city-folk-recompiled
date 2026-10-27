// HLE: Wii Remote (WPAD/KPAD), boot stand-in. The real KPAD/WPAD HLE (pointer from the
// mouse/right stick/gyro, accelerometer, Nunchuk, rumble, speaker; docs/WII.md "Input") is the
// next phase. Until then:
//   - the Bluetooth stack (WUD/BTE, which waits for the Bluetooth USB device through IOS) is never
//     started: WPADInit and KPADInit do nothing;
//   - channel 0 is a Wii Remote whose buttons come from the host controller of port 1 (F1 >
//     Controls bindings and --input scripts): A -> A, B -> B, START -> +, Z -> -, X -> 1, Y -> 2,
//     D-pad -> D-pad. No pointer, no motion. The other channels have no controller.
#include "recomp/hle.h"

#include <dolphin/pad.h>

namespace recomp {
u32 readHostPads(PADStatus* st);  // hle_pad.cpp

namespace {
// WPAD button bits (RVL SDK wpad.h).
enum : u32 {
    kWpadLeft = 0x0001 << 8,
    kWpadRight = 0x0002 << 8,
    kWpadDown = 0x0004,
    kWpadUp = 0x0008,
    kWpadPlus = 0x0010,
    kWpad2 = 0x0001,
    kWpad1 = 0x0002,
    kWpadB = 0x0400,
    kWpadA = 0x0800,
    kWpadMinus = 0x1000,
    kWpadHome = 0x8000,
};
constexpr u32 kWpadStateSetup = 3;  // WPAD_STATE_SETUP: the library is up
constexpr s32 kWpadErrNone = 0, kWpadErrNoController = -1;
constexpr u32 kWpadDevCore = 0, kWpadDevNotFound = 0xFD;

u32 remoteButtons(const PADStatus& p) {
    if (p.err != PAD_ERR_NONE) return 0;
    const u16 b = p.button;
    u32 w = 0;
    if (b & PAD_BUTTON_A) w |= kWpadA;
    if (b & PAD_BUTTON_B) w |= kWpadB;
    if (b & PAD_BUTTON_START) w |= kWpadPlus;
    if (b & PAD_TRIGGER_Z) w |= kWpadMinus;
    if (b & PAD_BUTTON_X) w |= kWpad1;
    if (b & PAD_BUTTON_Y) w |= kWpad2;
    if (b & PAD_BUTTON_UP) w |= kWpadUp;
    if (b & PAD_BUTTON_DOWN) w |= kWpadDown;
    if (b & PAD_BUTTON_LEFT) w |= kWpadLeft;
    if (b & PAD_BUTTON_RIGHT) w |= kWpadRight;
    return w;
}
u32 sHold[4] = {};
}  // namespace

HLE(WPADInit) {}
HLE(KPADInit) {}
HLE(WPADGetStatus) { c.r[3] = kWpadStateSetup; }
// WPADProbe(chan, u32* type)
HLE(WPADProbe) {
    const bool present = c.r[3] == 0;
    if (c.r[4]) wr32(c.r[4], present ? kWpadDevCore : kWpadDevNotFound);
    c.r[3] = u32(present ? kWpadErrNone : kWpadErrNoController);
}

// KPADRead(chan, KPADStatus* buf, u32 count) -> samples written. KPADStatus (0x84 bytes): hold,
// trig, release, then motion/pointer fields (left zero: no pointer, dpd_valid_fg 0), dev_type at
// +0x5C, wpad_err at +0x5D.
HLE(KPADRead) {
    const u32 chan = c.r[3], buf = c.r[4], count = c.r[5];
    if (chan != 0 || !buf || !count) {
        c.r[3] = 0;
        return;
    }
    PADStatus st[PAD_MAX_CONTROLLERS] = {};
    readHostPads(st);
    const u32 hold = remoteButtons(st[0]);
    const u32 prev = sHold[chan];
    sHold[chan] = hold;
    for (u32 i = 0; i < 0x84; i += 4) wr32(buf + i, 0);
    wr32(buf + 0x00, hold);
    wr32(buf + 0x04, hold & ~prev);
    wr32(buf + 0x08, prev & ~hold);
    wr32(buf + 0x10, 0xBF800000);  // acc.y = -1 g (remote lying flat)
    wr32(buf + 0x18, 0x3F800000);  // acc_value 1
    wr8(buf + 0x5C, kWpadDevCore);
    wr8(buf + 0x5D, 0);
    c.r[3] = 1;
}

}  // namespace recomp
