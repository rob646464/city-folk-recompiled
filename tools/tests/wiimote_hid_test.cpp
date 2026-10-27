// Real Wii Remote protocol test without hardware (docs/CONTROLS.md "Real Wii Remotes", test aids).
//
// FakeRemote is a test double of a Wii Remote's HID side: it takes the output reports Proto writes
// (src/recomp/wiimote_proto.cpp) and answers like the protocol documentation describes (status,
// acknowledgements, memory reads from an EEPROM / register model, data reports in the mode that was
// set, extension plug and unplug, speaker registers). The fake is written from the same reading of
// the documentation as Proto, so it checks Proto's logic (sequencing, retries, decoding, pointer
// maths, pacing), not whether that reading matches real hardware.
//
// Build: target cf_wiimote_hid_test (CMake); run: build/windows-clang/cf_wiimote_hid_test.exe.
#include "recomp/wiimote_proto.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

using namespace recomp::wiimote::hid;

namespace {

int gFailures = 0, gChecks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        gChecks++;                                                                 \
        if (!(cond)) {                                                             \
            gFailures++;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                          \
    } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) <= double(eps))

// WPAD button bits (src/recomp/wiimote.h).
constexpr u32 kLeft = 0x0001, kPlus = 0x0010, kTwo = 0x0100, kOne = 0x0200, kB = 0x0400, kA = 0x0800,
              kMinus = 0x1000, kZ = 0x2000, kC = 0x4000, kHome = 0x8000;

struct FakeRemote final : Link {
    // Model state.
    u8 eeprom[0x80] = {};
    std::map<u32, u8> regs;          // control registers (24-bit address)
    u8 leds = 0;
    u8 mode = 0x30;
    bool continuous = false, reporting = false;
    bool ir1 = false, ir2 = false, speakerEnabled = false, speakerMuted = false;
    bool rumble = false;
    bool rumbleBitWrong = false;     // a report carried the wrong rumble bit
    bool expectRumble = false;
    int dropAcks = 0;                // ignore this many register writes (no ack) to test retries
    bool nunchuk = false;
    u8 battery = 0xC0;
    bool batteryLowFlag = false;
    // What the Remote "senses".
    u8 b1 = 0, b2 = 0;               // button bytes (without accelerometer LSBs)
    u16 ax = 512, ay = 512, az = 616;
    struct Dot { int x = 1023, y = 1023; } dots[4];
    u8 nsx = 128, nsy = 128;
    u16 nax = 512, nay = 512, naz = 716;
    bool nc = false, nz = false;
    // Records.
    std::vector<std::vector<u8>> writes;
    std::vector<std::pair<double, std::vector<u8>>> speakerReports;
    int statusRequests = 0, modeSets = 0;
    std::deque<std::vector<u8>> out;  // input reports waiting
    double now = 0, nextReport = 0;

