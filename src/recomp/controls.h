#pragma once
// Controller configuration (F1 > Controls, docs/CONTROLS.md): which device drives each
// Wii Remote port, per-device binding profiles for every Wii Remote / Nunchuk input,
// stick/trigger shaping and rumble. Saved in settings.ini; applied to Aurora's PAD live.
// The Wii Remote state itself (pointer, motion, extension) is built in wiimote.cpp.
#include <dolphin/types.h>

#include <string>

union SDL_Event;
struct PADStatus;
struct SDL_Gamepad;

namespace recomp::controls {

// F1 menu tab (host thread). Returns true when a setting changed (the menu saves on close).
bool drawTab();
void selectSubTab(const char* name);  // "Ports", "Bindings", "Pointer", "Analog" or "DSU": selected on the next drawTab() (test aids)
// Host event loop, before the menu's own hotkeys. True: the event belongs to a "press a
// button to bind" capture and must not reach F1/Esc or other hotkeys.
bool handleEvent(const SDL_Event& ev);

// hle_pad.cpp, around Aurora's PADRead: beforeRead assigns devices to ports (hot-plug)
// and pushes changed bindings to Aurora; afterRead records each port's connection.
void beforeRead();
void afterRead(PADStatus* status);
// Motor of a port's controller (the game's WPADControlMotor / PADControlMotor), with the
// port's rumble setting applied. cmd: 0 stop, 1 rumble, 2 stop hard.
void controlMotor(u32 chan, u32 cmd);

// One port's bound Wii Remote inputs, read after readHostPads (hle_pad.cpp) on the same
// thread. False when nothing drives the port (no controller, no keyboard).
struct RemoteInputs {
    u32 buttons = 0;                 // wiimote.h kBtn* bits
    float stickX = 0, stickY = 0;    // Nunchuk stick, [-1, 1], +y up (dead zone applied)
    float ptrX = 0, ptrY = 0;        // pointer stick / keys, [-1, 1], +y up
    bool keyboard = false;           // the keyboard (and mouse) drive this port
    bool pad = false;                // a gamepad drives this port
};
bool remoteInputs(u32 port, RemoteInputs& out);
// The SDL gamepad assigned to a port (gyro pointer, wiimote.cpp), or null.
SDL_Gamepad* portGamepad(u32 port);
// "Pad name [+ keyboard]" / "Keyboard and mouse" / "" for the menu.
std::string portDeviceName(u32 port);

}  // namespace recomp::controls
