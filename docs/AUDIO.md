# Audio

City Folk's sound is NintendoWare `nw4r::snd` (sound archive `Sound/rv_forest.brsar`, one stream
`Sound/stream/title.brstm`) on the RVL SDK's **AX** library, which drives the audio DSP with its own
microcode. All of NW4R, AX and AXFX run unchanged as recompiled game code. The port replaces two
things only:

- the **DSP**: `src/recomp/audio_dsp.cpp` is a stand-in for the AX microcode. It takes the command
  list AX mails each audio frame, mixes on the host and writes back into guest memory exactly what
  the microcode writes (PBs, aux uploads, delayed surround, the AI buffer, Wii Remote speaker
  buffers);
- the **AI** (audio interface): `src/recomp/hle_audio.cpp` plays the AI DMA blocks through SDL3
  (`audio_out.cpp`, shared with LM) and raises the AI interrupt paced by the host device, as in LM
  (LM's docs/AUDIO.md "AI output and pacing").

Nothing here comes from Dolphin. The command set, the parameter block layout and every arithmetic
detail below were read from the game: the CPU side from the recompiled AX library (disassembly of
`__AXNextFrame`, `__AXSyncPBs`, `__AXServiceVPB`, `__AXOutNewFrame`, ...), the DSP side from the
microcode image in main.dol with LM's own DSP disassembler (`tools/dsp_disasm.py` in the LM repo,
written from the public DSP instruction set):

    python <LM>/tools/dsp_disasm.py "US DATA/DATA/sys/main.dol" 80542900 2000

Microcode addresses below (`uc 0x....`) are DSP instruction addresses in that listing.

## The frame loop (CPU side, AX of this SDK)

- 96 voices (`__AXVPBInit`), each a CPU-side `AXVPB` of 0x168 bytes (+0x18 index, +0x1C sync
  flags, +0x20 depop flag, +0x24 ITD buffer, +0x28 a copy of the PB) and a DSP PB of 0x140 bytes in
  one array (`__AXGetPBs`). `__AXSyncPBs` copies the changed fields (by sync flag) into the DSP PBs
  before each frame and drops voices over the DSP cycle budget (`AXSetMaxDspCycles`; the cycle
  estimate is all CPU side, the DSP reports nothing); `__AXServiceVPB` reads state, envelope volume
  and current address back.
