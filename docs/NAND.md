# NAND, IOS devices and console settings

How the port gives the game the Wii's system side: the NAND flash (save file, console settings,
Mii database), the other IOS devices (ES, STM, SD, USB, network), the console settings in
SYSCONF / setting.txt and the clock. Code: `src/recomp/hle_ios.cpp` (IPC), `ios_fs.cpp` (file
system), `ios_devices.cpp` (other devices), `sysconf.cpp` (settings), `nand_selftest.cpp` (test
aid); interface `src/recomp/ios.h`, `src/recomp/system.h`. Everything written from public Wii
documentation and the game's own code; no emulator code.

## Where the HLE sits: IPC, not the NAND library

The Wii's libraries reach IOS (the Starlet I/O processor) through IPC: `IOS_Open/Close/Read/
Write/Seek/Ioctl/Ioctlv` and their async forms. Above that the game links the SDK's ISFS, NAND,
SC, ES, VF, RFL, NWC24, KBD... We replace **only the IPC client** and implement IOS's devices on
the host; every library above it runs as recompiled game code. Reasons:

- The NAND library has many entry points (sync/async, Private, Safe, SimpleSafe, banner, home
  directory, NANDCheck quotas) and internal state (safe-open copies in `/tmp`, error code
  conversion, retry paths). At IPC level all of it stays the game's own code: its exact error
  handling, its order of requests, its use of `/tmp`.
- Several libraries share the file system (SC reads SYSCONF and setting.txt, RFL the Mii
  database, VF/NWC24 their files); one host file system serves all of them consistently.
- IOS's interface is small and documented (paths, result codes, a dozen ioctls).

(os/ and ipc/ translation units are HLE'd by the recompiler anyway; NAND/ISFS/SC/ES/RFL are not
in that list, so they run as code.)

Verified against the game's code (disassembly of this disc): NANDCheck's quotas (home folder
0x400 clusters / 0x21 inodes, system 0x4400 / 0xFA0; the folders it sums, a missing one counts 0),
the SYSCONF parser (ParseConfBuf / UnpackItem: "SCv0" header, item offsets, type bits, "SCed" at
the end, 0x46 bytes kept before it), __SCF1 (setting.txt at 0x80003800, rolling-XOR key), the STM
event handler, RFL's handling of a missing database.

## Requests and latency

`hle_ios.cpp` keeps IOS's descriptor table (32 entries, lowest free first). Each request is
**performed when the game issues it** (so the program order of creates, writes, renames is kept
exactly) and **answered after a modelled IOS time**:

- async (`IOS_*Async`): returns 0 at once, the callback `(result, arg)` comes as an interrupt
  when the request is done;
- sync: the calling thread waits with the CPU given up (`sched::withCpuReleased`): other guest
  threads and interrupts run, as on the console where the thread sleeps on the IPC reply;
- requests to one resource manager (FS, ES, STM, other) are answered in order: each queue has a
  "busy until" time, a new request starts when the previous one is done.

Costs (seconds, `cost()` of each handle): file open 0.6 ms, read 0.15 ms + 4 MB/s, write 0.3 ms
+ 1.5 MB/s, close 0.2 ms (8 ms after writes: the FS commits its metadata), create / delete /
rename / create-dir / set-attributes 6 ms, get-attributes 0.3 ms, read-dir 0.5 ms + per entry,
GetUsage 0.5 ms + per inode, other devices 0.1-0.2 ms. A 4.7 MB save then takes ~3 s, in the
range of the console. `CF_IOS_LATENCY=<scale>` scales every cost (0 = answers at once).

A device can keep a request unanswered (`kPending`, answered later with `ios::answer`): the STM
event hook; held requests of a synchronous caller block it until answered.

## The NAND file system

Host folder: `%APPDATA%\Animal Crossing City Folk Recompiled\nand\` (`CF_NAND_DIR` overrides;
tests always use a scratch one), the Wii's paths below it:

| NAND path | What |
|---|---|
| `/title/00010000/52555545/data/` | USA save (`rvforest.dat`, `banner.bin`); PAL `.../52555550/` |
| `/shared2/sys/SYSCONF` | console settings (generated, see below) |
| `/title/00000001/00000002/data/setting.txt` | System Menu's product info (generated) |
| `/shared2/menu/FaceLib/RFL_DB.dat` | Mii database (absent: see "Miis") |
| `/shared2/wc24/`, `/meta`, `/ticket` | created empty (folders NANDCheck and NWC24 look at) |
| `/tmp` | emptied at every start, as IOS does at boot (the NAND library's safe-open copies) |
| `~attributes.txt` | not a Wii file: owner/group/permissions per path (see below) |

What IOS does and we do the same (`ios_fs.cpp`):

- **Paths**: absolute, under 64 bytes, names of at most 12 characters, at most 8 levels
  (`FS_EINVAL` / `FS_EDIRDEPTH` otherwise). Names the host cannot store (`<>:"\|?*%`, control
  characters, trailing dot/space, CON/NUL/...) are stored as `%XX`; ReadDir gives the Wii names
  back. ReadDir lists alphabetically.
