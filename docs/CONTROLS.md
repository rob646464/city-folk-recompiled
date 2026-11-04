# Controls: Wii Remote ports

City Folk is played with a Wii Remote, alone (held upright) or with a Nunchuk; every menu is
pointer-only. The port emulates up to four Wii Remotes (+ Nunchuk) at the KPAD/WPAD level of the
SDK and builds each one from PC devices. Files: `src/recomp/hle_wpad.cpp` (SDK side),
`src/recomp/wiimote.cpp/.h` (Remote state and sources), `src/recomp/controls.cpp` (device
assignment, bindings, F1 > Controls), Aurora patch 0320 (`aurora::pad::control_value`).

## Layers

```
game (EGG::CoreControllerMgr, HOME Menu, system messages)
  -> KPADRead / WPADProbe / WPAD*        hle_wpad.cpp: KPADStatus, callbacks, timing
    -> wiimote::sample(chan)             wiimote.cpp: Remote state (State)
      -> Source per port                 EmulatedSource, or HidSource (real Wii Remote, wiimote_hid.cpp)
        -> controls::remoteInputs(port)  controls.cpp: bound buttons/sticks (Aurora bindings)
        -> pointer devices               mouse (window events), right stick, gamepad gyro (SDL sensors)
```

- **State** (wiimote.h): connected, extension (none / Nunchuk), buttons (RVL WPAD bit layout incl.
  Nunchuk C/Z), pointer (KPAD units: x -1..1 left to right, y -1..1 top to bottom of the screen;
  valid or "pointing off screen"), distance, roll, acceleration (g, KPAD axes, -1 on y at rest),
  Nunchuk stick and acceleration, battery level.
- **Source** interface: `read(chan, now, State&)`, `rumble(chan, on)`, `speakerControl(chan, cmd)`,
  `speakerData(chan, bytes, len)`. `sourceFor(chan)` picks a port's source: the real-Remote source
  when the port is set to "Real Wii Remote" (section "Real Wii Remotes"), else the emulated one.
- **Emulated source**: buttons, Nunchuk stick and the pointer stick come from the devices assigned to
  the port in F1 > Controls > Ports (a gamepad and/or keyboard + mouse buttons) through their binding
  profiles; the pointer from the port's pointer devices; motion is the rest pose plus a swing while a
  "shake" input is held (about 7 Hz, 2.5 g: the game reads `acc_value`/`acc_speed`).

## Real Wii Remotes

Wii Remotes (RVL-CNT-01 and RVL-CNT-01-TR) paired with the PC over Bluetooth, or a Mayflash
DolphinBar in mode 4, drive a port when F1 > Controls > Ports > Real Wii Remotes sets its source to
"Real Wii Remote". Files: `src/recomp/wiimote_hid.cpp` (devices, threads, Source, menu),
`src/recomp/wiimote_proto.cpp/.h` (the protocol, no SDL or runtime dependencies),
`tools/tests/wiimote_hid_test.cpp` (fake device test, target `cf_wiimote_hid_test`, also `ctest`).
Written from public protocol documentation (WiiBrew's Wiimote pages, read as untrusted reference
data), no emulator code.

- **Devices**: SDL's hidapi (`SDL_hid_enumerate(0x057E, 0)`, `SDL_hid_open_path`); only VID 057E with
  PID 0306 / 0330 is opened. Nothing is enumerated while every port is emulated (the default). While
  a port uses real Remotes, `SDL_HINT_JOYSTICK_HIDAPI_WII` is forced to 0, so SDL's own Wii driver
  (off by default in SDL 3) never sets the Remote's mode or LEDs. Hot-plug: enumeration every 2 s and
  on hidapi device changes; a Remote that stops answering (off, out of range, an empty DolphinBar
  slot) is set up again when it answers a status request (tried every 3 s); a failed read or write
  closes the device and it is reopened at a later enumeration.
- **Threads**: a manager thread (enumeration, port assignment, options) and one I/O thread per
  Remote (reads with a 4 ms timeout, runs the protocol, publishes a snapshot). The guest thread only
  copies its port's snapshot; rumble and speaker requests are queued for the I/O thread. The game's
  shutdown turns the motors off and closes the devices (`wiimote::hidShutdown`, main.cpp).