- A frame is **96 samples at 32 kHz (3 ms)**. AI DMA blocks are 0x180 bytes (96 stereo frames),
  double-buffered (`__AXOutInit`, mode 0). Each AI interrupt (`__AXOutAiCallback`): if the DSP has
  yielded (state 1) `__AXOutNewFrame` runs: sync PBs, mail the command list built last frame
  (`0xBABE0080`, then its address), run the voice callbacks, `__AXProcessAux` (the AXFX callbacks on
  the aux buffers), the user callback (NW4R's sound frame), build the next list (`__AXNextFrame`)
  and `AIInitDMA` the block the list just sent fills. If the DSP has not yielded yet (state 0) AX
  sets state 2 and the DSP's resume callback starts the frame instead (the AI repeats its block).
- Aux A/B/C buffers: three rotating slots (DSP writes "input", DSP reads "output", CPU processes the
  third in place), 0x600 bytes per slot for A/B (L, R, S and the DPL2 Rs: 4 x 96 s32), 0x480 for C.
- Wii Remote speakers: four rings of 180 samples (6 kHz), 18 samples per frame.

## Command list

16-bit words; addresses are two words (high, low). `__AXNextFrame` builds, in this order:

| cmd | name (ours) | parameters | microcode |
|---|---|---|---|
| 0 | SETUP | studio block | uc 0x008A |
| 1 / 2 / 3 | fold surround: stereo / surround / DPL2 | last frame's surround buffer | uc 0x0222 / 0x024F / 0x0E83 |
| 4 | PROCESS | first PB | uc 0x027E |
| 5 / 6 / 7 | aux A / B / C round trip | return volume, input, output | uc 0x049B / 0x04B4 / 0x04CD |
| 8 / 9 | aux A / B round trip, DPL2 | return volume, input, input Rs, output L, R, Ls, Rs | uc 0x0DBD / 0x0DDF |
| 10 | COMPRESSOR | threshold (0x8000), frames (10), gain table (0x80541940) | uc 0x057B |
| 13 | Wii Remote output | 4 buffers | uc 0x067C |
| 11 / 12 | OUTPUT / OUTPUT DPL2 | master volume, surround buffer, AI buffer | uc 0x060B / 0x0EC6 |
| 14 | END (task yields: 0xDCD10002) | - | uc 0x0672 |

Aux commands appear only for registered aux callbacks; 1/2/3 and 11/12 depend on `AXSetMode`
(0 stereo, 1 surround, 2 DPL2). Unknown commands: the microcode mails 0xBAAD and halts; we log and
drop the list.

## DSP memory and buses

Buses are 96 32-bit samples (high word, low word) in DSP data memory; our `sBus` mirrors them by
DSP address: main L 0x0000, R 0x00C0, S 0x0180; aux A 0x0400/0x04C0/0x0580; aux B
0x0640/0x0700/0x07C0; aux C 0x0880/0x0940/0x0A00; remote main 0x0240 + 0x24*n, remote aux
0x0AC0 + 0x24*n (18 samples). In DPL2 mode S is the left surround and the aux C slots carry the
right surround of main / aux A / aux B. The PB is DMA'd to DSP 0x02D0, the voice buffer is 0x0C60
(96 samples) after 32 samples of ITD history at 0x0C40.

Microcode state kept across frames: previous master / aux return volumes (0x0CE5..8, 0x8000 at
boot), the compressor counter (0x0CE4).

## Parameter block (DSP PB, 0x140 bytes)

| bytes | field |
|---|---|
| 0x00 / 0x04 | next PB / this PB (write-back address) |
| 0x08 / 0x0A | srcSelect (0 polyphase, 1 linear, 2 none) / coefSelect (table 0-2) |
| 0x0C | mixerCtrl (u32, below) |
| 0x10 / 0x12 | state (1 = playing) / type (1 = stream: keeps the ADPCM history across loops) |
| 0x14-0x43 | mix: vL dL vR dR, aux A L/R, aux B L/R, aux C L/R (vol, delta each), vS dS, aux A/B/C S |
| 0x44-0x51 | ITD: on, buffer, shift L/R, target shift L/R |
| 0x52-0x69 | depop (written by the DSP): L, AL, BL, CL, R, AR, BR, CR, S, AS, BS, CS |
| 0x6A / 0x6C | volume envelope: current, delta |
| 0x6E-0x7D | address: loop flag, format (0 ADPCM, 0x0A PCM16, 0x19 PCM8), loop, end, current |
| 0x7E-0xA5 | ADPCM: 16 coefficients, gain, pred/scale, yn1, yn2 |
| 0xA6-0xB3 | resampler: ratio (16.16), position fraction, last 4 samples (oldest first) |
| 0xB4-0xB9 | ADPCM loop context: pred/scale, yn1, yn2 |
| 0xBA-0xC1 | low-pass: on, yn1, a0, b0 |
| 0xC2-0xD5 | biquad: on, xn1 xn2 yn1 yn2, b0 b1 b2 a1 a2 |
| 0xD6 / 0xD8 | remote on / remote mixer control (2 bits per send: off, on, ramp) |
| 0xDA-0xF9 | remote mix: main0, aux0, main1, aux1, ... (vol, delta) |
| 0xFA-0x109 | remote depop: main 0-3, aux 0-3 |
| 0x10A-0x113 | remote resampler: fraction, last 4 samples |
| 0x114- | remote filter: type (1 low-pass, 2 biquad) + its parameters |

mixerCtrl: bits 0-4 main, 16-20 aux A, 21-25 aux B, 26-30 aux C; in each group 1 = L, 2 = R,
4 = L+R with ramp (implies both), 8 = S, 16 = S with ramp. Bit 31 feeds aux C L/R from the voice
before the ITD delays (uc dispatch tables at IRAM 0x0D13/0x0D33/0x0D53/0x0D73).

NW4R's `AxVoiceParamBlock::SetVoiceSrcType` maps its types to srcSelect/coefSelect: 0 none (2),
1 linear (1), 2/3/4 polyphase with table 0/1/2. In the bus scene it uses table 0 for ratios above
~1.33 (the 44.1 kHz title stream: 1.378) and table 2 for the rest.

## Per voice (PROCESS, uc 0x027E)

1. **Accelerator + resampler** (uc 0x071A / 0x076B / 0x07C0). Samples come from the accelerator:
   ADPCM (nibble addresses, header every 16 nibbles, `y = (n*2^scale*2048 + 1024 + c1*y1 +
   c2*y2) >> 11` saturated), PCM16 (`s*gain >> 11`), PCM8. Passing the end address jumps to the
   loop address and raises the accelerator exception, whose handler (uc 0x0F61) stops a voice
   without the loop flag (state 0; the rest of its frame reads a zero word) or reloads pred/scale
   (and, unless the voice is a stream, yn1/yn2) from the loop context.
   Polyphase: per output the position advances by the ratio, the new input samples go into a
   4-sample ring, output = `sat16(sum x_k c_k >> 15)` with the phase = top 7 bits of the fraction
   (128 phases). Linear: between the two oldest ring samples with 16-bit weights. None: one sample
   per output. History and fraction go back into the PB.
2. **Volume envelope** (uc 0x034C): sample i times `vol + i*delta` (unsigned 1.15), `>> 15`
   saturated; the PB keeps `vol + 96*delta`.
3. **Low-pass** `y = (a0 x + b0 y') >> 15` (unsigned coefficients), then **biquad**
   `y = (b0 x + b1 x' + b2 x'' + a1 y' + a2 y'') >> 14` (2.14), both truncating and saturating.
4. **Mixer** (uc 0x0BD1/0x0BEB constant, 0x0C21/0x0C51 ramped): `bus += (s * v) >> 15`, ramps use
   `v + i*delta` and leave `v + 96*delta` in the PB; each send stores its last sample's
   contribution as the voice's depop value.
5. **Wii Remote** (uc 0x03A2): optional low-pass/biquad, a 32 -> 6 kHz polyphase step of 0x55555
   (coefficient table 0) into 18 samples, up to 8 sends (main/aux per remote).
6. ITD shifts move one sample per frame toward their targets; the newest 32 samples go back to the
   ITD buffer. The whole PB is written back.

## The other commands

- SETUP fills every bus with the studio block's depop ramp (s32 start + s16 step, from voices that
  stopped last frame; `__AXDepopFadeMain`) or zeros.
