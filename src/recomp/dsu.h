#pragma once
// DSU ("cemuhook") controller client (docs/CONTROLS.md "DSU controllers"): pads served over UDP
// by DS4Windows, BetterJoy, phone and Steam Deck apps, ... (the protocol Dolphin's DSU client
// reads). Every connected DSU slot appears as an SDL virtual gamepad, so it is assigned to
// ports and bound like any other controller (F1 > Controls). Off by default.
#include <string>
#include <vector>

namespace recomp::dsu {

struct SlotStatus {
    int server = 0, slot = 0;
    std::string model;      // "full gyro", "partial gyro", ...
    std::string connection; // "USB", "Bluetooth", ...
    std::string battery;
    unsigned packets = 0;   // pad data packets received in the last second
};
struct ServerStatus {
    std::string address;    // as configured
    std::string error;      // resolve/socket problem; "" when fine
    bool answering = false; // a valid packet in the last 3 s
    std::string serverInfo; // "protocol 1001, id 1234ABCD"
};

// Called by controls.cpp under its lock (before PADRead and while the Controls tab is drawn):
// attaches, updates and detaches the virtual gamepads from the latest packets.
void update();

// Controls tab, "DSU" sub-tab. True when a setting changed.
bool drawTab();

}  // namespace recomp::dsu
