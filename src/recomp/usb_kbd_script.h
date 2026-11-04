#pragma once
// CF_KBD_TYPE test aid: scripted key presses on the emulated USB keyboard (usb_kbd_script.cpp).
#include "recomp/ctx.h"

namespace recomp::kbdscript {
// Host thread, before ios::usbKeyboardPoll: sets the scripted keys in the SDL scancode array.
void apply(u8* scancodes, int count);
}  // namespace recomp::kbdscript