- Fold surround adds last frame's S (uploaded by OUTPUT) into L/R: stereo L+S R+S, surround L-S
  R+S, DPL2 L+Ls R-Rs (a one-frame surround delay).
- Aux round trip: the aux bus (L, R, S) goes to `input` for the AXFX callback; `output` (processed
  by the CPU earlier) comes back and is added to main L/R/S at a return volume ramped from last
  frame's value in 96 steps of 1/96 (multipliers 0x2AB/0x2AA, uc 0x04E6).
- Compressor: if main L or R reaches the threshold this frame the counter restarts at `frames` and
  the frame is scaled by gain ramp `table[old counter]`, else each frame steps the counter down
  with `table[0x840 + counter*0xC0]` (96 unsigned 1.15 gains each; the tables are game data).
- OUTPUT: S to the surround buffer, then main R and L at the ramped master volume, saturated, into
  the AI buffer, **right first** (the console's DAC plays the first sample of a frame on the right;
  audio_out.cpp's `kRightFirst`).
- Wii Remote output: each remote's main bus saturated into its ring (the remote aux buses are not
  output by this microcode).

## What is ours, not the console's

- **The polyphase coefficient tables** live in the DSP's coefficient ROM (0x1000/0x1200/0x1400), not
  in the game. `coefs()` in audio_dsp.cpp computes 4-tap Hann-windowed sincs with cutoffs at
  0.5 / 0.75 / 1.0 of the input Nyquist frequency (the SDK names them 8K / 12K / 16K for a 32 kHz
  output), centred between the second and third tap, unity gain per phase; table 2 at phase 0 is
  the identity. The console's exact response is unknown here: high frequencies of resampled
  voices may differ slightly.
- PCM8 gain scaling (no voice uses PCM8 so far) and the half-documented ADPCM case of a current
  address one nibble past a header.
- DSP time: the resume callback (the yield after END) reaches the CPU `CF_AX_DSP_US` microseconds
  (default 400) after the list; the mix itself happens at the mail, which is safe because AX synced
  the PBs before sending and touches none of the list's outputs until the next frame.