    FakeRemote() {
        // Accelerometer calibration at 0x16 (zero 0x80 0x80 0x80 + LSBs, 1 g 0x9A 0x9A 0x9A), checksum.
        const u8 cal[10] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40, 0};
        setCal(0x16, cal);
        setCal(0x20, cal);
    }
    void setCal(int at, const u8 c[10], bool goodSum = true) {
        u32 sum = 0x55;
        for (int i = 0; i < 9; i++) sum += c[i];
        std::memcpy(eeprom + at, c, 9);
        eeprom[at + 9] = goodSum ? u8(sum) : u8(sum + 1);
    }
    void nunchukCal(u8* d16) const {
        const u8 c[14] = {0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80};
        std::memcpy(d16, c, 14);
        u32 sum = 0x55;
        for (int i = 0; i < 14; i++) sum += c[i];
        d16[14] = u8(sum);
        d16[15] = u8(d16[14] + 0x55);
    }

    std::vector<u8> head(u8 id) const {
        // Accelerometer LSBs ride in the button bytes.
        const u8 x1 = u8((ax & 3) << 5), y2 = u8(((ay >> 1) & 1) << 5), z2 = u8(((az >> 1) & 1) << 6);
        return {id, u8(b1 | x1), u8(b2 | y2 | z2)};
    }
    void status() {
        auto r = head(0x20);
        u8 f = u8(leds << 4);
        if (batteryLowFlag) f |= 0x01;
        if (nunchuk) f |= 0x02;
        if (speakerEnabled) f |= 0x04;
        if (ir1) f |= 0x08;
        r.insert(r.end(), {f, 0, 0, battery});
        out.push_back(r);
        reporting = false;  // a status report stops data reports until the mode is set again
    }
    void ack(u8 id, u8 err) {
        auto r = head(0x22);
        r.insert(r.end(), {id, err});
        out.push_back(r);
    }
    u8 readByte(bool reg, u32 a, bool& err) const {
        if (!reg) return a < sizeof(eeprom) ? eeprom[a] : 0;
        if ((a & 0xFF0000) == 0xA40000) {
            if (!nunchuk) {
                err = true;
                return 0;
            }
            const u32 o = a & 0xFF;
            const bool init = regs.count(0xA400F0) && regs.at(0xA400F0) == 0x55;
            if (o >= 0xFA) {
                if (!init) return 0xFF;
                const u8 id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
                return id[o - 0xFA];
            }
            if (o >= 0x20 && o < 0x30) {
                u8 c[16];
                nunchukCal(c);
                return c[o - 0x20];
            }
        }
        auto it = regs.find(a);
        return it == regs.end() ? 0 : it->second;
    }

    int write(const u8* d, size_t n) override {
        writes.emplace_back(d, d + n);
        if (n < 2) return -1;
        if (((d[1] & 1) != 0) != expectRumble) rumbleBitWrong = true;
        switch (d[0]) {
        case 0x10: rumble = d[1] & 1; break;
        case 0x11: leds = d[1] >> 4; break;
        case 0x12:
            continuous = d[1] & 0x04;
            mode = d[2];
            reporting = true;
            modeSets++;
            break;
        case 0x13: ir1 = d[1] & 0x04; break;
        case 0x1A: ir2 = d[1] & 0x04; break;
        case 0x14: speakerEnabled = d[1] & 0x04; break;
        case 0x19: speakerMuted = d[1] & 0x04; break;
        case 0x15:
            statusRequests++;
            status();
            break;
        case 0x16: {
            if (dropAcks > 0) {
                dropAcks--;
                break;
            }
            const bool reg = d[1] & 0x04;
            const u32 a = u32(d[2]) << 16 | u32(d[3]) << 8 | d[4];
            const u8 size = d[5];
            if (reg && (a & 0xFF0000) == 0xA40000 && !nunchuk) {
                ack(0x16, 7);
                break;
            }
            for (u8 i = 0; i < size; i++) {
                if (reg) regs[a + i] = d[6 + i];
                else if (a + i < sizeof(eeprom)) eeprom[a + i] = d[6 + i];
            }
            ack(0x16, 0);
            break;
        }
        case 0x17: {
            const bool reg = d[1] & 0x04;
            const u32 a = u32(d[2]) << 16 | u32(d[3]) << 8 | d[4];
            const u16 size = u16(d[5] << 8 | d[6]);
            for (u16 off = 0; off < size; off += 16) {
                const u16 chunk = u16(std::min<int>(16, size - off));
                auto r = head(0x21);
                bool err = false;
                u8 data[16] = {};
                for (u16 i = 0; i < chunk; i++) data[i] = readByte(reg, a + off + i, err);
                r.push_back(u8(((chunk - 1) << 4) | (err ? 7 : 0)));
                r.push_back(u8((a + off) >> 8));
                r.push_back(u8(a + off));
                r.insert(r.end(), data, data + 16);
                out.push_back(r);
                if (err) break;
            }
            break;
        }
        case 0x18: speakerReports.emplace_back(now, std::vector<u8>(d, d + n)); break;
        default: break;
        }
        return int(n);
    }

    std::vector<u8> dataReport() const {
        auto r = head(mode);
        if (mode != 0x37) return r;
        r.push_back(u8(ax >> 2));
        r.push_back(u8(ay >> 2));
        r.push_back(u8(az >> 2));
        for (int g = 0; g < 2; g++) {
            const Dot& p = dots[g * 2];
            const Dot& q = dots[g * 2 + 1];
            const bool ps = p.x < 1023, qs = q.x < 1023;
            const int px = ps ? p.x : 1023, py = ps ? p.y : 1023, qx = qs ? q.x : 1023, qy = qs ? q.y : 1023;
            r.push_back(u8(px));
            r.push_back(u8(py));
            r.push_back(u8(((py >> 8) & 3) << 6 | ((px >> 8) & 3) << 4 | ((qy >> 8) & 3) << 2 | ((qx >> 8) & 3)));
            r.push_back(u8(qx));
            r.push_back(u8(qy));
        }
        if (nunchuk) {
            r.push_back(nsx);
            r.push_back(nsy);
            r.push_back(u8(nax >> 2));
            r.push_back(u8(nay >> 2));
            r.push_back(u8(naz >> 2));
            r.push_back(u8((naz & 3) << 6 | (nay & 3) << 4 | (nax & 3) << 2 | (nc ? 0 : 2) | (nz ? 0 : 1)));
        } else {
            r.insert(r.end(), 6, 0);
        }
        return r;
    }

    void plug(bool on) {
        nunchuk = on;
        status();  // unsolicited status report on extension change
    }
};

