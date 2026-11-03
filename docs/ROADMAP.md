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
- **Real Wii Remotes**: Wii Remotes paired with the PC over Bluetooth (or through a Mayflash
  DolphinBar) work as on a Wii: buttons, pointer from the IR camera (needs a sensor bar),
  accelerometer, Nunchuk, rumble and the remote speaker. Read over HID (SDL's hidapi) under our
  WPAD/KPAD HLE, so they mix freely with mice and normal controllers per player port.
- **Normal controller menu navigation**: select and interact in menus with stick/D-pad and
  buttons, without being forced to use the pointer. (First check what the game's own Classic
  Controller support covers, if it has one.)
- **Grass wear**: Original (game default) / New Leaf style / Off (spec and the New Leaf values
  that are our estimate: docs/FEATURE_RESEARCH.md).
- **Skin tone picker** in F1: applies only to the player currently being played (the game has
  up to four residents per town).
- **Region-exclusive content in every version**: holidays, furniture, events, items etc. from the
  other regions, including translated item names and dialogue where needed. Research:
  docs/REGION_CONTENT.md. Research result: every disc already carries all regions' event
  dialogue (in all its languages), items and models; the region is a runtime value in
  main.dol, so this is event-calendar hooks, not data copies or translation. Only the
  Nintendo-distributed items are on no disc (WiiConnect24 downloads; docs/ONLINE.md).
- **Hemisphere toggle**: Northern (original) / Southern, seasons shifted by six months
  (weather, snow, flowers/trees, insects and fish, seasonal events where that makes sense).
  New Leaf has no hemispheres: New Horizons is the reference for what moves and what keeps
  its date (docs/FEATURE_RESEARCH.md).
- **Real-time weather** (off by default): the user searches for their town or city by name
  (a postal code also works where the service knows it) and picks it from a list; the game then
  looks up the current weather there over the internet (a key-free service such as Open-Meteo)
  and the town's weather follows it (clear / cloudy / rain / snow, within what the game can show
  for the season). Only the chosen place's rounded coordinates (~10 km) are sent, and only while
  the option is on; no automatic IP location (at most an explicit "use my approximate location"
  button). Refreshed every 30-60 minutes; the game's own weather when offline.
- **Online play restored** between players of this port (user decision 2026-10-27: not with
  real consoles or revival services). Done on branch online-1: DWC-level HLE (the game's own
  gate/visit/roster code on top), serverless rooms (the host's game is the room: room code with
  the host's endpoints, LAN list, direct IP:port, UPnP/NAT-PMP + STUN, encrypted, optional
  password, kick, ask before join), F1 > Online. Next: Wii Speak from the PC microphone, USB
  keyboard chat, friends system with automatic roster entries, letters between friends
  (WiiConnect24 replacement), NAT hole punching, optional self-hosted relay. docs/ONLINE.md.

- **Nintendo event distributions** (user decision 2026-10-27: WiiLink's archive, with their
  permission). Done on branch distrib-1: F1 > Online > Nintendo events (off by default; "In
  turn" weekly / 2-weekly / monthly loop as the main mode, original dates, only when I choose,
  "Receive now" per distribution, other regions), delivered through the game's own WiiConnect24
  path (KD/NCD HLE, wc24dl.vff) to Pete. Next: letters in the other languages (archive is English
  only), the missing Japanese/Korean distributions if another archive has them, PAL/Japan/Korea
  checks. docs/DISTRIBUTIONS.md.

## Ideas to look at later
- Texture packs (Aurora has Dolphin-compatible replacement; the RUUE01HD project recreates
  City Folk textures).
- Time-travel-friendly clock offset in F1.
