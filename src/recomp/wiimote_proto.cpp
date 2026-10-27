// Real Wii Remote protocol over HID (wiimote_proto.h; docs/CONTROLS.md "Real Wii Remotes").
//
// Written from public protocol documentation (WiiBrew's Wiimote pages, treated as untrusted
// reference data: every value is listed here with what it does, nothing taken from emulator code).
//
// Output reports used (byte 1 bit 0 = rumble, in every report):
//   0x10 rumble, 0x11 LEDs (bits 4-7), 0x12 reporting mode (TT MM; TT 0x04 = continuous),
//   0x13 / 0x1A IR camera enable (0x04), 0x14 speaker enable (0x04), 0x15 status request,
//   0x16 write memory/registers (MM AA AA AA SS DD*16; MM 0x04 = registers), 0x17 read
//   (MM AA AA AA SS SS), 0x18 speaker data (LL = len << 3, 20 bytes), 0x19 speaker mute (0x04).
// Input reports: 0x20 status (BB BB LF 00 00 VV), 0x21 read reply (BB BB SE AA AA DD*16),
//   0x22 acknowledgement (BB BB RR EE), 0x30-0x3F data reports.
// Setup as the Wii does it: status, calibration (EEPROM 0x16, mirror 0x20), IR camera (Wii
// sequence, basic dots for report 0x37), extension (unencrypted init 0x55 -> 0xA400F0, 0x00 ->
// 0xA400FB, id at 0xA400FA, Nunchuk calibration 0xA40020), then report mode 0x37 (buttons +
// accelerometer + 10 bytes IR + 6 bytes extension), re-sent after every status report (the Remote
// stops data reports after one).
#include "recomp/wiimote_proto.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace recomp::wiimote::hid {
namespace {

constexpr double kAnswerTimeout = 0.25;  // ack / read reply (s)
constexpr int kMaxTries = 3;
constexpr double kPlainGap = 0.01;       // pause after a command without an answer
constexpr size_t kSpeakerQueueMax = 1200;  // 0.4 s of stream

// Wii Remote button bytes -> WPAD bits (wiimote.h): byte 1 = D-pad + Plus (low byte), byte 2 =
// 2, 1, B, A, Minus, HOME (high byte).
constexpr u32 kCoreMask = 0x9F1F;
constexpr u32 kBtnZ = 0x2000, kBtnC = 0x4000;

const IrSensitivity kSens[5] = {
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xFE}, {0xFD, 0x05}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xB4}, {0xB3, 0x04}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64}, {0x63, 0x03}},
    {{0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xC8, 0x00, 0x36}, {0x35, 0x03}},
    {{0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20}, {0x1F, 0x03}},
};

// Raw accelerometer axes (x, y, z; z = +1 g lying face up) -> KPAD axes.
Vec3f toKpad(float gx, float gy, float gz) { return {-gx, -gz, gy}; }

float calAxis(u16 raw, u16 zero, u16 one) {
    const float span = float(int(one) - int(zero));
    return std::fabs(span) < 1 ? 0.0f : float(int(raw) - int(zero)) / span;
}

bool plausibleCal(const u16 zero[3], const u16 one[3]) {
    for (int i = 0; i < 3; i++) {
        const int d = int(one[i]) - int(zero[i]);
        if (d < 40 || d > 400 || zero[i] < 200 || zero[i] > 820) return false;
    }
    return true;
}

float stickAxis(u8 v, u8 lo, u8 mid, u8 hi) {
    float f;
    if (v < mid) f = mid > lo ? float(int(v) - int(mid)) / float(mid - lo) : 0.0f;
    else f = hi > mid ? float(int(v) - int(mid)) / float(hi - mid) : 0.0f;
    f = std::clamp(f, -1.0f, 1.0f);
    return std::fabs(f) < 0.02f ? 0.0f : f;
}

}  // namespace

const IrSensitivity& irSensitivity(int level) { return kSens[std::clamp(level, 1, 5) - 1]; }

u8 batteryLevel(u8 raw, bool lowFlag) {
    // Estimate (fresh AA cells read about 0xC0-0xC8): four steps over the useful range.
    u8 lvl = raw >= 0x82 ? 4 : raw >= 0x5A ? 3 : raw >= 0x32 ? 2 : raw >= 0x14 ? 1 : 0;
    if (lowFlag) lvl = std::min<u8>(lvl, 1);
    return lvl;
}

// ---- Command queue -------------------------------------------------------------------------

void Proto::push(Cmd c) { queue_.push_back(std::move(c)); }

