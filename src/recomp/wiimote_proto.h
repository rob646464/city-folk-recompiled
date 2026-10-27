#pragma once
// Real Wii Remote protocol (HID reports), independent of SDL and of the runtime so that it can be
// tested against a fake device (tools/tests/wiimote_hid_test.cpp; docs/CONTROLS.md "Real Wii
// Remotes"). Written from the public protocol documentation (WiiBrew's Wiimote pages, used as
// untrusted reference data); nothing taken from emulator code.
//
// One Proto per opened HID device. The owner (wiimote_hid.cpp: one host thread per device) feeds it
// every input report and calls tick() often (every few ms); Proto writes its output reports through
// a Link. All output goes through one command queue: a register write waits for its acknowledgement
// (input 0x22), a memory read for its reply (0x21), with retries; the speaker stream (0x18) and
// rumble changes go out between commands.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace recomp::wiimote::hid {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

// Nintendo's vendor id and the two Wii Remote product ids (RVL-CNT-01, RVL-CNT-01-TR). A
// DolphinBar in mode 4 shows up as RVL-CNT-01 devices too. No other HID device is ever opened.
constexpr u16 kVendorNintendo = 0x057E;
constexpr u16 kProductRemote = 0x0306, kProductRemoteTR = 0x0330;
inline bool isWiiRemote(u16 vid, u16 pid) { return vid == kVendorNintendo && (pid == kProductRemote || pid == kProductRemoteTR); }

// Output side of a HID device (report id first, as hidapi writes it).
class Link {
public:
    virtual ~Link() = default;
    virtual int write(const u8* data, size_t len) = 0;  // < 0: failed
};

// Sensitivity blocks of the IR camera for the Wii's sensor bar sensitivity setting 1..5
// (block 1: 9 bytes at 0xB00000, block 2: 2 bytes at 0xB0001A).
struct IrSensitivity {
    u8 block1[9];
    u8 block2[2];
};
const IrSensitivity& irSensitivity(int level);  // level clamped to 1..5

struct Vec3f {
    float x = 0, y = 0, z = 0;
};

enum class Ext : u8 { None, Nunchuk, Other, Identifying };

// What the Remote reports, decoded and calibrated. Acceleration in g on KPAD's axes (x right,
// y = -1 with the Remote lying face up, z toward the screen).
struct Input {
    bool ready = false;          // set up (status seen, calibration read, reporting on)
    u32 buttons = 0;             // WPAD bit layout (wiimote.h kBtn*), Nunchuk C/Z included
    Vec3f acc{0, -1, 0};
    struct Dot {
        u16 x = 1023, y = 1023;  // camera pixels, 0..1023 x 0..767
        bool seen = false;
    } dots[4];
    u32 irSeq = 0;               // bumped with every report that carried IR data
    Ext ext = Ext::None;
    u8 extId[6] = {};
    float stickX = 0, stickY = 0;  // Nunchuk, -1..1 (+y up)
    Vec3f nunAcc{0, -1, 0};
    u8 batteryRaw = 0;           // status report battery byte
    bool batteryLow = false;
    u8 battery = 4;              // WPADInfo level 0..4
    bool speakerOn = false;
    bool irOn = false;
    u32 reports = 0;             // data reports received
    double lastReport = -1;      // time of the last input report
};

struct Calibration {
    u16 zero[3] = {512, 512, 512}, one[3] = {616, 616, 616};  // 10-bit
    bool fromEeprom = false;
};
struct NunchukCalibration {
    u16 zero[3] = {512, 512, 512}, one[3] = {716, 716, 716};  // 10-bit
    u8 xMin = 35, xMid = 128, xMax = 228, yMin = 27, yMid = 128, yMax = 220;
    bool fromEeprom = false;
};

class Proto {
public:
    struct Options {
        int irSensitivity = 3;  // 1..5 (SYSCONF BT.SENS)
        int speakerVolume = 0x40;  // 0..0x7F (SYSCONF BT.SPKV scale), written as the ADPCM volume
        double statusInterval = 10.0;  // seconds between battery polls
    };

    explicit Proto(Link& link) : link_(link) {}

    void start(double now);                      // begins the setup sequence
    void onInput(const u8* r, size_t n, double now);
    void tick(double now);

    // Host requests (any thread that owns the Proto; wiimote_hid.cpp serializes them).
    void setOptions(const Options& o);
    void setLeds(u8 mask);         // bits 0..3 = LEDs 1..4
    void setRumble(bool on);
    void speakerControl(int cmd);  // WPADControlSpeaker command: 0 off, 1 on, 2 mute, 3 unmute
    void speakerData(const u8* data, size_t len);  // 4-bit Yamaha ADPCM bytes (WPADSendStreamData)