// Runs Proto against the fake for `seconds` of simulated time at 1 ms steps.
void run(Proto& p, FakeRemote& f, double seconds) {
    const double end = f.now + seconds;
    while (f.now < end) {
        while (!f.out.empty()) {
            auto r = f.out.front();
            f.out.pop_front();
            p.onInput(r.data(), r.size(), f.now);
        }
        if (f.reporting && f.continuous && f.now >= f.nextReport) {
            f.nextReport = f.now + 0.01;  // 100 Hz
            auto r = f.dataReport();
            p.onInput(r.data(), r.size(), f.now);
        }
        p.tick(f.now);
        f.now += 0.001;
    }
}

bool regIs(const FakeRemote& f, u32 a, std::initializer_list<u8> v) {
    u32 i = 0;
    for (u8 b : v) {
        auto it = f.regs.find(a + i++);
        if (it == f.regs.end() || it->second != b) return false;
    }
    return true;
}

void testSetup() {
    std::printf("setup sequence\n");
    FakeRemote f;
    Proto p(f);
    Proto::Options o;
    o.irSensitivity = 3;
    p.setOptions(o);
    p.setLeds(0x2);  // Wii Remote 2
    p.start(f.now);
    run(p, f, 1.0);
    CHECK(p.answered());
    CHECK(p.input().ready);
    CHECK(f.leds == 0x2);
    CHECK(f.ir1 && f.ir2);
    const IrSensitivity& s = irSensitivity(3);
    CHECK(regIs(f, 0xB00000, {s.block1[0], s.block1[1], s.block1[2], s.block1[3], s.block1[4], s.block1[5], s.block1[6],
                              s.block1[7], s.block1[8]}));
    CHECK(regIs(f, 0xB0001A, {s.block2[0], s.block2[1]}));
    CHECK(regIs(f, 0xB00033, {0x01}));
    CHECK(regIs(f, 0xB00030, {0x08}));
    CHECK(f.mode == 0x37 && f.continuous && f.reporting);
    CHECK(p.input().irOn);
    CHECK(p.calibration().fromEeprom);
    CHECK(p.calibration().zero[0] == 0x200 && p.calibration().one[2] == 0x268);
    CHECK(p.input().reports > 50);
    CHECK(!f.rumbleBitWrong);
    // Sensitivity levels are the documented Wii blocks.
    CHECK(irSensitivity(1).block1[8] == 0xFE && irSensitivity(5).block1[0] == 0x07 && irSensitivity(9).block2[0] == 0x1F);
}

void testButtonsAccel() {
    std::printf("buttons + accelerometer\n");
    FakeRemote f;
    Proto p(f);
    p.start(f.now);
    run(p, f, 0.6);
    f.b1 = 0x01 | 0x10;          // Left, Plus
    f.b2 = 0x08 | 0x80 | 0x02;   // A, HOME, 1
    f.ax = 512, f.ay = 512, f.az = 616;  // lying face up: +1 g on z
    run(p, f, 0.05);
    CHECK(p.input().buttons == (kLeft | kPlus | kA | kHome | kOne));
    CHECK_NEAR(p.input().acc.x, 0, 0.01);
    CHECK_NEAR(p.input().acc.y, -1, 0.01);
    CHECK_NEAR(p.input().acc.z, 0, 0.01);
    // Accelerometer LSBs must not leak into the buttons; x LSBs are read.
    f.b1 = 0, f.b2 = 0x04 | 0x01 | 0x10;  // B, 2, Minus
    f.ax = 512 + 3 + 104;  // +1 g on raw x, odd LSBs
    f.ay = 512 + 104;      // +1 g on raw y
    f.az = 512;
    run(p, f, 0.05);
    CHECK(p.input().buttons == (kB | kTwo | kMinus));
    CHECK_NEAR(p.input().acc.x, -(107.0 / 104.0), 0.01);  // KPAD x = -raw x
    CHECK_NEAR(p.input().acc.z, 1, 0.02);                  // KPAD z = raw y
    CHECK_NEAR(p.input().acc.y, 0, 0.02);
}