- **Assignment**: per port "Auto" (the first answering Remote no other port has) or one Remote by
  serial (Bluetooth address). The player LED shows the port (LED N = Wii Remote N, as on a Wii); a
  Remote no port uses has its LEDs off.
- **Setup sequence** (`Proto::start`): LEDs, status request (0x15), accelerometer calibration from
  EEPROM 0x16 (10 bytes, checksum = sum + 0x55; mirror at 0x20; defaults when both are bad), IR camera
  in the Wii's order (0x13 / 0x1A enable, 0x01 -> 0xB00030, sensitivity block 1 at 0xB00000 and block 2
  at 0xB0001A for the console's IR sensitivity 1-5, mode 1 = basic -> 0xB00033, 0x08 -> 0xB00030), then
  report mode 0x37 continuous (buttons + accelerometer + 10-byte IR + 6-byte extension). Every
  register write waits for its acknowledgement (0x22), reads for their replies (0x21): 250 ms
  timeout, three tries. After every status report (the battery poll every 10 s, extension changes)
  the mode is set again, since the Remote stops data reports after one.
- **Extension**: on the status flag, unencrypted init (0x55 -> 0xA400F0, 0x00 -> 0xA400FB), id at
  0xA400FA (Nunchuk = 00 00 A4 20 00 00; anything else is reported to the game as no extension),
  Nunchuk calibration at 0xA40020 (16 bytes, two checksums; defaults when they fail, as with many
  third-party Nunchuks). Stick scaled by the calibrated min/centre/max, C/Z active low, 10-bit
  accelerometer. No Motion Plus, no encryption.
- **Pointer** (`hid::updatePointer`): the game's KPAD is HLE'd (hle_wpad.cpp writes KPADStatus), so
  the pointer is computed here, not by game code. The two sensor bar dots (with more, the pair nearest
  the last midpoint); their midpoint turned back by the dots' angle (roll; which dot is the left one
  is kept from the last pair, or taken from gravity when there is none); x mirrored (pointing right
  moves the dots left in the camera image); scaled to KPAD units (x 1.5, y 2.2 per camera half-width)
  and shifted by the sensor bar position (bar below the TV: aiming at it = +0.5, above: -0.5; console
  setting, F1 > Enhancements > General > Console). One dot visible: the midpoint from the last pair's
  vector. Distance from the dot separation (LED groups 20 cm apart, camera 33 degrees across 1024 px).
  F1 > Controls > Pointer & motion > Real Wii Remotes: pointer range, height, IR X / Y inversion.
- **Motion**: raw axes (x, y, z; +1 g on z lying face up), calibrated, to KPAD's as (-x, -z, y).
- **Rumble**: bit 0 of every output report, plus report 0x10 on a change.
- **Speaker**: WPADControlSpeaker "on": 0x14 enable, 0x19 mute, 0x01 -> 0xA20009, 0x08 -> 0xA20001,
  config `00 00 E8 03 VV 00 00` at 0xA20001 (4-bit Yamaha ADPCM, 6 MHz / 1000 = 6 kHz; VV = the
  console's speaker volume scaled to 0..0x40), 0x01 -> 0xA20008, unmute; mute / unmute / off as the
  game asks. The bytes from WPADSendStreamData (already ADPCM: the game's WENC code encodes them) go
  out as reports 0x18, 20 bytes = 40 samples every 6.67 ms, paced by the I/O thread (0.4 s queue,
  oldest dropped when late). Option "Remote speaker sound".
- **Battery**: status byte -> WPAD level 0-4 (thresholds 0x14 / 0x32 / 0x5A / 0x82 are an estimate);
  the low-battery flag caps it at 1 (the game warns at 1).

Pairing (also in F1 > Controls > Ports, "How to pair a Wii Remote"): Windows Settings > Bluetooth &
devices > Add device > Bluetooth, press SYNC under the battery cover (or hold 1 + 2), pick "Nintendo
RVL-CNT-01", skip the PIN. DolphinBar: mode 4, sync the Remotes with the bar. Linux: pair with
bluetoothctl or the desktop's settings; needs access to /dev/hidraw (udev rule for 057e:0306 and
057e:0330); the kernel's hid-wiimote driver also binds the Remote (whether it interferes is not
checked). macOS: System Settings > Bluetooth.

