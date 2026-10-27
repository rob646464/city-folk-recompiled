# Roadmap

Engine work for the Wii: docs/WII.md. This file tracks the phases and the user's feature list.

## Phases
1. **Boot** — scaffold, Wii discs, MEM2, function discovery, RELs, OS/IOS HLE, title screen.
2. **Playable** — Wii Remote HLE with mouse pointer, NAND saves, sound (AX), real clock.
3. **Core enhancements from LM** — any resolution, widescreen/ultrawide (native 16:9 + Hor+),
   any frame rate by interpolation (logic at the game's own rate), MSAA, shader precache,
   accuracy toggles, F1 menu, controls, setup flow, updater, packaging (Windows/Linux/macOS CI).
4. **Game features** (below), all optional; originals by default.
5. **PAL and Japan bases** (function matching; language selection from PAL).

## Feature list (from the user, 2026-10-27)
- **Wii pointer controls** from the mouse, from the right stick, and from gyro on controllers
  with motion sensors (when connected). F1 > Controls picks the source per port.
- **Normal controller menu navigation**: select and interact in menus with stick/D-pad and
  buttons, without being forced to use the pointer. (First check what the game's own Classic
  Controller support covers, if it has one.)
- **Grass wear**: Original (game default) / New Leaf style / Off.
- **Skin tone picker** in F1: applies only to the player currently being played (the game has
  up to four residents per town).
- **Region-exclusive content in every version**: holidays, furniture, events, items etc. from the
  other regions, including translated item names and dialogue where needed. Research:
  docs/REGION_CONTENT.md. Needs the matching discs where content is only on another region's
  disc (greyed out otherwise).
- **Hemisphere toggle**: Northern (original) / Southern, seasons shifted by six months
  (weather, snow, flowers/trees, insects and fish, seasonal events where that makes sense; New
  Leaf is the reference for what moves and what keeps its date).
- **Real-time weather** (off by default): the user searches for their town or city by name
  (a postal code also works where the service knows it) and picks it from a list; the game then
  looks up the current weather there over the internet (a key-free service such as Open-Meteo)
  and the town's weather follows it (clear / cloudy / rain / snow, within what the game can show
  for the season). Only the chosen place's rounded coordinates (~10 km) are sent, and only while
  the option is on; no automatic IP location (at most an explicit "use my approximate location"
  button). Refreshed every 30-60 minutes; the game's own weather when offline.
- **Online play restored** however possible: Nintendo WFC (DWC: visiting towns, friend codes,
  Wii Speak chat) through a revival server such as Wiimmfi, and WiiConnect24 (letters, Nintendo
  event distributions) through a revival service such as WiiLink/RiiConnect24, if they support
  City Folk. Needs real sockets/HTTP/SSL under the IOS network HLE. Research first:
  docs/ONLINE.md.

## Ideas to look at later
- Texture packs (Aurora has Dolphin-compatible replacement; the RUUE01HD project recreates
  City Folk textures).
- Time-travel-friendly clock offset in F1.