void testCalibrationFallback() {
    std::printf("calibration checksum fallback\n");
    {
        FakeRemote f;
        const u8 bad[10] = {0x70, 0x70, 0x70, 0x00, 0x90, 0x90, 0x90, 0x00, 0x40, 0};
        const u8 good[10] = {0x81, 0x7F, 0x80, 0x00, 0x9B, 0x99, 0x9A, 0x00, 0x40, 0};
        f.setCal(0x16, bad, false);
        f.setCal(0x20, good);
        Proto p(f);
        p.start(f.now);
        run(p, f, 0.8);
        CHECK(p.calibration().zero[0] == 0x81 << 2 && p.calibration().zero[1] == 0x7F << 2);
    }
    {
        FakeRemote f;
        const u8 junk[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        f.setCal(0x16, junk, false);
        f.setCal(0x20, junk, false);
        Proto p(f);
        p.start(f.now);
        run(p, f, 0.8);
        CHECK(!p.calibration().fromEeprom);
        CHECK(p.input().ready);
    }
}

void testPointer() {
    std::printf("IR pointer\n");
    PointerParams pp;  // bar below the TV (console default)
    PointerState ps;
    Input::Dot d[4];
    auto set = [&](int ax, int ay, int bx, int by) {
        for (auto& x : d) x = Input::Dot{};
        if (ax >= 0) d[0] = {u16(ax), u16(ay), true};
        if (bx >= 0) d[1] = {u16(bx), u16(by), true};
    };
    const Vec3f up{0, -1, 0};
    set(412, 384, 612, 384);  // pointing at the sensor bar, 200 px apart
    updatePointer(ps, d, up, pp);
    CHECK(ps.valid);
    CHECK_NEAR(ps.px, 0, 1e-4);
    CHECK_NEAR(ps.py, 0.5, 1e-4);   // bar below the TV: aiming at it = lower half of the screen
    CHECK_NEAR(ps.roll, 0, 1e-4);
    const double sep = 200.0 / 1024.0 * 33.0 * 3.14159265 / 180.0;
    CHECK_NEAR(ps.dist, 0.2 / (2 * std::tan(sep / 2)), 1e-3);
    // Pointing right: the dots move left in the camera image.
    set(212, 384, 412, 384);
    updatePointer(ps, d, up, pp);
    CHECK_NEAR(ps.px, 200.0 / 512.0 * 1.5, 1e-3);
    // The same aim with the Remote rolled: the pointer stays, the roll follows the dots.
    {
        PointerState r;
        const double a = 30.0 * 3.14159265 / 180.0;
        const double cx = 312, cy = 384, hx = 100 * std::cos(a), hy = 100 * std::sin(a);
        // The midpoint's offset from the centre turns with the image.
        const double ox = (cx - 512) * std::cos(a), oy = (cx - 512) * std::sin(a);
        const double mx = 512 + ox, my = 384 + oy;
        (void)cy;
        set(int(std::lround(mx - hx)), int(std::lround(my - hy)), int(std::lround(mx + hx)), int(std::lround(my + hy)));
        updatePointer(r, d, up, pp);
        CHECK_NEAR(r.px, ps.px, 0.01);
        CHECK_NEAR(r.py, ps.py, 0.01);
        CHECK_NEAR(r.roll, a, 0.01);
    }
    // Dots listed in the other order: same result (orientation kept from the last pair).
    set(412, 384, 212, 384);
    updatePointer(ps, d, up, pp);
    CHECK_NEAR(ps.px, 200.0 / 512.0 * 1.5, 1e-3);
    CHECK_NEAR(ps.roll, 0, 1e-3);
    // One dot left the view: the midpoint from the last pair.
    set(212, 384, -1, 0);
    updatePointer(ps, d, up, pp);
    CHECK(ps.valid);
    CHECK_NEAR(ps.px, 200.0 / 512.0 * 1.5, 1e-3);
    // None: pointer off the screen.
    set(-1, 0, -1, 0);
    updatePointer(ps, d, up, pp);
    CHECK(!ps.valid);
    // Bar above the TV, pointing up (dots move down = smaller y).
    pp.barAbove = true;
    PointerState q;
    set(412, 284, 612, 284);
    updatePointer(q, d, up, pp);
    CHECK_NEAR(q.py, -100.0 / 512.0 * 2.2 - 0.5, 1e-3);
    // Upside-down Remote with no previous pair: gravity picks the order.
    PointerState u;
    pp.barAbove = false;
    set(612, 384, 412, 384);
    updatePointer(u, d, Vec3f{0, 1, 0}, pp);
    CHECK_NEAR(std::fabs(u.roll), 3.14159265, 1e-3);
    // Invert options.
    PointerState v;
    pp.invertX = true;
    set(212, 384, 412, 384);
    updatePointer(v, d, up, pp);
    CHECK_NEAR(v.px, -200.0 / 512.0 * 1.5, 1e-3);
}

void testIrDecode() {
    std::printf("IR report decoding\n");
    FakeRemote f;
    Proto p(f);
    p.start(f.now);
    run(p, f, 0.6);
    f.dots[0] = {1000, 700};
    f.dots[1] = {5, 300};
    f.dots[3] = {700, 767 - 1};
    run(p, f, 0.05);
    const auto& d = p.input().dots;
    CHECK(d[0].seen && d[0].x == 1000 && d[0].y == 700);
    CHECK(d[1].seen && d[1].x == 5 && d[1].y == 300);
    CHECK(!d[2].seen);
    CHECK(d[3].seen && d[3].x == 700 && d[3].y == 766);
}

void testNunchuk() {
    std::printf("Nunchuk plug, init, data, unplug\n");
    FakeRemote f;
    Proto p(f);
    p.start(f.now);
    run(p, f, 0.6);
    CHECK(p.input().ext == Ext::None);
    const int modes = f.modeSets;
    f.plug(true);
    run(p, f, 0.5);
    CHECK(regIs(f, 0xA400F0, {0x55}));
    CHECK(regIs(f, 0xA400FB, {0x00}));
    CHECK(p.input().ext == Ext::Nunchuk);
    CHECK(p.nunchukCalibration().fromEeprom);
    CHECK(f.modeSets > modes && f.reporting);  // reporting turned back on after the status report
    f.nsx = 0xE0, f.nsy = 0x20;   // full right, full down (calibration max / min)
    f.nc = true;
    f.naz = 512, f.nay = 716;     // Nunchuk pointing up: +1 g on raw y
    run(p, f, 0.05);
    CHECK_NEAR(p.input().stickX, 1, 0.01);
    CHECK_NEAR(p.input().stickY, -1, 0.01);
    CHECK((p.input().buttons & (kC | kZ)) == kC);
    CHECK_NEAR(p.input().nunAcc.z, 1, 0.02);
    f.nc = false, f.nz = true;
    f.nsx = 0x80, f.nsy = 0x81;
    run(p, f, 0.05);
    CHECK((p.input().buttons & (kC | kZ)) == kZ);
    CHECK_NEAR(p.input().stickX, 0, 0.001);
    CHECK_NEAR(p.input().stickY, 0, 0.03);
    f.plug(false);
    run(p, f, 0.3);
    CHECK(p.input().ext == Ext::None);
    CHECK((p.input().buttons & (kC | kZ)) == 0);
    CHECK(f.reporting);
}

void testRumbleAndRetry() {
    std::printf("rumble bit, lost acknowledgements\n");
    FakeRemote f;
    f.dropAcks = 2;  // the first two register writes get no answer: resent
    Proto p(f);
    p.start(f.now);
    run(p, f, 1.5);
    CHECK(p.input().ready && p.input().irOn);
    CHECK(regIs(f, 0xB00030, {0x08}));
    f.expectRumble = true;
    p.setRumble(true);
    CHECK(f.rumble);
    p.setLeds(0x4);
    run(p, f, 0.1);
    CHECK(f.leds == 0x4);
    CHECK(!f.rumbleBitWrong);  // the LED report carried the rumble bit
    f.expectRumble = false;
    p.setRumble(false);
    CHECK(!f.rumble);
    run(p, f, 0.1);
    CHECK(!f.rumbleBitWrong);
}

void testStatusPoll() {
    std::printf("battery poll, reporting resumes\n");
    FakeRemote f;
    Proto p(f);
    Proto::Options o;
    o.statusInterval = 1.0;
    p.setOptions(o);
    p.start(f.now);
    run(p, f, 0.6);
    CHECK(p.input().battery == 4);
    f.battery = 0x40;
    f.batteryLowFlag = true;
    const int req = f.statusRequests;
    const u32 reports = p.input().reports;
    run(p, f, 1.5);
    CHECK(f.statusRequests > req);
    CHECK(p.input().battery == 1 && p.input().batteryLow);
    CHECK(f.reporting && p.input().reports > reports + 100);
    CHECK(batteryLevel(0x00, false) == 0 && batteryLevel(0xC8, false) == 4 && batteryLevel(0xC8, true) == 1);
}

void testSpeaker() {
    std::printf("speaker init and stream pacing\n");
    FakeRemote f;
    Proto p(f);
    Proto::Options o;
    o.speakerVolume = 0x7F;
    p.setOptions(o);
    p.start(f.now);
    run(p, f, 0.6);
    p.speakerControl(1);
    run(p, f, 0.3);
    CHECK(f.speakerEnabled && !f.speakerMuted);
    CHECK(regIs(f, 0xA20009, {0x01}));
    CHECK(regIs(f, 0xA20001, {0x00, 0x00, 0xE8, 0x03, 0x40, 0x00, 0x00}));  // ADPCM, 6 MHz / 1000 = 6 kHz, volume
    CHECK(regIs(f, 0xA20008, {0x01}));
    CHECK(p.input().speakerOn);
    // The game sends 20 bytes (40 samples) every 6.67 ms; one second of stream.
    std::vector<u8> sent;
    const double t0 = f.now;
    double nextFeed = f.now;
    u8 v = 0;
    f.speakerReports.clear();
    while (f.now < t0 + 1.0) {
        if (f.now >= nextFeed) {
            u8 chunk[20];
            for (auto& c : chunk) c = v++;
            sent.insert(sent.end(), chunk, chunk + 20);
            p.speakerData(chunk, 20);
            nextFeed += 20.0 / Proto::kSpeakerBytesPerSecond;
        }
        run(p, f, 0.001);
    }
    run(p, f, 0.05);
    std::vector<u8> got;
    for (const auto& [t, r] : f.speakerReports) {
        const size_t n = r[1] >> 3;
        CHECK(n >= 1 && n <= 20);
        got.insert(got.end(), r.begin() + 2, r.begin() + 2 + long(n));
    }
    CHECK(got == sent);
    const size_t reports = f.speakerReports.size();
    CHECK(reports >= 148 && reports <= 153);
    // Paced: no two reports closer than 5 ms in steady state.
    int tooClose = 0;
    for (size_t i = 2; i < f.speakerReports.size(); i++)
        if (f.speakerReports[i].first - f.speakerReports[i - 1].first < 0.005) tooClose++;
    CHECK(tooClose == 0);
    // Mute, off.
    p.speakerControl(2);
    run(p, f, 0.05);
    CHECK(f.speakerMuted);
    p.speakerControl(0);
    run(p, f, 0.05);
    CHECK(!f.speakerEnabled && !p.input().speakerOn);
    f.speakerReports.clear();
    u8 more[20] = {};
    p.speakerData(more, 20);
    run(p, f, 0.05);
    CHECK(f.speakerReports.empty());
}

void testFilter() {
    std::printf("device filter\n");
    CHECK(isWiiRemote(0x057E, 0x0306));
    CHECK(isWiiRemote(0x057E, 0x0330));
    CHECK(!isWiiRemote(0x057E, 0x2009));  // Switch Pro Controller
    CHECK(!isWiiRemote(0x054C, 0x0306));
}

}  // namespace

int main() {
    testFilter();
    testSetup();
    testButtonsAccel();
    testCalibrationFallback();
    testPointer();
    testIrDecode();
    testNunchuk();
    testRumbleAndRetry();
    testStatusPoll();
    testSpeaker();
    std::printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