- **Results**: `-101` invalid, `-102` access denied, `-105` exists, `-106` not found, `-109` too
  many open files (15), `-111` file in use (delete/rename of an open file), `-114` host I/O error.
- **File descriptors**: open modes 1 read / 2 write / 3 both (checked against the permissions);
  read past the end returns what is there; seek beyond the end is `-101`; writes extend the file;
  ioctl 11 GetFileStats returns `{length, position}`.
- **/dev/fs ioctls**: 2 GetStats (16 KiB clusters; a console with the System Menu and a few
  channels, plenty free), 3 CreateDir, 5 SetAttr, 6 GetAttr, 7 Delete (a folder with its
  contents), 8 Rename (an existing target file is replaced; this is how a safe close puts the
  `/tmp` copy back), 9 CreateFile, 0x0A SetFileVerCtrl (no-op), 0x0D Shutdown; ioctlv 4 ReadDir
  (count, or names + count), 0x0C GetUsage (clusters + inodes, the path itself included).
  Format (1) is refused.
- **Owners and permissions**: files the game creates belong to the game (uid 0x1001, group 0x3031
  = maker "01") with the permissions it asked for; SYSCONF and setting.txt belong to the System
  Menu (uid 0x1000; setting.txt read-only for others). Opening needs the permission for the mode
  (owner / group / other, as IOS checks); creating and deleting need write permission on the
  parent folder. Kept in `~attributes.txt` (one line per path: owner, group, three permissions,
  attribute byte, path); paths without a line use defaults (the game's data folder is the game's,
  everything else read/write for all).
- **NANDCheck** sees real usage of the host folder (the game's home folder and the system folders
  it sums), so it answers "fits" unless a save really outgrows its quota.

Note for readers of the SDK names: the function named `NANDGetLength` (0x803A8444, SDK_NAMES.md)
returns the *second* word of the file stats, the position; the game uses it as a "tell" (it seeks
back to that value, fn_8021E0BC / fn_8021E620). The async `NANDGetLengthAsync` callback reads the
first word (length). IOS's layout is `{length, position}` either way.

## Console settings: SYSCONF and setting.txt

Generated by `sysconf.cpp` before the game's first NAND access (`sys::prepareNand`, from
`nandRoot()`), from F1 settings (section `[system]` of settings.ini, plus the port's video/audio
settings):

| SYSCONF item | Value | Source |
|---|---|---|
| IPL.LNG | language (0 JP, 1 EN, 2 DE, 3 FR, 4 ES, 5 IT, 6 NL) | F1 > Game > Console language; only the disc's languages (US: EN/FR/ES, PAL: EN/DE/FR/ES/IT/NL); default English |
| IPL.AR | 1 = 16:9 (0 when F1 aspect is "Original 4:3") | port aspect setting |
| IPL.PGS | 1 (progressive scan) | |
| IPL.E60 | PAL: 1 = 60 Hz (EuRGB60) | `[system] eurgb60` |
| IPL.SND | 0 mono, 1 stereo, 2 surround | port audio output |
| IPL.SSV | 0 (no screen burn-in dimming) | |
| IPL.PC | parental controls off (0x4A zero bytes) | |
| IPL.NIK | console nickname "Wii" (UTF-16) | `[system] nickname` |
| IPL.SADR | country (USA 49; PAL by language: UK 110, DE 78, FR 77, ES 105, IT 83, NL 94) | |
| BT.BAR / BT.SENS / BT.SPKV / BT.MOT | sensor bar below (0) / above (1); IR sensitivity 3; speaker volume 0x58; rumble on | `[system]` |
| IPL.CB, IPL.DH, IPL.IDL, IPL.CD, IPL.EULA, BT.DINF, BT.CDIF | first-setup defaults (counter bias 0, no WiiConnect24 standby, setup done, no paired remotes) | kept once written (the SC library may write some back with SCFlush) |