    const Input& input() const { return in_; }
    const Calibration& calibration() const { return cal_; }
    const NunchukCalibration& nunchukCalibration() const { return ncal_; }
    bool failed() const { return failed_; }        // writes failed: the device is gone
    bool answered() const { return answered_; }    // the Remote has replied at least once
    size_t speakerQueued() const { return spk_.size(); }

    // Speaker stream rate: 4-bit ADPCM at 6000 Hz, 40 samples = 20 bytes per report 0x18.
    static constexpr double kSpeakerRate = 6000.0;
    static constexpr double kSpeakerBytesPerSecond = kSpeakerRate / 2.0;

private:
    enum class Kind : u8 { Plain, Write, Read };
    struct Cmd {
        Kind kind = Kind::Plain;
        std::vector<u8> bytes;     // the report (id first); rumble bit added when sent
        u32 addr = 0;              // Read: requested address (for matching replies)
        u16 size = 0;              // Read: bytes requested
        int tag = 0;               // what to do with the reply / ack
        double delayAfter = 0;     // pause before the next command
    };
    enum Tag : int {
        kTagNone, kTagCal, kTagCalMirror, kTagExtInit1, kTagExtInit2, kTagExtId, kTagNunCal, kTagIrDone,
        kTagSpeakerDone, kTagMode
    };

    void push(Cmd c);
    void plain(std::initializer_list<u8> bytes, double delayAfter = 0, int tag = kTagNone);
    void writeReg(u32 addr, std::initializer_list<u8> data, int tag = kTagNone, bool eeprom = false);
    void writeReg(u32 addr, const u8* data, size_t len, int tag = kTagNone, bool eeprom = false);
    void readMem(u32 addr, u16 size, int tag, bool eeprom);
    void send(const u8* r, size_t n);
    void sendCurrent(double now);
    void done(int tag, bool ok, const u8* data, size_t len);
    void queueIrInit();
    void queueExtInit();
    void queueMode();
    void queueSpeakerOn();
    void queueSpeakerOff();
    void parseButtons(const u8* b);
    void parseAccel(const u8* r);
    void parseIrBasic(const u8* ir);
    void parseExt(const u8* e, size_t n);
    void onStatus(const u8* r, size_t n, double now);

    Link& link_;
    Options opt_;
    Input in_;
    Calibration cal_;
    NunchukCalibration ncal_;
    std::deque<Cmd> queue_;
    bool waiting_ = false;       // the front command was sent, its answer is due
    double sentAt = 0, nextAt = 0;
    int tries_ = 0;
    bool started_ = false, failed_ = false, answered_ = false;
    bool setupDone_ = false;     // calibration read and IR + mode programmed once
    bool rumble_ = false;
    u8 leds_ = 1;
    bool extPresent_ = false;    // status flag 0x02
    bool speakerWanted_ = false, speakerMuted_ = false;
    double lastStatus = 0;
    double spkNext = 0;
    std::deque<u8> spk_;
    std::vector<u8> readBuf_;    // a multi-packet memory read being assembled
};

// ---- IR dots -> KPAD pointer -------------------------------------------------------------

struct PointerParams {
    bool barAbove = false;    // sensor bar above the TV (SYSCONF BT.BAR = 1)
    float range = 1.0f;       // pointer travel multiplier (F1)
    float offsetY = 0.0f;     // added vertical offset (F1)
    bool invertX = false, invertY = false;
};

struct PointerState {
    bool valid = false;
    float px = 0, py = 0;     // KPAD units (-1..1, y down)
    float dist = 2.0f;        // metres from the sensor bar
    float roll = 0;           // radians, 0 = upright
    // Tracking between reports (one-dot fallback, roll continuity).
    bool havePair = false;
    float pairDx = 0, pairDy = 0;  // last two-dot vector (camera px, from the bar's left dot to its right)
    float midX = 512, midY = 384;  // last midpoint (camera px)
    int oneDotReports = 0;
};

// Updates ps from one IR sample (dots as decoded) and the Remote's acceleration (to tell an
// upright Remote from an upside-down one). Mapping (docs/CONTROLS.md "Real Wii Remotes"):
// midpoint of the sensor bar's two dots, turned back by the dots' angle (roll), centre of the
// camera = sensor bar; x/y scaled to KPAD units, shifted by the sensor bar position.
void updatePointer(PointerState& ps, const Input::Dot dots[4], const Vec3f& acc, const PointerParams& pp);

// WPADInfo battery level 0..4 from the status report's battery byte and low-battery flag.
u8 batteryLevel(u8 raw, bool lowFlag);

}  // namespace recomp::wiimote::hid