void Proto::plain(std::initializer_list<u8> bytes, double delayAfter, int tag) {
    Cmd c;
    c.kind = Kind::Plain;
    c.bytes.assign(bytes.begin(), bytes.end());
    c.delayAfter = delayAfter > 0 ? delayAfter : kPlainGap;
    c.tag = tag;
    push(std::move(c));
}

void Proto::writeReg(u32 addr, std::initializer_list<u8> data, int tag, bool eeprom) {
    writeReg(addr, data.begin(), data.size(), tag, eeprom);
}

void Proto::writeReg(u32 addr, const u8* data, size_t len, int tag, bool eeprom) {
    Cmd c;
    c.kind = Kind::Write;
    c.bytes.assign(22, 0);
    c.bytes[0] = 0x16;
    c.bytes[1] = eeprom ? 0x00 : 0x04;
    c.bytes[2] = u8(addr >> 16);
    c.bytes[3] = u8(addr >> 8);
    c.bytes[4] = u8(addr);
    c.bytes[5] = u8(std::min<size_t>(len, 16));
    std::memcpy(&c.bytes[6], data, std::min<size_t>(len, 16));
    c.addr = addr;
    c.tag = tag;
    push(std::move(c));
}

void Proto::readMem(u32 addr, u16 size, int tag, bool eeprom) {
    Cmd c;
    c.kind = Kind::Read;
    c.bytes = {0x17, u8(eeprom ? 0x00 : 0x04), u8(addr >> 16), u8(addr >> 8), u8(addr), u8(size >> 8), u8(size)};
    c.addr = addr;
    c.size = size;
    c.tag = tag;
    push(std::move(c));
}

void Proto::send(const u8* r, size_t n) {
    u8 buf[22] = {};
    n = std::min<size_t>(n, sizeof(buf));
    std::memcpy(buf, r, n);
    if (n > 1) buf[1] = u8((buf[1] & ~1u) | (rumble_ ? 1 : 0));
    if (link_.write(buf, n) < 0) failed_ = true;
}

void Proto::sendCurrent(double now) {
    Cmd& c = queue_.front();
    send(c.bytes.data(), c.bytes.size());
    sentAt = now;
    tries_++;
    if (c.kind == Kind::Plain) {
        const double gap = c.delayAfter;
        const int tag = c.tag;
        queue_.pop_front();
        tries_ = 0;
        nextAt = now + gap;
        done(tag, true, nullptr, 0);
    } else {
        waiting_ = true;
        readBuf_.clear();
    }
}

void Proto::start(double now) {
    started_ = true;
    lastStatus = now;
    plain({0x11, u8(leds_ << 4)});
    plain({0x15, 0x00});
    readMem(0x0016, 10, kTagCal, true);
    queueIrInit();
    queueMode();
    tick(now);
}

void Proto::queueIrInit() {
    const IrSensitivity& s = irSensitivity(opt_.irSensitivity);
    plain({0x13, 0x04});
    plain({0x1A, 0x04});
    writeReg(0xB00030, {0x01});
    writeReg(0xB00000, s.block1, sizeof(s.block1));
    writeReg(0xB0001A, s.block2, sizeof(s.block2));
    writeReg(0xB00033, {0x01});  // basic IR (10 bytes: the size report 0x37 carries)
    writeReg(0xB00030, {0x08}, kTagIrDone);
}

void Proto::queueExtInit() {
    in_.ext = Ext::Identifying;
    writeReg(0xA400F0, {0x55}, kTagExtInit1);
    writeReg(0xA400FB, {0x00}, kTagExtInit2);
    readMem(0xA400FA, 6, kTagExtId, false);
}

void Proto::queueMode() {
    // At most one mode command waiting, always the last one.
    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->tag == kTagMode && !(waiting_ && it == queue_.begin())) it = queue_.erase(it);
        else ++it;
    }
    plain({0x12, 0x04, 0x37}, kPlainGap, kTagMode);
}

void Proto::queueSpeakerOn() {
    const u8 vol = u8(std::clamp(opt_.speakerVolume, 0, 0x7F) * 0x40 / 0x7F);
    const u16 div = u16(6000000.0 / kSpeakerRate);  // ADPCM rate = 6 MHz / divisor
    plain({0x14, 0x04});
    plain({0x19, 0x04});
    writeReg(0xA20009, {0x01});
    writeReg(0xA20001, {0x08});
    writeReg(0xA20001, {0x00, 0x00, u8(div), u8(div >> 8), vol, 0x00, 0x00});  // 4-bit ADPCM
    writeReg(0xA20008, {0x01});
    plain({0x19, u8(speakerMuted_ ? 0x04 : 0x00)}, kPlainGap, kTagSpeakerDone);
}