## Settings

- F1 > Sound > Reverb / echo off: the aux returns are not mixed (AXFX still runs).
- F1 > Sound > Voice filters off: no per-voice low-pass/biquad (main and remote).
- F1 > Sound > Master / Music / Sound effects / Voices / Wii Remote speaker volume: below.
- F1 > Sound > Output: Stereo / Mono / Surround (matrix-encoded) / 5.1 / 7.1 (discrete): below.
  Stereo is the default and leaves the output as before (byte-identical AI stream).

## Volume sliders

The console has no such sliders, so they are applied in the DSP stand-in per voice, as a gain on
the voice's 96 samples after the envelope and filters and before the mixer (so its reverb sends,
Wii Remote sends and depop values follow), ramped from last frame's gain over the frame (no click
when a slider moves). With every slider at 100 % nothing is multiplied: the mix stays bit-exact.
Master is applied at the output (audio_out.cpp) to everything, the Remote speaker included.

Which slider a voice follows is decided by the **NW4R sound that owns it**, found by following
pointers the game's own NW4R snd code keeps (read from the recompiled code, USA addresses; PAL and
Japan by symbol name):

1. The DSP PB's index in AX's PB array (`__AXGetPBs` is `lwz r3, d(r13)`: the SDA global; the
   ITD buffers and the CPU-side AXVPBs (0x168 bytes each) are the next two globals, in the order
   `__AXVPBInitCommon` clears them) gives the **AXVPB**; its user context (+0x14, AXAcquireVoice's
   third argument) is NW4R's **AxVoice** when `AxVoice +0x38` (its callback) is
   `Voice::AxVoiceCallbackFunc` (else the voice is not NW4R's, e.g. the HOME Menu's).
2. `AxVoice +0x3C` = the **Voice** (`AxVoiceManager::AcquireAxVoice` stores callback and data at
   +0x38/+0x3C); `Voice +0x94/+0x98` = its callback and data (`Voice::InitParam`):
   - `Channel::VoiceCallbackFunc` -> a **Channel**, whose callback and data (+0xE0/+0xE4,
     `Channel::InitParam`) are `SeqTrack::ChannelCallbackFunc` + the SeqTrack (its SeqPlayer at
     +0xC0, the pointer `ChannelCallbackFunc` calls through) or `WsdPlayer::ChannelCallbackFunc` +
     the WsdPlayer;
   - `StrmPlayer::VoiceCallbackFunc` -> one of the StrmPlayer's tracks (`StrmPlayer::AllocVoices`:
     0x38-byte tracks from +0xB78, the count at +0x82C), so the player is found by trying the
     track numbers.
3. Seq/Strm/Wave sounds hold their player at +0x10C (`GetBasicPlayer`), the BasicSound has its
   **SoundPlayer** at +0x10 (`AttachSoundPlayer`). Its index in the SoundArchivePlayer's array
   (+0x3C, count +0x38, 0x64 bytes each: `SetupSoundPlayer`, hooked in hle_audio.cpp to learn the
   array) is the **player id** of `rv_forest.brsar` (the USA, PAL and Japanese archives have the
   same 30 players and 9244 sounds).

