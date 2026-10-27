#pragma once
// Calendar features (docs/CALENDAR.md): hemisphere toggle (cf_calendar.cpp) and real-time
// weather (cf_weather.cpp). Both are F1 > Enhancements > Gameplay options, off / original by
// default; their settings are ini sections of their own ([calendar], [weather]).
#include "recomp/ctx.h"

namespace recomp::calendar {

// Southern hemisphere chosen (F1, or CF_HEMISPHERE=south|north for one run).
bool southern();
// (month 0-11, day 1-31) six months on, the day clamped to the new month's length
// (Feb 28, or 29 when leapYear). Identity in the northern hemisphere.
void seasonDate(int& month, int& day, bool leapYear = false);

// F1 > Enhancements > Gameplay. Return true when a setting changed (the menu saves on close).
bool drawHemisphereMenu();
bool drawWeatherMenu();

// Per-version address of a hooked function by its USA address (USA / PAL / Japan rev 1 tables,
// all matched by hash: config/<id>/us_map.txt).
u32 versionAddr(u32 usa);

}  // namespace recomp::calendar