void Proto::queueSpeakerOff() {
    plain({0x19, 0x04});
    plain({0x14, 0x00});
}

void Proto::done(int tag, bool ok, const u8* d, size_t n) {
    switch (tag) {
    case kTagCal:
    case kTagCalMirror: {
        bool good = false;
        if (ok && n >= 10) {
            u32 sum = 0x55;
            for (int i = 0; i < 9; i++) sum += d[i];
            Calibration c;
            c.zero[0] = u16(d[0] << 2 | (d[3] >> 4 & 3));
            c.zero[1] = u16(d[1] << 2 | (d[3] >> 2 & 3));
            c.zero[2] = u16(d[2] << 2 | (d[3] & 3));
            c.one[0] = u16(d[4] << 2 | (d[7] >> 4 & 3));
            c.one[1] = u16(d[5] << 2 | (d[7] >> 2 & 3));
            c.one[2] = u16(d[6] << 2 | (d[7] & 3));
            c.fromEeprom = true;
            const bool sumOk = u8(sum) == d[9];
            if (plausibleCal(c.zero, c.one) && (sumOk || tag == kTagCalMirror)) {
                cal_ = c;
                good = true;
            }
        }
        if (!good && tag == kTagCal) {
            // Try the mirror right away (front of the queue: before the IR setup).
            Cmd c;
            c.kind = Kind::Read;
            c.bytes = {0x17, 0x00, 0x00, 0x00, 0x20, 0x00, 0x0A};
            c.addr = 0x0020;
            c.size = 10;
            c.tag = kTagCalMirror;
            queue_.push_front(std::move(c));
        }
        break;
    }
    case kTagExtId:
        if (ok && n >= 6) {
            std::memcpy(in_.extId, d, 6);
            // Nunchuk: 00 00 A4 20 00 00.
            const bool nunchuk = d[2] == 0xA4 && d[3] == 0x20 && d[4] == 0x00 && d[5] == 0x00;
            in_.ext = nunchuk ? Ext::Nunchuk : Ext::Other;
            if (nunchuk) readMem(0xA40020, 16, kTagNunCal, false);
        } else {
            in_.ext = extPresent_ ? Ext::Other : Ext::None;
        }
        queueMode();
        break;
    case kTagNunCal:
        if (ok && n >= 16) {
            u32 sum = 0x55;
            for (int i = 0; i < 14; i++) sum += d[i];
            NunchukCalibration c;
            for (int i = 0; i < 3; i++) {
                c.zero[i] = u16(d[i] << 2);
                c.one[i] = u16(d[4 + i] << 2);
            }
            c.xMax = d[8], c.xMin = d[9], c.xMid = d[10], c.yMax = d[11], c.yMin = d[12], c.yMid = d[13];
            c.fromEeprom = true;
            const bool sticks = c.xMin < c.xMid && c.xMid < c.xMax && c.yMin < c.yMid && c.yMid < c.yMax;
            if (u8(sum) == d[14] && u8(d[14] + 0x55) == d[15] && sticks) ncal_ = c;
        }
        break;
    case kTagIrDone: in_.irOn = true; break;
    case kTagSpeakerDone: in_.speakerOn = speakerWanted_; break;
    case kTagMode:
        setupDone_ = true;
        in_.ready = true;
        break;
    default: break;
    }
}

void Proto::tick(double now) {
    if (!started_ || failed_) return;
    // Answer overdue: send again, or give up on this command.
    if (waiting_ && now - sentAt > kAnswerTimeout) {
        waiting_ = false;
        if (tries_ >= kMaxTries) {
            Cmd c = std::move(queue_.front());
            queue_.pop_front();
            tries_ = 0;
            done(c.tag, false, nullptr, 0);
        }
    }
    if (!waiting_ && !queue_.empty() && now >= nextAt) sendCurrent(now);
    // Speaker stream: 20 bytes (40 samples) per report at the stream rate.
    if (in_.speakerOn && !spk_.empty() && now >= spkNext) {
        u8 r[22] = {0x18};
        const size_t n = std::min<size_t>(spk_.size(), 20);
        r[1] = u8(n << 3);
        for (size_t i = 0; i < n; i++) {
            r[2 + i] = spk_.front();
            spk_.pop_front();
        }
        send(r, 22);
        spkNext = std::max(spkNext, now - 0.01) + double(n) / kSpeakerBytesPerSecond;  // little catch-up after a gap
    }
    // Battery poll.
    if (setupDone_ && queue_.empty() && !waiting_ && now - lastStatus > opt_.statusInterval) {
        lastStatus = now;
        plain({0x15, 0x00});
    }
}