Player ids (from the archive's symbol and player tables):

| kind | players |
|---|---|
| music | 21 PLAYER_STRM (title.brstm), 22 PLAYER_MUSIC (K.K. songs, records: BGM_MU*), 23 PLAYER_CONDUCTOR, 24 PLAYER_FANFARE (jingles: BGM_FANFARE_*), 25 PLAYER_BGM (hourly music, KK, bus, ...) |
| voices | 15 PLAYER_DOUBUTSUGO (animal speech syllables), 16 PLAYER_KEYBOARD (the per-letter speech sounds of each language, SE_Keyboard_<lang>_*) |
| sound effects | everything else: 0-2 the town tune melodies (chime, editor, instruments), 3-7 system/menus/cursor, 8 walking, 9 furniture, 10 actions, 11 environment, 12/13 reactions, 14 environment loops, 17 money, 18 furniture loops, 19 dances, 20 fireworks, 26 gyroids, 27/28 TV |

A voice that cannot be classified counts as a sound effect. NW4R lets go of a Voice before its
release has died away (the AX voice plays out without an owner): such a voice keeps the kind it
had; a voice that starts takes its kind and gain at once.

Checked: with `CF_AUDIO_LOG=1` every voice start names its kind and player; across the title,
the bus scene and the town every voice start was classified (`CF_AUDIO_KIND_LOG=1` logs any that
is not, with the step that failed). Level checks with `CF_SETTINGS_AT` switching sliders during
one run: see "Verified".

## Wii Remote speaker on the PC

What a Remote's speaker plays is what the game streams to it: `WPADSendStreamData` packets (20
bytes = 40 samples, one output report 0x18 each) of 4-bit Yamaha ADPCM at 6 kHz, after
`WPADControlSpeaker` turned the speaker on (1) and started playback (4). Two writers exist in City
Folk: NW4R's RemoteSpeaker (encodes the DSP's Remote output, `AXRmtGetSamples`, with WENC) and the
HOME Menu (homebutton::RemoteSpk: its own sounds, PCM scaled by its speaker volume level / 10, then
`WENCGetEncodeData` and `WPADSendStreamData`). `src/recomp/audio_remote.cpp` decodes the stream,
which covers both and plays what the real speaker gets (its ADPCM noise included); hle_wpad.cpp
hands it every packet and speaker command.

- Decoder (from the public description of Yamaha 4-bit ADPCM): per nibble `diff = (2|n|+1) *
  step / 8` added or subtracted (bit 3 = sign), `step = step * {230,230,230,230,307,409,512,614}[|n|]
  / 256` kept in 127..24576, prediction saturated to 16 bits; start state 0 / 127, reset at speaker
  off/on/play. **High nibble first**: checked on the HOME Menu's streams (the low-nibble-first
  decode drifts into saturation; high-nibble-first gives four clean decaying clicks, peaks in
  proportion to the HOME Menu volume steps 6/7/6/5).
- Rate: the HOME Menu sends 150.0 packets/s while a sound plays (6003 samples/s measured,
  `CF_AUDIO_LOG`), i.e. the speaker's 6 kHz.
- Each Remote has a queue (playback starts at 40 ms queued, at most 120 ms kept); once per AI block
  audio_out.cpp pulls the summed queues resampled to the AI rate (8-tap Kaiser-windowed sinc,
  cutoff 0.9 of the 3 kHz Nyquist frequency) through a one-pole 250 Hz high-pass (the small
  speaker has no bass).
- Choices (not the console's): it plays **in the centre** (the centre speaker with 5.1/7.1, else
  0.707 on each side), 3 dB under the TV mix at 100 %, scaled by F1 > Sound > Wii Remote speaker
  volume (default 100 %, 0 = off) and Master. The game's speaker volume (`WPADSetSpeakerVolume`)
  is not applied again: the HOME Menu already scales its samples by it. Remotes that are not
  connected, speaker off or muted play nothing.
- Where City Folk uses it: the HOME Menu (opening it turns the speaker on; Wii Remote Settings >
  Volume +/- plays a test click on the Remote). The game's own sounds never had a Remote send in
  the title, bus or town scenes (`remote 0.0` per frame in `CF_AUDIO_LOG`; NW4R's RemoteSpeaker
  is linked but no sound used a Remote output line there).
- Verified: HOME Menu at the title, Volume + twice and - twice: 4 bursts of 119 packets, decoded,
  in `CF_AUDIO_WAV` (device output) at ~700 rms in 100 ms windows where the AI stream (`CF_AUDIO_
  WAV_RAW`, the TV) has 10-120. `CF_RMT_DUMP=<file>` writes the raw stream per Remote.
- The DSP's own Remote buffers (command 13) are still written for the game (`axRemoteFrame` gives
  the last frame's samples); they are not mixed on the PC, as their sound arrives through the
  stream.

## Surround output

