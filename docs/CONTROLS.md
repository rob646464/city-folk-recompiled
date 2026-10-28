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
      -> Source per port                 today: EmulatedSource; later: HID Wii Remote
        -> controls::remoteInputs(port)  controls.cpp: bound buttons/sticks (Aurora bindings)
        -> pointer devices               mouse (window events), right stick, gamepad gyro (SDL sensors)
```

- **State** (wiimote.h): connected, extension (none / Nunchuk), buttons (RVL WPAD bit layout incl.
  Nunchuk C/Z), pointer (KPAD units: x -1..1 left to right, y -1..1 top to bottom of the screen;
  valid or "pointing off screen"), distance, roll, acceleration (g, KPAD axes, -1 on y at rest),
  Nunchuk stick and acceleration, battery level.
- **Source** interface: `read(chan, now, State&)` and `rumble(chan, on)`. `sourceFor(chan)` picks a
  port's source; a real Wii Remote over Bluetooth HID becomes a second implementation that fills the
  same State from its reports (IR dots -> pointer, accelerometer, extension) and takes rumble and
  speaker data (plan: docs/WII.md "Real Wii Remotes").
- **Emulated source**: buttons, Nunchuk stick and the pointer stick come from the devices assigned to
  the port in F1 > Controls > Ports (a gamepad and/or keyboard + mouse buttons) through their binding
  profiles; the pointer from the port's pointer devices; motion is the rest pose plus a swing while a
  "shake" input is held (about 7 Hz, 2.5 g: the game reads `acc_value`/`acc_speed`).

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

## F1 > Controls

- **Ports**: per controller port (= Wii Remote N): device (Auto / None / Keyboard only / a gamepad)
  and "+ Keyboard"; then per Wii Remote: Connected (Auto = while a device drives the port / Always /
  Never), Extension (Nunchuk / None), Pointer (Mouse + right stick, Mouse, Right stick, Gyro + right
  stick, Gyro, all three, None) and status; input test (pointer on a screen, Nunchuk stick, buttons,
  acc).
- **Bindings**: Wii Remote (A, B, 1, 2, +, -, HOME), D-pad, Nunchuk (C, Z, stick), pointer stick/keys,
  motion (shake Remote, shake Nunchuk, recenter pointer), two slots each, per device profile.
- **Pointer & motion**: hide the OS cursor over the game while the mouse points, stick pointer speed
  and absolute mode, gyro sensitivity and inversion, sensor bar distance.
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
`remote<N>_pointer=mouse+stick|...|none`, `pointer_stick_speed`, `pointer_stick_absolute`,
`gyro_sensitivity`, `gyro_invert_x/y`, `hide_cursor`, `distance`).

## Test aids

- `--input` drives the Remotes: items `<time>:<tokens>[*<frames>]`, `P2:`..`P4:` for other Remotes.
  Tokens: `A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z SHAKE NSHAKE STICKUP STICKDOWN STICKLEFT
  STICKRIGHT` (`START`/`X`/`Y` = `PLUS`/`1`/`2`), `STICK=x/y` = Nunchuk stick at any position
  (-1..1, up = +y), `@x/y` = pointer at KPAD position (stays until the next one), `@off` = pointing
  away. Time in retraces, or in guest frames with `CF_INPUT_CLOCK=frames` (matches `CF_SHOT` frame
  numbers). A time `+N` = N after the previous item's start; `~N` after an item repeats it N times
  (`+60:A ~10` = ten presses 60 apart).
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
  (the game's clock follows guest frames) make it repeatable; run.ps1 sets all three
  (12345678, 2026-10-27 10:00). Diagnostics: `CF_RNG_LOG=1` (every setSeed), `CF_RNG_DRAWS=a,b`
  (every main-generator draw in frames a..b), `CF_TIME_LOG=1` (who reads the clock).
- `CF_WPAD_LOG=1`: Remote state changes, callbacks, motor/DPD/speaker commands.
- Route from boot to the town (USA, guest frames, see docs/WII.md "Scripted route to the town").

PowerShell note: a script parameter named `$Input` is PowerShell's automatic pipeline variable, so a
test wrapper with `param([string]$Input)` silently passes nothing to `--input` (the title then
"ignores" A). Name it something else.