// ---- Host requests -------------------------------------------------------------------------

void Proto::setOptions(const Options& o) {
    const bool irChanged = o.irSensitivity != opt_.irSensitivity;
    opt_ = o;
    if (started_ && irChanged) queueIrInit();
}

void Proto::setLeds(u8 mask) {
    mask &= 0xF;
    if (mask == leds_) return;
    leds_ = mask;
    if (started_) plain({0x11, u8(mask << 4)});
}

void Proto::setRumble(bool on) {
    if (on == rumble_) return;
    rumble_ = on;
    if (started_) {
        const u8 r[2] = {0x10, u8(on ? 1 : 0)};
        send(r, 2);
    }
}

void Proto::speakerControl(int cmd) {
    switch (cmd) {
    case 0:  // off
        speakerWanted_ = false;
        in_.speakerOn = false;
        spk_.clear();
        queueSpeakerOff();
        break;
    case 1:  // on
        speakerWanted_ = true;
        speakerMuted_ = false;
        spk_.clear();
        queueSpeakerOn();
        break;
    case 2:  // mute
        speakerMuted_ = true;
        if (speakerWanted_) plain({0x19, 0x04});
        break;
    case 3:  // unmute
        speakerMuted_ = false;
        if (speakerWanted_) plain({0x19, 0x00});
        break;
    default: break;  // play / stream setup: nothing to send
    }
}

void Proto::speakerData(const u8* data, size_t len) {
    if (!speakerWanted_ || !data) return;
    for (size_t i = 0; i < len; i++) spk_.push_back(data[i]);
    while (spk_.size() > kSpeakerQueueMax) spk_.pop_front();  // late: drop the oldest
}

// ---- Input reports -------------------------------------------------------------------------

void Proto::parseButtons(const u8* b) {
    const u32 core = (u32(b[1]) << 8 | b[0]) & kCoreMask;
    in_.buttons = (in_.buttons & (kBtnC | kBtnZ)) | core;
}

void Proto::parseAccel(const u8* r) {
    // r = report (id at 0): LSBs in the button bytes (x bits 6:5 of byte 1, y bit 5 and z bit 6
    // of byte 2), high bits in bytes 3-5.
    const u16 x = u16(r[3] << 2 | (r[1] >> 5 & 3));
    const u16 y = u16(r[4] << 2 | (r[2] >> 4 & 2));
    const u16 z = u16(r[5] << 2 | (r[2] >> 5 & 2));
    in_.acc = toKpad(calAxis(x, cal_.zero[0], cal_.one[0]), calAxis(y, cal_.zero[1], cal_.one[1]),
                     calAxis(z, cal_.zero[2], cal_.one[2]));
}

void Proto::parseIrBasic(const u8* ir) {
    // Two 5-byte groups of two dots: X1 Y1 (hi: Y1 7:6, X1 5:4, Y2 3:2, X2 1:0) X2 Y2.
    for (int g = 0; g < 2; g++) {
        const u8* p = ir + g * 5;
        Input::Dot& a = in_.dots[g * 2];
        Input::Dot& b = in_.dots[g * 2 + 1];
        a.x = u16(p[0] | (p[2] >> 4 & 3) << 8);
        a.y = u16(p[1] | (p[2] >> 6 & 3) << 8);
        b.x = u16(p[3] | (p[2] & 3) << 8);
        b.y = u16(p[4] | (p[2] >> 2 & 3) << 8);
        a.seen = a.x < 1023 && a.y < 1023;
        b.seen = b.x < 1023 && b.y < 1023;
    }
    in_.irSeq++;
}