AX mixes in one of three modes (`AXSetMode`, from NW4R's output mode, from the console's sound
setting in SYSCONF, which the port writes from F1 > Sound > Output: Mono 0, Stereo 1, any surround
choice 2): stereo, surround (Dolby Pro Logic: one surround bus S) or DPL2 (Ls on the S bus, Rs on
the aux C slot). Each frame the fold command adds last frame's surround into the fronts (stereo
L+S R+S, surround L-S R+S, DPL2 L+Ls R-Rs). Matrix-encoded surround (F1 "Surround") is that
stereo signal, unchanged. For discrete output (F1 "Surround 5.1" / "7.1", a device with 6/8
channels; WAV captures without a device use the chosen layout) the DSP keeps, per AI block, what
the fold added (at the output's master volume ramp) and the surround buses themselves;
audio_out.cpp subtracts the fold from the fronts and plays Ls/Rs (surround mode: S on both) on
the surround speakers (7.1: side and back at 0.707 each), the Remote speaker on the centre, LFE
silent (the game has no LFE or centre bus: its centre stays a phantom centre). In stereo mode
(the game mixing stereo) the extra channels stay silent. Stereo (the default) never uses side
data: the fold-down is the console's.

## Verified

- Title (`title.brstm`, 44.1 kHz stereo DSP-ADPCM stream, ratio 1.378, table 0): the decoded
  history the DSP writes back matches an independent Python decode of the file bit for bit; the
  output matches that decode resampled by an ideal 64-tap sinc with correlation 0.9999, gain 1.000,
  36-38 dB match SNR (the residue is the 4-tap filter), channels in the right order. Peaks
  -4.7 dBFS, no clipping. Tones at 110/220/330 Hz (A major): pitch right.
- New-game bus scene: the music sequence (PCM16 and ADPCM instrument notes, ratios 0.44-2.12, panned
  sends) and Rover's speech (one-shot ADPCM syllables at ratio ~0.5); peaks around -10 dBFS, no
  clipping, nothing at the 96-sample frame edges (mean second difference flat over the frame phase).
- AX's frame loop runs at real time: 333 frames per second against 60 guest frames per second.
- A 5-minute run through the bus dialogue: no clipped sample, flat frame-phase profile; repeated
  AI blocks only where the host stalled the guest (title appearing, the bus interior loading) and
  at shutdown.
- Device path (`CF_AUDIO=1` with `SDL_AUDIO_DRIVER=disk`, no speakers): 3 ms blocks paced by the
  device, queue ~44 ms on average, runs dry only during those host stalls.
- These runs used the render-1 branch merged locally (without it, Aurora's FIFO error at the title
  often aborts the run before the bus scene); the AX code is the same on both.

Tools (scratch, not in the repo): per-second level/clip/correlation stats, a frame-phase check and
the brstm cross-check were small numpy scripts over `CF_AUDIO_WAV_RAW` captures.

## Test aids

- `CF_AUDIO_WAV_RAW=<file>`: the AI stream (stereo, left first, 32028 Hz). `CF_AUDIO_WAV=<file>`:
  what goes to the device. Test runs: `CF_AUDIO=0` (no device, timer pacing).
- `CF_AUDIO_LOG=1`: the first command lists, every voice start (format, src/coef, ratio, loop,
  addresses, mixer control, volumes, filters, ITD, remote), and once a second: voices playing,
  starts, filter/remote/ITD/aux use, output peaks, clipped samples, compressor counter; AI pacing.
- `CF_AX_TRACE=<PB address>`: that voice's state at each frame start and its first/last samples.
- `CF_AX=0`: lists finish without mixing (silence; the old boot stand-in) for A/B tests.
- `CF_AX_DSP_US=<us>`: DSP time before the resume callback.
- `CF_IRQ_STALL=1` (+ `CF_TRACE=ring`): also logs every interrupt handler that ran over 20 ms
  with its recent functions.

## Known issues

- **Interrupt stall at the title** (not audio code): the VI pre-retrace interrupt
  (`PreRetraceCallback` -> `EGG::AsyncDisplay::preVRetrace` -> `XfbManager::setNextFrameBuffer` ->
  `VISetNextFrameBuffer`, the frame handoff) blocked inside the interrupt handler for 1.5-2.4 s when
  the title appears (also with a warm shader cache), holding every interrupt: the AI repeats its
  last block for that time (a short stutter as the title music starts). Shown by `CF_IRQ_STALL=1
  CF_TRACE=ring` ("interrupt handler ran 2384 ms"). For the VI/rendering side: the handoff should
  not wait inside interrupt context.
- Reverb (AXFX on aux buses), ITD, filters and the Wii Remote speaker were not exercised by the
  scenes reached so far (no voice sent to aux, used a filter or the remote): implemented from the
  microcode, unverified in play.