**Verified only against the fake device** (`cf_wiimote_hid_test`, 240 checks; a mutation of the
protocol code makes it fail): setup order and register values, retries after lost
acknowledgements, calibration checksum fallback, button and accelerometer decoding incl. the LSBs in
the button bytes, basic IR decoding, Nunchuk init / id / calibration / data / unplug, mode re-enable
after status reports, rumble bit in every report, battery levels, speaker register sequence, stream
contents and pacing, pointer maths (centre, sides, roll compensation, dot order, one-dot fallback,
bar above / below, inversion), the VID/PID filter. The fake was written from the same reading of the
documentation, so it cannot catch a misreading of it. **Not verified with hardware at all**: whether
real Remotes accept the sequence (especially -TR over Windows' Bluetooth stack, and the DolphinBar),
the IR image orientation (x mirrored, y direction: the invert options exist for this), accelerometer
signs of x and z, the roll sign, pointer scale and sensor bar offset compared with a real Wii,
speaker sound (rate and volume register), battery thresholds, reconnecting a Remote that was
switched off and on, Linux and macOS.

Test aids: `CF_WIIMOTE_HID=0` (never touch HID; ports stay emulated), `CF_WIIMOTE_HID=nodevices`
(real-Remote ports and menu without any HID access: "No Wii Remote found"), `CF_WIIMOTE_HID_LOG=1`
(devices opened, calibration, battery), `CF_WIIMOTE_HID_MENU=1` (Ports tab scrolled to the section with
the pairing help open, for screenshots; with `CF_MENU=Controls CF_CONTROLS_TAB=Ports`). The F1 menu ID
sweep (`CF_MENU_ID_CHECK=exit`) with two real-Remote ports: 0 conflicts. Settings `[wiimote_hid]`: `remote<N>_source=emulated|real`,
`remote<N>_device=auto|<serial>`, `pointer_range`, `pointer_offset`, `invert_x`, `invert_y`, `speaker`.

## SDK side (hle_wpad.cpp)

What the game uses (traced from the recompiled code): `KPADInit`, `KPADRead` (EGG reads every
channel once per frame with a 16-entry buffer and keeps entry 0), `WPADProbe` (no controller /
device type), connect and extension callbacks (EGG, HOME Menu), `WPADGetInfoAsync` (battery: the
game warns at level <= 1), `WPADControlMotor` (EGG CoreController start/stopMotor), `WPADDisconnect`
(the game disconnects Remotes of other players), accept-connection, DPD control, speaker/volume/
stream calls and simple sync (HOME Menu), `WPADSaveConfig`. The Bluetooth stack (WUD/BTE) never runs.

Timing as the hardware: 200 Hz reports, so `KPADRead` returns the samples since the previous read,
newest first (3-4 at 60 Hz; older ones blend pointer/stick/acc from the previous read), trigger and
release bits in the newest; callbacks arrive later on the interrupt side (connection immediately at
the next read, extension identified 150 ms later, info/DPD/LED/speaker replies after 12 ms); the
Remotes connect 1 s after `WPADInit`, like paired Remotes after the console starts. A disconnected
Remote comes back when one of its buttons is pressed.

`VIGetDimmingCount` (hle_vi.cpp) is non-zero while a Remote is connected: the SDK resets it on every
Remote report, and City Folk shows "Communications with the Wii Remote have been interrupted" when it
reads 0 (that was the boot stand-in's message in the bus).

Pointer mapping: KPAD position = screen position (checked on the game's own cursor: (0, 0) is the
centre, (+-1, +-1) the corners, at 4:3 and at native 16:9). The mouse pointer maps the window to
[-1, 1] (divided by the widescreen factor when the port's widescreen keeps 2D at the game's aspect).
Wider than 16:9 with "Keep HUD at 16:9" off the pointer spans the whole window: "Pointer in
ultrawide" below.

## F1 > Controls

- **Ports**: per controller port (= Wii Remote N): device (Auto / None / Keyboard only / a gamepad)
  and "+ Keyboard"; then per Wii Remote: Connected (Auto = while a device drives the port / Always /
  Never), Extension (Nunchuk / None), Pointer (Mouse + right stick, Mouse, Right stick, Gyro + right
  stick, Gyro, all three, None) and status; input test (pointer on a screen, Nunchuk stick, buttons,
  acc); Real Wii Remotes (source per port: Emulated / Real Wii Remote, which Remote, status with
  battery and extension, the Remotes found, pairing help).
- **Bindings**: Wii Remote (A, B, 1, 2, +, -, HOME), D-pad, Nunchuk (C, Z, stick), pointer stick/keys,
  motion (shake Remote, shake Nunchuk, recenter pointer), two slots each, per device profile.
- **Pointer & motion**: hide the OS cursor over the game while the mouse points, stick pointer speed
  and absolute mode, gyro sensitivity and inversion, sensor bar distance; real Wii Remotes: pointer
  range, height, IR inversion, Remote speaker on/off.
- **Sticks, triggers & rumble**: dead zone/sensitivity/inversion of the Nunchuk stick (left) and the
  pointer stick (right), trigger thresholds, rumble on/strength (the game's Remote motor).

Defaults: Wii Remote 1 = Remote + Nunchuk from keyboard + mouse and the first gamepad (pointer: mouse
+ right stick); Remotes 2-4 connect when a gamepad is assigned (pointer: right stick).

| Input | Keyboard + mouse | Gamepad |
|---|---|---|
| A | left click / Space | bottom face button (Nintendo: A) |
| B | right click / Left Shift | right face button (B), right trigger |
| 1 / 2 | 1 / 2 | left / top face button |
| + / - / HOME | Enter / Backspace / H | Start / Back / Guide |
| D-pad | arrow keys | D-pad |
| Nunchuk stick | WASD | left stick |
| C / Z | C, Q / Z, E | left shoulder / left trigger |
| Pointer | mouse | right stick (gyro if chosen) |
| Shake Remote / Nunchuk | F, middle click / G | right shoulder / left stick press |
| Recenter pointer | R | right stick press |

Settings: `[controls]`, `[controls.keyboard]`, `[controls.pad.<guid>]` (bindings) and `[wiimote]`
(`remote<N>_connect=auto|on|off`, `remote<N>_extension=nunchuk|none`,
`remote<N>_pointer=mouse+stick|...|none`, `remote<N>_snap=auto|on|off`, `pointer_stick_speed`,
`pointer_stick_absolute`, `gyro_sensitivity`, `gyro_invert_x/y`, `hide_cursor`, `pointer_timeout`,
`distance`).

## Pointer timeout

F1 > Controls > Pointer > "Hide the pointer after" (`pointer_timeout`, seconds, default 3, 0 =
never). The emulated Remote's pointer (mouse, right stick, gyro) goes off the screen after that long
without moving, like a Remote put down or pointed away, so the game switches to its pointer-less
field controls (no hand, no pointer toolbar). Moving the pointer brings it back where it was; the
Recenter input and the menu snap count as moves. It never times out while a pointer screen is up
(`menunav::menuUp()`: items other than the field toolbar reported in the last frames: pockets, map,
shops, dialogue choices) or the F1 menu is open. Real Wii Remotes are not affected (they point
where they are held). Implemented in `wiimote.cpp` (EmulatedSource::read).

Test runs with `--input` keep the pointer (timeout off, so existing routes are unchanged);
`CF_POINTER_TIMEOUT=<s>` sets it for any run, scripted pointer positions included, and logs
`[wiimote] remote N: pointer timed out (hidden)` / `pointer back` (also with `CF_WPAD_LOG=1`).
Checked in the user-town fixture: Rover's choice boxes keep the pointer, in town the hand and the
toolbar go 3 s after the last scripted move and come back with the next one.

## Snap pointer to menu items

Every City Folk menu is pointer-only (FEATURE_RESEARCH.md section 1). With "Snap pointer to menu
items" (F1 > Controls > Pointer & motion, per Wii Remote: Auto / On / Off) a normal controller plays
them without aiming: while a pointer screen is up, the **D-pad or the left (Nunchuk) stick moves the
pointer to the nearest item in that direction**, A clicks it (the game's own A), B is the game's own
cancel / back. The pointer stays on the chosen item (and follows it while it slides or scrolls)
until the right stick, the mouse or the gyro moves it freely again; then the next D-pad press
continues from the item under the pointer. "Recenter pointer" (right stick press) jumps to the item
nearest the pointer. The game still only ever sees a Wii Remote pointer: no menu code is changed.
Files: `src/recomp/menu_nav.cpp/.h` (generic: items, direction search, lock, pointer <-> layout
mapping helpers), `src/game/cf_menu_nav.cpp` (where the items come from), wiimote.cpp (applied to
the emulated source only; real Wii Remotes point themselves).

**Default Auto** (on for every port): snapping is active while the port's pointer was last placed by
something other than the mouse (gamepad D-pad / sticks / gyro, or nothing yet). The first mouse
movement turns it off until a controller moves the pointer again, so mouse play is exactly the
original (the D-pad then reaches the game in menus too). Reason: a gamepad player gets menu
navigation without finding an option, a mouse player never notices it; the D-pad / stick change
meaning only while a pointer screen is up, where the game does not use them (it walks and changes
tools with them in the field, which stays unchanged). On = also while the mouse points; Off = the
original everywhere.

**Where the items come from** (USA rev 1, the game's own hit tests, not coordinates of ours): after
each hit test the hook reads what the game tested, every guest frame:
1. Bounding panes named `B*` (`BC*`/`Bc*` = circles): the game's menus walk a layout's pane tree with
   fn_802B5598(layout, test fn_80007878, filter, {x, y, scale, hit}); the test checks the visible
   chain, the name, and the point through the inverse global matrix against the pane rectangle x
   scale. The hook enumerates the same panes (same filter, called as the game calls it) and takes
   each one's rectangle through its global matrix. This covers the HUD toolbar (`B_tb_00..07`, the
   memo button `B_tbBtn_00`), the town map (resident list `B_msg_*`, house markers, scroll and map
   buttons), dialogue choices (`B_msg_00..`), pockets, shops and the other `d_menu_*` screens.
2. Keyboards (name, town name, letters) use the game's copy of the HOME Menu's gui library:
   gui::Manager::update fn_80314AF4 with one PaneComponent per pane; the keys are the components
   with their trigger-target byte (+0x90) set; managers of screens that are not up get x = y = 1000.
3. The HOME Menu: homebutton::gui::Manager::update, trigger-target components (+0x24).

Layout coordinates (screen centre origin, y up) are converted to KPAD units with a scale calibrated
per source from the point the game tested and the pointer it had (`menunav::calibrate`): walks use
x * 304, y * -228 at 16:9 (the game's full-screen 608 x 456 space; 4:3-designed screens are squeezed
by their pane matrices), the keyboards x * 416. So the mapping follows whatever the screen uses;
`menunav::layoutToPointer` / `pointerToLayout` are the shared helpers for other pointer work.

**Field HUD**: the toolbar's layouts (pane names `B_tb*`, `B_actnBtn*`) are hit-tested whenever the
player walks. Alone they do not take the D-pad / stick (tools, view, walking stay the game's):
"Recenter pointer" enters the toolbar (pointer onto the slot nearest it, the toolbar slides up),
D-pad left / right moves along it, A opens (pockets, map, ...), B or Recenter leaves it (the pointer
goes back where it was; that B does not reach the game), moving the pointer freely leaves it too.
The game's own shortcuts need nothing new: - opens the pockets, + the town map (both tried in the
field, Nunchuk style; gamepad Back / Start), B closes them.

**Screens drawn but not hit-tested** (1b): the pockets record their slot panes' centres once when
they open (d_menu_itemNP calls the same pane walk with a REL test function at setup) and test
those stored positions, so they are found through the layouts drawn each frame instead
(cf_hud.cpp's Pane::Draw hook calls `cf::navLayoutDrawn` for every root pane: visible Bounding
panes named `B*`, the text box `B_msgWin` left out since A advances text wherever the pointer is).
Items the game hit-tests this frame are "tested"; while any tested non-HUD item is up only tested
items are navigable, so a choice box over the pockets ("Drop / Quit") is modal like in the game.

Verified (USA, fixture `user-town` and the new-game route, scripted D-pad presses and screenshots):
- Dialogue choices: Rover's "Yeah! / DS Suitcase / ..." chosen with D-pad down (lands on the choice
  nearest the pointer), up, A.
- Town map (+): resident list up/down, left into the house markers, down along the markers, the
  scroll button (the list scrolls while the pointer rests on it), down onto the toolbar, B closes.
- Pockets (-): slot to slot across the rows, down onto the toolbar, A on an item opens "Drop / Quit",
  D-pad down -> Quit, A; B closes.
- HUD toolbar in the field: Recenter -> nearest slot (toolbar slides up with its label), left /
  right along the slots, A on the bag opens the pockets, B leaves (pointer back); the Nunchuk stick
  walks the player afterwards (not taken in the field).
- Name keyboard (new game): keys, "More" (symbol page), "Confirm" reached; "@", "=", "m" typed.
- Mouse unchanged: with Auto the first mouse movement switches snapping off (not run with a real
  mouse in a scripted test: the scripted pointer counts as a free move, not as the mouse).
Not tried yet: shops / catalog / museum, the letter writing screen, the design editor, the HOME
Menu (hooked, unverified), PAL / Japan / Korea (the hooks are USA-only: hooks.txt lines untagged).

Direction search: candidates beyond the selected item's middle in the pressed direction (8 ways with
the stick), cost = gap along + 0.25 x centre distance along + 3 x gap across + 0.35 x centre
distance across (isotropic screen units), so items in line win and a grid moves cell by cell; at the
edge the selection stays. Item boxes are clipped to the screen (the toolbar slots reach below it);
the target is the clipped box's centre. Auto-repeat while held: 0.36 s, then every 0.11 s.

Test aids: script tokens `UP DOWN LEFT RIGHT STICK*` drive it like a pad, `RECENTER` presses
"Recenter pointer"; a scripted `@x/y` counts as a free pointer move. `CF_NAV_LOG=1` (each item set
as it becomes navigable, every selection change, toolbar enter/leave), `CF_NAV_DUMP=1` (every hit-test
source with its scale and items, every 30 frames).

## Pointer in ultrawide

With a window wider than the game's 16:9 and F1 > Visuals "Keep HUD at 16:9" off, `cf_hud.cpp`
moves the clock to the left edge and the Save button (top right) and the camera-adjust icon to the
right edge by shifting their panes' global matrices, so the game's own hit tests (which read those
matrices) find them where they are drawn. The pointer then covers the whole window:

- KPAD x runs over +-1 / factor (`ws::pointerRangeX()`: 1.3125 at 21:9, 2 at 32:9; y stays +-1).
  The mouse maps the window onto that range as before (x / factor) and now stays valid up to the
  window edge (it went invalid past 1.1, i.e. still inside the side areas); the stick and gyro
  pointer are clamped to the range instead of +-1, the absolute stick pointer is scaled to it.
- The game's cursor is drawn at any KPAD x already; what kept it out was the game's "pointer on the
  screen" test fn_8017FF4C (|x| <= 1.05, |y| <= 1.075 of the KPAD position from fn_800FA6AC): past
  it the field HUD goes (toolbar down, Save button hidden), as when the Remote points away. It is
  run with the x it reads divided by the range (hooks on both). The player's own test before
  walking to the pointer (d_a_playerNP fn_80_2DB28, limits in f1/f2) gets its x limit times the
  range, so the player also walks toward the pointer in the side areas (the 3D is Hor+ there, and
  the game's 16:9 unprojection extends linearly). `src/game/cf_pointer.cpp`, hooks.txt `all`.
- Snap to menu items: items are read from the same pane matrices, so the anchored Save button is an
  item at its drawn place; the screen edge of the item clipping / targets (menu_nav.cpp `edgeX`)
  follows the range.

Nothing changes at 16:9 or 4:3, or with "Keep HUD at 16:9" on (range 1: the same tests and
clamps as before). Test aid `CF_POINTER_LOG=1`: each change of fn_8017FF4C's answer, with the range.

Verified (USA, fixture `user-town`, scripted `@x/y` pointer, screenshots; 1680x720 = 21:9 and
2560x720 = 32:9 with the HUD anchored, 1280x720 and 21:9 with "Keep HUD at 16:9" on unchanged):
field toolbar stays up with the pointer at the window edges, Save button hover ("Save") and click
("Would you like to quit now?" with its choices) at 21:9 (x 1.17) and 32:9 (x 1.855), the snap
from the toolbar (Recenter, right along the slots) onto the anchored Save button and A, dialogue
choices, pockets and the letters panel, town map, Nook's Cranny's buy choices with the pointer
moved through both side areas (nothing reacts there, no misplaced hits). Not tried: the design
editor, the HOME Menu, a real mouse in the side areas (the mapping is the scripted one's), the
camera-adjust icon (not seen on screen in these routes), PAL / Japan / Korea (hooks mapped by `all`, not
run).

## Test aids

- `--input` drives the Remotes: items `<time>:<tokens>[*<frames>]`, `P2:`..`P4:` for other Remotes.
  Tokens: `A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z SHAKE NSHAKE STICKUP STICKDOWN STICKLEFT
  STICKRIGHT RECENTER` (`START`/`X`/`Y` = `PLUS`/`1`/`2`), `STICK=x/y` = Nunchuk stick at any position
  (-1..1, up = +y), `@x/y` = pointer at KPAD position (stays until the next one), `@off` = pointing
  away. Time in retraces, or in guest frames with `CF_INPUT_CLOCK=frames` (matches `CF_SHOT` frame
  numbers). A time `+N` = N after the previous item's start; `~N` after an item repeats it N times
  (`+60:A ~10` = ten presses 60 apart). `after link <REL module> [n]` in a route file: the items
  after it are timed (`+N`) from the n-th time the game links that module (scene changes, e.g.
  `d_bgi_drawNP` when a building's interior loads), until the next absolute time; `CF_INPUT_LOG=1`
  lists every link with its frame.
- **Route files** (`--input @file`, `tools/routes/*.txt`): items one per line or comma-separated,
  `#` comments, `include <file>` (relative to the including file) chains routes. `CF_INPUT_LOG=1`
  prints the expanded script. Routes: `new_game.txt` (boot -> new game "rob" / town "Aa" -> the
  town gate), `town_basics.txt` (walk, run, pockets, map), see docs/WII.md "Scripted route to the
  town". `tools/routes/run.ps1 -Name <run> -Route <file> -Seconds <s> [-Shots first,last,step]`
  does a test run with scratch folders (build/runs/<run>/), muted, off screen, controllers hidden;
  `py tools/routes/sheet.py <shots dir> <out.png>` makes a contact sheet of the `CF_SHOT` frames.
- **Same town every run**: a new town's layout and villagers come from random numbers and the
  clock. `CF_RNG_SEED=<hex>` (seeds from (seed, caller, call count), one main-generator stream per
  guest thread; src/game/cf_game.cpp), `CF_CLOCK_START="YYYY-MM-DD HH:MM"` and `CF_CLOCK_FRAMES=1`
  (the game's clock and the Wii Remote report timing follow guest frames) make it repeatable; run.ps1 sets all three
  (12345678, 2026-10-27 10:00). Diagnostics: `CF_RNG_LOG=1` (every setSeed), `CF_RNG_DRAWS=a,b`
  (every main-generator draw in frames a..b), `CF_TIME_LOG=1` (who reads the clock).
- `CF_WPAD_LOG=1`: Remote state changes, callbacks, motor/DPD/speaker commands.
- Route from boot to the town (USA, guest frames, see docs/WII.md "Scripted route to the town").

PowerShell note: a script parameter named `$Input` is PowerShell's automatic pipeline variable, so a
test wrapper with `param([string]$Input)` silently passes nothing to `--input` (the title then
"ignores" A). Name it something else.