void Proto::parseExt(const u8* e, size_t n) {
    if (in_.ext != Ext::Nunchuk || n < 6) return;
    if (e[0] == 0xFF && e[1] == 0xFF && e[5] == 0xFF) return;  // not answering (being pulled out)
    in_.stickX = stickAxis(e[0], ncal_.xMin, ncal_.xMid, ncal_.xMax);
    in_.stickY = stickAxis(e[1], ncal_.yMin, ncal_.yMid, ncal_.yMax);
    const u16 ax = u16(e[2] << 2 | (e[5] >> 2 & 3));
    const u16 ay = u16(e[3] << 2 | (e[5] >> 4 & 3));
    const u16 az = u16(e[4] << 2 | (e[5] >> 6 & 3));
    in_.nunAcc = toKpad(calAxis(ax, ncal_.zero[0], ncal_.one[0]), calAxis(ay, ncal_.zero[1], ncal_.one[1]),
                        calAxis(az, ncal_.zero[2], ncal_.one[2]));
    u32 b = 0;
    if (!(e[5] & 0x01)) b |= kBtnZ;  // active low
    if (!(e[5] & 0x02)) b |= kBtnC;
    in_.buttons = (in_.buttons & ~(kBtnC | kBtnZ)) | b;
}

void Proto::onStatus(const u8* r, size_t n, double now) {
    if (n < 7) return;
    const u8 flags = r[3];
    in_.batteryRaw = r[6];
    in_.batteryLow = flags & 0x01;
    in_.battery = batteryLevel(in_.batteryRaw, in_.batteryLow);
    lastStatus = now;
    const bool ext = flags & 0x02;
    if (ext && !extPresent_) {
        extPresent_ = true;
        queueExtInit();
    } else if (!ext && extPresent_) {
        extPresent_ = false;
        in_.ext = Ext::None;
        in_.buttons &= ~(kBtnC | kBtnZ);
        in_.stickX = in_.stickY = 0;
        in_.nunAcc = {0, -1, 0};
    }
    // A status report ends data reporting until the mode is set again.
    queueMode();
}

void Proto::onInput(const u8* r, size_t n, double now) {
    if (n < 3) return;
    const u8 id = r[0];
    if (id < 0x20 || id > 0x3F) return;
    answered_ = true;
    in_.lastReport = now;
    if (id != 0x3D) parseButtons(r + 1);
    switch (id) {
    case 0x20: onStatus(r, n, now); break;
    case 0x21: {  // read reply
        if (n < 22 || !waiting_ || queue_.empty() || queue_.front().kind != Kind::Read) break;
        Cmd& c = queue_.front();
        const u8 err = r[3] & 0x0F;
        const size_t size = size_t(r[3] >> 4) + 1;
        const u16 lo = u16(r[4] << 8 | r[5]);
        if (lo != u16(c.addr + readBuf_.size())) break;  // not ours
        if (err) {
            const int tag = c.tag;
            queue_.pop_front();
            waiting_ = false;
            tries_ = 0;
            done(tag, false, nullptr, 0);
            break;
        }
        readBuf_.insert(readBuf_.end(), r + 6, r + 6 + std::min<size_t>(size, 16));
        sentAt = now;  // more packets keep the request alive
        if (readBuf_.size() >= c.size) {
            const int tag = c.tag;
            std::vector<u8> data = std::move(readBuf_);
            readBuf_.clear();
            queue_.pop_front();
            waiting_ = false;
            tries_ = 0;
            done(tag, true, data.data(), data.size());
        }
        break;
    }
    case 0x22:  // acknowledgement
        if (n >= 5 && waiting_ && !queue_.empty() && queue_.front().kind == Kind::Write && r[3] == 0x16) {
            const int tag = queue_.front().tag;
            const bool ok = r[4] == 0;
            queue_.pop_front();
            waiting_ = false;
            tries_ = 0;
            done(tag, ok, nullptr, 0);
        }
        break;
    default: {  // data reports
        in_.reports++;
        // Byte offsets (from the report id) of accelerometer, IR and extension data.
        int acc = -1, ir = -1, irLen = 0, ext = -1, extLen = 0;
        switch (id) {
        case 0x31: acc = 3; break;
        case 0x32: ext = 3, extLen = 8; break;
        case 0x33: acc = 3, ir = 6, irLen = 12; break;
        case 0x34: ext = 3, extLen = 19; break;
        case 0x35: acc = 3, ext = 6, extLen = 16; break;
        case 0x36: ir = 3, irLen = 10, ext = 13, extLen = 9; break;
        case 0x37: acc = 3, ir = 6, irLen = 10, ext = 16, extLen = 6; break;
        case 0x3D: ext = 1, extLen = 21; break;
        default: break;
        }
        if (acc >= 0 && n >= size_t(acc + 3)) parseAccel(r);
        if (ir >= 0 && irLen == 10 && n >= size_t(ir + 10)) parseIrBasic(r + ir);
        if (ext >= 0 && n >= size_t(ext + 6)) parseExt(r + ext, size_t(extLen));
        break;
    }
    }
}