The items above "kept" are replaced at every start; others stay as the game (SC) wrote them. The
file keeps the console format (`SCv0`, offsets, items, `SCed` at 0x3FFC; 0x46 bytes before the end
left free for the SC library's index), so SCFlush round-trips.

`setting.txt` (0x100 bytes, encrypted with the System Menu's rolling key 0x73B5DBFA): `AREA=USA`,
`MODEL=RVL-001(USA)`, `DVD=0`, `MPCH=0x7FFE`, `CODE=LU`, `SERNO=100000017`, `VIDEO=NTSC`,
`GAME=US` (PAL: EUR / LEH / PAL / EU). Written to the NAND file and to low memory 0x80003800
(OSInit), where the System Menu leaves it and `__SCF1` reads it (SCGetProductArea /
SCGetProductGameRegion; SCGetLanguage checks the language against the area).

Checked in the game: the SC library reads both files (async NAND path), the title is drawn for 16:9
(4:3 without SYSCONF before), and with language 3 the strap screen, ESRB notice and title are in
French.

## Other devices (`ios_devices.cpp`)

| Device | Port |
|---|---|
| `/dev/es` | GetTitleID (0x20) = 00010000-52555545 (PAL 52555550), GetDataDir (0x1D), GetDeviceID (0x07, fixed), GetTitlesCount/GetTitles and owned titles (System Menu + this game), GetTicketViewsCount/GetTicketViews (one view), GetConsumption (no limits). LaunchTitle (0x08, leaving to the Wii Menu) ends the program like "Quit game". Others: logged, `-4`. |
| `/dev/stm/immediate`, `/dev/stm/eventhook` | hot reset / power off / standby end the program; LED and dimming accepted. The OS's own event hook is HLE'd (os/OSStateTM.c): `ios::stmEvent(0x800 power / 0x20000 reset)` calls the game's OSSetPowerCallback / OSSetResetCallback callback as the OS handler does (City Folk registers a reset callback, 0x80107870, which checks the disc with DVDCheckDiskAsync). |
| `/dev/di` | accepted (disc reads are HLE'd at the DVD library). |
| `/dev/sdio/slot0`, `slot1` | **SD slot with no card**: GetStatus (0x0B) = 0, card commands fail. Photos to SD: later (see "Not done"). |
| `/dev/usb/kbd` | **absent** (open fails, `-6`): no USB keyboard. A device version (open succeeds, the event request waits forever = no keyboard attached) exists behind `CF_USB_KBD_DEVICE=1`. |
| `/dev/usb/oh0` | USB host with no devices (Wii Speak absent); not opened by the game so far. |
| `/dev/net/*` (KD = WiiConnect24, NCD, IP, SSL, WD = DS wireless) | **not running** (open fails, `-6`): the game is offline. NWC24 retries `/dev/net/kd/request` every ~2 s at the title (logged once). Online play: docs/ONLINE.md. |

Leaving the game: OSShutdownSystem, OSRestart, OSResetSystem, OSReturnToMenu (and the
data-management / settings / error variants), `__OSHotReset`, `__OSShutdownToSBY` end the program
the way "Quit game" does (`menu::requestQuit`); the calling thread never returns.

## Miis

The game ships its own Mii parts (`files/Face/RFLRes01.arc`, passed to RFLInitRes), so only the
**database** `/shared2/menu/FaceLib/RFL_DB.dat` comes from the NAND. We leave it absent: the RFL
library's own fallback for that case (RFLiBootLoadDatabaseAsync: open fails with
NAND_RESULT_NOEXISTS -12 -> RFLiClearDBBuffer, an empty database, no error) is what a console
whose Mii Channel was never used does. A player's own database copied there
(`nand\shared2\menu\FaceLib\RFL_DB.dat`, from their Wii) is read as on the console. Character
creation in City Folk does not use Miis (Rover's questions); Mii masks at Shampoodle read the
database (empty: none offered). Not reached in a test yet (no input past the title, see below).

## Clock

The guest's time base counts from 2000-01-01 in the host's **local** time (`runtime.cpp
initTimeBase`, as a Wii's clock is set), plus the clock offset: F1 > Game > Console "Clock
offset" (days / hours, `[system] clock_offset` in seconds), `CF_CLOCK_OFFSET=<s>` overrides (test
aid). Applies at start (the time base must not jump under running alarms). OSGetTime /
OSTicksToCalendarTime / OSCalendarTimeToTicks are HLE'd on it. IPL.CB (counter bias) is 0. The
game's own "set the clock" keeps its own offset in the save, as on the console.

## Testing

All runs use scratch `APPDATA`, `CF_NAND_DIR`, `CF_CACHE_DIR`, `CF_CARD_DIR` (never the user's).

- `CF_IOS_LOG=1`: every open (with fd or error) and unhandled request; `2`: every request with
  its result.
- `CF_NAND_SELFTEST=1`: after the game's NANDInit, the game's own NAND library runs a save-like
  sequence on scratch files in the data folder and logs `[nand] selftest PASSED/FAILED`: home
  folder, open missing (-12), create, create again (-6), open RW, two writes, seek, position,
  read back and compare, seek to end, read at end (0), close, GetStatus (owner 0x1001, group
  0x3031, permissions as created), GetType (file), NANDCheck (36 blocks fit), the
  SimpleSafeOpen / write / SimpleSafeClose a save uses (`/tmp/sys/` copy renamed back) and
  reading its result, delete, delete missing (-12). `CF_NAND_SELFTEST=keep` leaves the test file;
  the next run reads it back (restart persistence). Both PASSED (2026-10-27).
- `CF_STM_EVENT=<s>[:power|reset]`: presses the console button that long after boot.
- `CF_CLOCK_OFFSET=<s>`: clock offset for one run (`[time] console clock offset` log line).
- Language / aspect: a scratch settings.ini with `[system]` `language=3` -> French strap screen
  and title (`[sys] SYSCONF created: language French, 16:9, stereo, sensor bar below`).

Not testable yet (2026-10-27): a new game up to the first save and a restart that loads it. The
title screen does not react to scripted buttons (A, START, ...) on main either (the Wii Remote
HLE is the wpad agent's work), and with Aurora's FIFO thread the title intro ends in
`FIFO processing range ... is outside buffered range` (AURORA_FIFO_INLINE=1 avoids it; rendering
work). The save path below the game (the NAND library on our IOS) is covered by the self-test; the
game itself so far only checks for `rvforest.dat` at boot (not found -> new game).

## Not done / next

- **First save and load in the game** once input reaches the bus scene and Rover's questions
  (needs the Wii Remote HLE's pointer for the name keyboard): check `rvforest.dat` / `banner.bin`
  creation (sizes, the NAND banner via NANDInitBanner), the save icon and bed save, a restart
  loading the town, and the "Saving..." duration against a console.
- **SD card**: the game reaches the card through `/dev/sdio/slot0` with its own FAT code (VF has
  no SD driver linked). Plan: an SD host (ioctls ResetCard 4, SetClock 6, SendCommand 7 with
  CMD17/18/24/25 block reads/writes, GetStatus 0x0B, GetOCR 0x0C) over a FAT image in
  `%APPDATA%\...\sd\sd.img` (2 GB, FAT16 like the game expects: no SDHC), with F1 buttons to
  open the photo folder (copy JPEGs out of the image). Until then: slot empty.
- **USB keyboard**: a real keyboard for letters through `/dev/usb/kbd` events (attach, key
  reports) from SDL keyboard input.
- **WiiConnect24 / network**: `/dev/net/kd/*` as "WiiConnect24 off" (requests answered with the
  library's disabled error) instead of absent, then NCD/SO for online play (docs/ONLINE.md).
- **Reset button**: wire F1 / a key to `ios::stmEvent(kStmEventReset)` (the game's callback runs
  and checks the disc; what it does next is not seen yet: probably its own soft reset to the
  title, then OSRestart).
- **Japan base**: title id 00010000-5255554A, setting.txt `AREA=JPN`, language Japanese
  (`ios::titleId`, `sysconf.cpp` only know USA and PAL so far).
