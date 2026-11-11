#pragma once
// F1 > Controls "Type with the PC keyboard" (docs/CONTROLS.md "Typing with the PC keyboard"): the
// PC keyboard is the Wii's USB keyboard (/dev/usb/kbd, ios_devices.cpp) for every text box of the
// game, and its keys stop acting as Wii Remote buttons (controls.cpp). Live: the keyboard is
// attached / detached as the option changes.
#include <string>

union SDL_Event;

namespace recomp::pckbd {

// The option is on (settings [keyboard] type_with_pc_keyboard, or CF_USB_KBD_DEVICE=1).
bool typing();
// A key that types something or edits text on the Wii keyboard (letters, digits, Enter, Esc,
// Backspace, Tab, Space, punctuation): never also a push-to-talk key while typing.
bool typesText(int scancode);

// Host event loop, before the F1 menu's own hotkeys (menuOpen = the menu was open when the event
// came: its keys are the menu's). Key presses and releases become USB keyboard reports in their
// order; F1 never reaches the game, nor anything while the menu is open.
void handleEvent(const SDL_Event& ev, bool menuOpen);
// Host thread, every frame: attach / detach with the option, releases every key while the menu
// is open, and the CF_KBD_TYPE test aid (real SDL key events through the event queue).
void hostFrame();
// Route token KEYS=<text> (any thread): typed like CF_KBD_TYPE text, starting now.
void scriptType(const std::string& text);

}  // namespace recomp::pckbd