// ---- Pointer -------------------------------------------------------------------------------

namespace {
constexpr float kCamW = 1024, kCamH = 768;
constexpr float kFovX = 33.0f * 3.14159265f / 180.0f;  // camera field of view across 1024 px
constexpr float kBarWidth = 0.20f;                     // metres between the sensor bar's LED groups
constexpr float kGainX = 1.5f, kGainY = 2.2f;          // camera half-width units -> KPAD units
constexpr float kBarOffset = 0.5f;                     // KPAD y of the sensor bar's direction
}  // namespace

void updatePointer(PointerState& ps, const Input::Dot dots[4], const Vec3f& acc, const PointerParams& pp) {
    int idx[4], n = 0;
    for (int i = 0; i < 4; i++)
        if (dots[i].seen) idx[n++] = i;
    float mx, my, dx, dy;
    if (n >= 2) {
        // The pair: the two dots whose midpoint is nearest the last one (reflections, lamps).
        int a = idx[0], b = idx[1];
        if (n > 2 && ps.havePair) {
            float best = 1e9f;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) {
                    const float cx = (dots[idx[i]].x + dots[idx[j]].x) * 0.5f - ps.midX;
                    const float cy = (dots[idx[i]].y + dots[idx[j]].y) * 0.5f - ps.midY;
                    if (cx * cx + cy * cy < best) best = cx * cx + cy * cy, a = idx[i], b = idx[j];
                }
        }
        dx = float(dots[b].x) - float(dots[a].x);
        dy = float(dots[b].y) - float(dots[a].y);
        // Which dot is the bar's left one: keep the orientation of the previous pair; without one,
        // assume the Remote is held face up (|roll| < 90 degrees) unless gravity says face down.
        bool flip;
        if (ps.havePair) flip = dx * ps.pairDx + dy * ps.pairDy < 0;
        else flip = acc.y > 0.5f ? dx > 0 : dx < 0;
        if (flip) dx = -dx, dy = -dy;
        mx = (float(dots[a].x) + float(dots[b].x)) * 0.5f;
        my = (float(dots[a].y) + float(dots[b].y)) * 0.5f;
        ps.havePair = true;
        ps.pairDx = dx;
        ps.pairDy = dy;
        ps.oneDotReports = 0;
        const float sep = std::sqrt(dx * dx + dy * dy);
        if (sep > 1) ps.dist = kBarWidth / (2.0f * std::tan(sep / kCamW * kFovX * 0.5f));
    } else if (n == 1 && ps.havePair && ps.oneDotReports < 200) {
        // One dot (the other left the camera's view): the midpoint from the last pair's vector,
        // on the side that is nearer the last midpoint.
        ps.oneDotReports++;
        dx = ps.pairDx;
        dy = ps.pairDy;
        const float x = dots[idx[0]].x, y = dots[idx[0]].y;
        const float ax = x + dx * 0.5f, ay = y + dy * 0.5f, bx = x - dx * 0.5f, by = y - dy * 0.5f;
        const bool first = (ax - ps.midX) * (ax - ps.midX) + (ay - ps.midY) * (ay - ps.midY) <=
                           (bx - ps.midX) * (bx - ps.midX) + (by - ps.midY) * (by - ps.midY);
        mx = first ? ax : bx;
        my = first ? ay : by;
    } else {
        ps.valid = false;
        if (n == 0) ps.havePair = false;
        return;
    }
    ps.midX = mx;
    ps.midY = my;
    // Turn the midpoint back by the dots' angle: a rolled Remote points where an upright one would.
    const float ang = std::atan2(dy, dx);
    const float c = std::cos(ang), s = std::sin(ang);
    const float ox = mx - kCamW * 0.5f, oy = my - kCamH * 0.5f;
    const float rx = ox * c + oy * s, ry = -ox * s + oy * c;
    // Camera image vs aim: pointing right moves the dots left in the image, pointing up moves
    // them down (decreasing y).
    float nx = -rx / (kCamW * 0.5f), ny = ry / (kCamW * 0.5f);
    if (pp.invertX) nx = -nx;
    if (pp.invertY) ny = -ny;
    ps.px = nx * kGainX * pp.range;
    ps.py = ny * kGainY * pp.range + (pp.barAbove ? -kBarOffset : kBarOffset) + pp.offsetY;
    ps.roll = ang;
    ps.valid = std::fabs(ps.px) < 1.6f && std::fabs(ps.py) < 1.6f;
}

}  // namespace recomp::wiimote::hid
