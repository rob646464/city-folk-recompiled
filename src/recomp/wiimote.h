#pragma once
// Wii Remote state for the KPAD/WPAD HLE (hle_wpad.cpp; docs/CONTROLS.md, docs/WII.md "Input").
//
// Each of the four Wii Remote ports takes its state from a Source. Today every port uses the
// emulated source: buttons and the Nunchuk stick from the devices bound to the port in
// F1 > Controls (keyboard + mouse buttons and/or a gamepad, controls.cpp), the pointer from the
// port's pointer devices (mouse position over the game image, right stick, gamepad gyro),
// motion from bound "shake" inputs. A real Wii Remote over Bluetooth HID (docs/WII.md) is a
// second Source implementation behind the same interface: it fills the same State from its
// reports (IR dots -> pointer, accelerometer, extension) and takes rumble/speaker output.
#include <dolphin/types.h>

#include <string>

union SDL_Event;

namespace recomp::wiimote {

constexpr int kRemotes = 4;

// Button bits: the RVL SDK's WPAD/KPAD button layout (core buttons + the Nunchuk's Z and C,
// as KPADStatus.hold reports them), plus port-only actions above bit 15 that are turned into
// motion / pointer changes and never reach the game.
enum : u32 {
    kBtnLeft = 0x0001,
    kBtnRight = 0x0002,
    kBtnDown = 0x0004,
    kBtnUp = 0x0008,
    kBtnPlus = 0x0010,
    kBtn2 = 0x0100,
    kBtn1 = 0x0200,
    kBtnB = 0x0400,
    kBtnA = 0x0800,
    kBtnMinus = 0x1000,
    kBtnZ = 0x2000,  // Nunchuk
    kBtnC = 0x4000,  // Nunchuk
    kBtnHome = 0x8000,
    kBtnGameMask = 0xFFFF,
    kBtnShake = 0x10000,         // shake the Wii Remote
    kBtnShakeNunchuk = 0x20000,  // shake the Nunchuk
    kBtnRecenter = 0x40000,      // pointer back to the centre (stick / gyro pointers)
};

// WPAD device types (WPADProbe, KPADStatus.dev_type).
enum : u8 { kDevCore = 0, kDevNunchuk = 1, kDevNotFound = 0xFD };

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// One sample of a Wii Remote as the game should see it. KPAD conventions: pointer x -1..1
// left to right and y -1..1 top to bottom of the screen; acceleration in g along KPAD's axes
// (x right, y = -1 with the Remote lying face up, z toward the screen).
struct State {
    bool connected = false;
    u8 ext = kDevCore;             // kDevCore (no extension) or kDevNunchuk
    u32 buttons = 0;               // kBtn* (game bits only)
    bool pointerValid = false;     // the Remote points at the screen (both sensor bar dots seen)
    float px = 0, py = 0;          // pointer
    float dist = 2.0f;             // distance from the sensor bar, metres
    float roll = 0;                // radians around the pointing axis, 0 = upright
    Vec3 acc{0, -1, 0};
    float stickX = 0, stickY = 0;  // Nunchuk stick, -1..1, +y up
    Vec3 nunAcc{0, -1, 0};
    u8 battery = 4;                // WPADInfo battery level 0..4
};

// Where a port's state comes from. Guest thread (KPADRead / WPADProbe).
class Source {
public:
    virtual ~Source() = default;
    virtual const char* kind() const = 0;
    // Fills st (connected=false: no Remote). now = guest time in seconds.
    virtual void read(int chan, double now, State& st) = 0;
    virtual void rumble(int chan, bool on) = 0;
};

// Guest thread: the current state of a Remote (its port's source, the --input script, F1
// menu suppression applied). Polls the host devices at most once per guest millisecond.
void sample(int chan, State& out);
// The game's motor command (WPADControlMotor) for a Remote.
void rumble(int chan, bool on);

// Host thread.
void handleEvent(const SDL_Event& ev);  // mouse position over the window (mouse pointer)
void hostFrame();                       // inside the ImGui frame: hides the OS cursor over the game
bool drawRemotesSection();              // F1 > Controls > Ports: connection, extension, pointer per Remote
bool drawPointerTab();                  // F1 > Controls > Pointer & motion
void drawInputTest(int chan);           // F1 > Controls > Ports: what the game reads from a Remote

}  // namespace recomp::wiimote
