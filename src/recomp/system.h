#pragma once
// The Wii's system side as the game sees it (docs/NAND.md): console settings (SYSCONF and
// setting.txt, generated from the port's settings), the Mii database, the clock offset.
// Settings live in settings.ini section [system] (a SettingsExtension owned by sysconf.cpp).
#include "recomp/ctx.h"

#include <string>

namespace recomp::sys {

// Console language as the Wii stores it (SYSCONF IPL.LNG).
enum Language : int { Japanese = 0, English, German, French, Spanish, Italian, Dutch, ChineseS, ChineseT, Korean };

struct SystemSettings {
    int language = -1;          // -1 = the disc's region default (USA/PAL English, Japan Japanese, Korea Korean); else Language
    int sensorBar = 0;          // 0 below the TV, 1 above
    int irSensitivity = 3;      // Wii Remote pointer sensitivity 1..5 (BT.SENS)
    int speakerVolume = 0x58;   // Wii Remote speaker volume 0..127 (BT.SPKV)
    bool rumble = true;         // Wii Remote rumble (BT.MOT)
    bool eurgb60 = true;        // PAL only: 60 Hz (EuRGB60) instead of 50 Hz
    long long clockOffset = 0;  // seconds added to the host clock (time travel; applied at start)
    std::string nickname = "Wii";  // console nickname (IPL.NIK)
};
SystemSettings& settings();

// Languages the running disc offers (US: English, French, Spanish; PAL: English, German,
// French, Spanish, Italian, Dutch; Japan: Japanese; Korea: Korean), and the one the console reports.
bool languageAvailable(int lang);
int effectiveLanguage();
const char* languageName(int lang);

// Brings the NAND's system files in line with the settings before the game starts: writes
// /shared2/sys/SYSCONF (created if missing, the items the port controls replaced, the rest kept)
// and /title/00000001/00000002/data/setting.txt. Called once when the NAND is first used.
void prepareNand();
// Copies the encrypted setting.txt to low memory 0x80003800, where the system menu leaves it
// for the SC library (__SCF1 reads the product area / game region from there). OSInit.
void writeSettingTxtToLowMemory();

// Seconds added to the host clock for the guest's time base (clock offset setting,
// CF_CLOCK_OFFSET=<s> overrides). runtime.cpp initTimeBase.
long long clockOffsetSeconds();

// F1 menu (Game > Console): language, sensor bar, clock offset...
bool drawMenu();  // true: a setting changed (save)

}  // namespace recomp::sys
