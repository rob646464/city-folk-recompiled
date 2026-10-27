// Console settings for the game (docs/NAND.md "SYSCONF"): the Wii keeps them in
// /shared2/sys/SYSCONF (read and written by the SC library: language, aspect ratio, progressive
// scan, sound mode, sensor bar, Wii Remote settings, parental controls, ...) and in the System
// Menu's encrypted setting.txt (product area, video standard, game region; also left by the
// System Menu at 0x80003800). We generate both from the port's settings before the game starts;
// the items the port does not control (Wii Remote pairings the SC library writes back, ...) are
// kept between runs. Formats from public Wii documentation; checked against the game's SC
// parser (ParseConfBuf / UnpackItem / __SCF1).
#include "recomp/system.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/ios.h"
#include "recomp/menu.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"

#include <imgui.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <vector>

namespace recomp::sys {
namespace {

namespace fs = std::filesystem;

SystemSettings sSettings;

// --- settings.ini [system] ---------------------------------------------------------------------

void resetConfig() { sSettings = SystemSettings{}; }
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
    while (!s.empty() && (s[0] == ' ' || s[0] == '	')) s.erase(0, 1);
    return s;
}
void loadKey(const std::string& section, const std::string& rawKey, const std::string& rawValue) {
    if (section != "system") return;
    const std::string key = trim(rawKey), value = trim(rawValue);
    auto num = [&] { return value == "true" ? 1LL : std::atoll(value.c_str()); };
    if (key == "language") sSettings.language = std::clamp(int(num()), -1, 9);
    else if (key == "sensor_bar") sSettings.sensorBar = std::clamp(int(num()), 0, 1);
    else if (key == "ir_sensitivity") sSettings.irSensitivity = std::clamp(int(num()), 1, 5);
    else if (key == "speaker_volume") sSettings.speakerVolume = std::clamp(int(num()), 0, 127);
    else if (key == "rumble") sSettings.rumble = num() != 0;
    else if (key == "eurgb60") sSettings.eurgb60 = num() != 0;
    else if (key == "clock_offset") sSettings.clockOffset = num();
    else if (key == "nickname") sSettings.nickname = value.substr(0, 10);
}
void saveConfig(std::ostream& out) {
    const SystemSettings& s = sSettings;
    out << "[system]\n";
    out << "language=" << s.language << "\n";
    out << "sensor_bar=" << s.sensorBar << "\n";
    out << "ir_sensitivity=" << s.irSensitivity << "\n";
    out << "speaker_volume=" << s.speakerVolume << "\n";
    out << "rumble=" << (s.rumble ? "true" : "false") << "\n";
    out << "eurgb60=" << (s.eurgb60 ? "true" : "false") << "\n";
    out << "clock_offset=" << s.clockOffset << "\n";
    out << "nickname=" << s.nickname << "\n";
}
[[maybe_unused]] const bool sRegistered = [] {
    port::addSettingsExtension({&resetConfig, &loadKey, &saveConfig});
    return true;
}();

bool isPal() { return port::install().running == port::GameVersion::PAL; }
bool isJapan() { return port::install().running == port::GameVersion::JPN; }

// --- SYSCONF ----------------------------------------------------------------------------

constexpr size_t kConfSize = 0x4000;
enum : u8 { BigArray = 0x20, SmallArray = 0x40, Byte = 0x60, Short = 0x80, Long = 0xA0, LongLong = 0xC0, Bool = 0xE0 };

struct Item {
    std::string name;
    u8 type = Byte;
    std::vector<u8> data;
};

size_t dataSize(u8 type, const u8* p, size_t avail) {
    switch (type) {
    case BigArray: return avail >= 2 ? size_t((p[0] << 8 | p[1]) + 1) : 0;
    case SmallArray: return avail >= 1 ? size_t(p[0] + 1) : 0;
    case Byte: case Bool: return 1;
    case Short: return 2;
    case Long: return 4;
    case LongLong: return 8;
    default: return 0;
    }
}

// Items of an existing SYSCONF (empty if it is not a valid one).
std::vector<Item> parse(const std::vector<u8>& b) {
    std::vector<Item> items;
    if (b.size() != kConfSize || std::memcmp(b.data(), "SCv0", 4) != 0 || std::memcmp(&b[kConfSize - 4], "SCed", 4) != 0)
        return items;
    const size_t count = size_t(b[4] << 8 | b[5]);
    for (size_t i = 0; i < count; i++) {
        const size_t off = size_t(b[6 + 2 * i] << 8 | b[7 + 2 * i]);
        if (off + 1 >= kConfSize) return {};
        Item it;
        it.type = b[off] & 0xE0;
        const size_t nameLen = (b[off] & 0x1F) + 1;
        if (off + 1 + nameLen >= kConfSize) return {};
        it.name.assign(reinterpret_cast<const char*>(&b[off + 1]), nameLen);
        size_t at = off + 1 + nameLen;
        const size_t lenField = it.type == BigArray ? 2 : it.type == SmallArray ? 1 : 0;
        const size_t n = dataSize(it.type, &b[at], kConfSize - at);
        if (!n || at + lenField + n > kConfSize) return {};
        it.data.assign(b.begin() + long(at + lenField), b.begin() + long(at + lenField + n));
        items.push_back(std::move(it));
    }
    return items;
}

std::vector<u8> serialize(const std::vector<Item>& items) {
    std::vector<u8> b(kConfSize, 0);
    std::memcpy(b.data(), "SCv0", 4);
    b[4] = u8(items.size() >> 8);
    b[5] = u8(items.size());
    size_t at = 6 + 2 * (items.size() + 1);
    for (size_t i = 0; i <= items.size(); i++) {
        b[6 + 2 * i] = u8(at >> 8);
        b[7 + 2 * i] = u8(at);
        if (i == items.size()) break;
        const Item& it = items[i];
        b[at++] = u8(it.type | ((it.name.size() - 1) & 0x1F));
        std::memcpy(&b[at], it.name.data(), it.name.size());
        at += it.name.size();
        const size_t n = it.data.size();
        if (it.type == BigArray) {
            b[at++] = u8((n - 1) >> 8);
            b[at++] = u8(n - 1);
        } else if (it.type == SmallArray) {
            b[at++] = u8(n - 1);
        }
        std::memcpy(&b[at], it.data.data(), n);
        at += n;
    }
    // The SC library keeps 0x46 bytes before the end mark for its item index.
    if (at > kConfSize - 4 - 0x46) fatal("SYSCONF: %zu bytes of items do not fit", at);
    std::memcpy(&b[kConfSize - 4], "SCed", 4);
    return b;
}

Item* find(std::vector<Item>& items, const char* name) {
    for (Item& it : items)
        if (it.name == name) return &it;
    return nullptr;
}

// Sets an item the port controls (replaced every start), or adds one with a default the
// game may change later (`keep`: an existing value stays).
void put(std::vector<Item>& items, const char* name, u8 type, std::vector<u8> data, bool keep = false) {
    if (Item* it = find(items, name)) {
        if (keep && it->type == type && it->data.size() == data.size()) return;
        it->type = type;
        it->data = std::move(data);
        return;
    }
    items.push_back({name, type, std::move(data)});
}
std::vector<u8> u8v(u32 v) { return {u8(v)}; }
std::vector<u8> u32v(u32 v) { return {u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; }

// Wii country codes (IPL.SADR byte 0).
u8 countryCode() {
    if (isJapan()) return 1;  // Japan
    if (!isPal()) return 49;  // United States
    switch (effectiveLanguage()) {
    case German: return 78;   // Germany
    case French: return 77;   // France
    case Spanish: return 105; // Spain
    case Italian: return 83;  // Italy
    case Dutch: return 94;    // Netherlands
    default: return 110;      // United Kingdom
    }
}

std::vector<u8> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}
void writeFile(const fs::path& p, const std::vector<u8>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

void writeSysconf() {
    const fs::path path = ios::hostPath("/shared2/sys/SYSCONF");
    std::vector<Item> items = parse(readFile(path));
    const bool fresh = items.empty();
    const port::Settings& ps = port::settings();
    const SystemSettings& s = sSettings;
    // Controlled by the port's settings (replaced at every start).
    put(items, "IPL.LNG", Byte, u8v(u32(effectiveLanguage())));
    put(items, "IPL.AR", Byte, u8v(ps.video.aspect == port::AspectMode::Original4x3 ? 0 : 1));
    put(items, "IPL.PGS", Byte, u8v(1));  // progressive scan (480p)
    put(items, "IPL.E60", Byte, u8v(isPal() && s.eurgb60 ? 1 : 0));
    const u32 sound = ps.audio.output == port::AudioOutput::Mono ? 0 : ps.audio.output == port::AudioOutput::Stereo ? 1 : 2;
    put(items, "IPL.SND", Byte, u8v(sound));
    put(items, "IPL.SSV", Byte, u8v(0));  // screen burn-in reduction off (the game would dim the picture)
    put(items, "IPL.PC", SmallArray, std::vector<u8>(0x4A, 0));  // parental controls off
    put(items, "BT.BAR", Byte, u8v(u32(s.sensorBar)));
    put(items, "BT.SENS", Long, u32v(u32(s.irSensitivity)));
    put(items, "BT.SPKV", Byte, u8v(u32(s.speakerVolume)));
    put(items, "BT.MOT", Byte, u8v(s.rumble ? 1 : 0));
    {
        std::vector<u8> nik(0x16, 0);  // UTF-16BE name (10 characters), then its length
        const std::string& n = s.nickname;
        const size_t len = std::min<size_t>(n.size(), 10);
        for (size_t i = 0; i < len; i++) nik[2 * i + 1] = u8(n[i]);
        nik[0x15] = u8(len);
        put(items, "IPL.NIK", SmallArray, nik);
    }
    {
        std::vector<u8> sadr(0x1007, 0);
        sadr[0] = countryCode();
        put(items, "IPL.SADR", BigArray, sadr);
    }
    // Defaults the System Menu's first-time setup leaves; the game or SC may change them later.
    put(items, "IPL.CB", Long, u32v(0), true);        // RTC counter bias
    put(items, "IPL.DH", Byte, u8v(0), true);         // horizontal display offset
    put(items, "IPL.IDL", SmallArray, {0, 0}, true);  // WiiConnect24 standby off
    put(items, "IPL.CD", Bool, u8v(1), true);         // first-time setup done
    put(items, "IPL.EULA", Bool, u8v(1), true);       // WiiConnect24 user agreement shown
    put(items, "BT.DINF", BigArray, std::vector<u8>(0x461, 0), true);  // paired Wii Remotes (none)
    put(items, "BT.CDIF", BigArray, std::vector<u8>(0x205, 0), true);
    writeFile(path, serialize(items));
    ios::Attr a;
    a.owner = 0x1000;  // the System Menu
    a.group = 1;
    ios::setAttr("/shared2/sys/SYSCONF", a);
    recomp::log("[sys] SYSCONF %s: language %s, %s, %s, sensor bar %s", fresh ? "created" : "updated",
        languageName(effectiveLanguage()), ps.video.aspect == port::AspectMode::Original4x3 ? "4:3" : "16:9",
        sound == 0 ? "mono" : sound == 1 ? "stereo" : "surround", s.sensorBar ? "above" : "below");
}

// --- setting.txt ---------------------------------------------------------------------------

std::string settingTxtPlain() {
    if (isJapan())
        return "AREA=JPN\r\nMODEL=RVL-001(JPN)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LJ\r\nSERNO=100000017\r\nVIDEO=NTSC\r\nGAME=JP\r\n";
    if (isPal())
        return "AREA=EUR\r\nMODEL=RVL-001(EUR)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LEH\r\nSERNO=100000017\r\nVIDEO=PAL\r\nGAME=EU\r\n";
    return "AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\nSERNO=100000017\r\nVIDEO=NTSC\r\nGAME=US\r\n";
}

// 0x100 bytes, each XORed with the low byte of a key that starts at 0x73B5DBFA and rotates left
// one bit per byte (a zero byte in the encrypted text is skipped by __SCF1, so the text must
// not produce one).
std::vector<u8> settingTxtEncrypted() {
    const std::string plain = settingTxtPlain();
    std::vector<u8> b(0x100, 0);
    u32 key = 0x73B5DBFA;
    for (size_t i = 0; i < b.size(); i++) {
        const u8 p = i < plain.size() ? u8(plain[i]) : 0;
        b[i] = u8(p ^ u8(key));
        if (i < plain.size() && b[i] == 0) recomp::log("[sys] setting.txt: byte %zu encrypts to 0 (the SC library would skip it)", i);
        key = key << 1 | key >> 31;
    }
    return b;
}

}  // namespace

SystemSettings& settings() { return sSettings; }

bool languageAvailable(int lang) {
    if (isPal()) return lang >= English && lang <= Dutch;
    if (isJapan()) return lang == Japanese;
    return lang == English || lang == French || lang == Spanish;
}

int effectiveLanguage() {
    const int l = sSettings.language;
    return l >= 0 && languageAvailable(l) ? l : isJapan() ? Japanese : English;
}

const char* languageName(int lang) {
    static const char* const kNames[] = {"Japanese", "English", "German",  "French", "Spanish",
                                         "Italian",  "Dutch",   "Chinese (simplified)", "Chinese (traditional)", "Korean"};
    return lang >= 0 && lang < 10 ? kNames[lang] : "default";
}

void prepareNand() {
    writeSysconf();
    const char* settingPath = "/title/00000001/00000002/data/setting.txt";
    writeFile(ios::hostPath(settingPath), settingTxtEncrypted());
    ios::Attr a;
    a.owner = 0x1000;
    a.group = 1;
    a.groupPerm = 1;
    a.otherPerm = 1;  // read-only for games
    ios::setAttr(settingPath, a);
}

void writeSettingTxtToLowMemory() {
    const std::vector<u8> b = settingTxtEncrypted();
    std::memcpy(hostp(0x80003800), b.data(), b.size());
}

long long clockOffsetSeconds() {
    if (const char* v = std::getenv("CF_CLOCK_OFFSET")) return std::atoll(v);
    return sSettings.clockOffset;
}

bool drawMenu() {
    SystemSettings& s = sSettings;
    bool dirty = false;
    // Language: the disc's languages only.
    {
        std::vector<int> langs{-1};
        for (int l = 0; l < 10; l++)
            if (languageAvailable(l)) langs.push_back(l);
        int idx = 0;
        for (size_t i = 0; i < langs.size(); i++)
            if (langs[i] == s.language) idx = int(i);
        std::vector<std::string> names;
        for (int l : langs)
            names.push_back(l < 0 ? std::string("Default (") + (isJapan() ? "Japanese" : "English") + ")" : languageName(l));
        if (ImGui::BeginCombo("Console language", names[size_t(idx)].c_str())) {
            for (size_t i = 0; i < langs.size(); i++)
                if (ImGui::Selectable(names[i].c_str(), int(i) == idx)) {
                    s.language = langs[i];
                    dirty = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The Wii's language setting, which the game's text follows.\n"
                              "Only the languages on this disc are listed. Applies at the next start.");
    }
    {
        int bar = s.sensorBar;
        ImGui::Text("Sensor bar:");
        ImGui::SameLine();
        dirty |= ImGui::RadioButton("Below the TV", &bar, 0);
        ImGui::SameLine();
        dirty |= ImGui::RadioButton("Above the TV", &bar, 1);
        s.sensorBar = bar;
    }
    dirty |= recomp::menu::SliderInt("Pointer sensitivity", &s.irSensitivity, 1, 5);
    ImGui::SetItemTooltip("The Wii's sensor bar sensitivity setting (1-5). Applies at the next start.");
    if (isPal()) {
        dirty |= recomp::menu::Checkbox("60 Hz mode (EuRGB60)", &s.eurgb60);
        ImGui::SetItemTooltip("The European Wii's 50/60 Hz setting. Applies at the next start.");
    }
    {
        int days = int(s.clockOffset / 86400), hours = int((s.clockOffset % 86400) / 3600);
        bool c = recomp::menu::SliderInt("Clock offset (days)", &days, -3650, 3650);
        c |= recomp::menu::SliderInt("Clock offset (hours)", &hours, -23, 23);
        if (c) {
            s.clockOffset = (long long)days * 86400 + (long long)hours * 3600;
            dirty = true;
        }
        ImGui::SetItemTooltip("Moves the console clock the game sees away from your PC's clock (time travel).\n"
                              "The game's own \"set the clock\" keeps working as on the console.\n"
                              "Applies at the next start.");
    }
    return dirty;
}

}  // namespace recomp::sys
