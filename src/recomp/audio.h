#pragma once
// Audio back end shared by hle_audio.cpp (AI and DSP-task SDK HLE), audio_dsp.cpp (our
// stand-in for the AX DSP microcode) and audio_out.cpp (host output through SDL3, WAV
// capture). See docs/AUDIO.md.
#include "recomp/ctx.h"

#include <chrono>

namespace recomp::audio {

// The console's real AI sample rates: the "48 kHz" clock is 54 MHz / 1124 and the
// "32 kHz" rate two thirds of it (docs/AUDIO.md, "Output rate").
constexpr double kConsoleRate48 = 54000000.0 / 1124.0;      // 48042.7 Hz
constexpr double kConsoleRate32 = kConsoleRate48 * 2 / 3;  // 32028.5 Hz

// ---- DSP (audio_dsp.cpp): AX microcode stand-in ----
// One AX command list of `bytes` bytes at guest address `addr` (the mail pair 0xBABE<bytes>,
// addr that __AXOutNewFrame sends): mixes one 3 ms frame into the AI buffer it names.
void axCommandList(u32 addr, u32 bytes);
// The task's resume callback for that list (the DSP yielded) reached the CPU.
void axFrameDone();
// The last frame's 18 samples (6 kHz) for Wii Remote speaker 0-3, as written to the game's
// remote buffers.
const s16* axRemoteFrame(int remote);
// The game's SDA base (r13), from the DSP mail HLE: AX's voice tables are SDA globals.
void axSetSdaBase(u32 r13);
// nw4r::snd::SoundArchivePlayer::SetupSoundPlayer ran: `players` SoundPlayer objects of
// `stride` bytes from `base` (index = the sound archive's player id; docs/AUDIO.md
// "Volume sliders").
void noteSoundPlayers(u32 base, u32 count, u32 stride);
// Host thread safe: a DSP frame is in flight (from its command list until the resume callback
// was delivered), and the steady_clock time the last one ended.
bool dspFrameBusy();
std::chrono::steady_clock::time_point dspFrameEnd();
// The DSP has mixed the next AI block for the last AI interrupt (the AX command list sent from
// that interrupt; hle_audio.cpp waits for it before raising the next one).
void noteGuestMixed();

// Per output sample of the DSP's stereo mix, the parts that did not come from the front
// buses, at the output level (after the master volume): lExtra / rExtra are what the
// surround fold (AX surround: L-S R+S; Dolby Pro Logic II: L+Ls R-Rs) added to the left /
// right output, sl / sr the surround buses on their own. A discrete multichannel output
// takes the extras back out of the fronts and plays sl / sr on the surround speakers.
struct SideSample {
    float lExtra = 0, rExtra = 0, sl = 0, sr = 0;
};
// What the DSP mixed for one AI block besides its stereo samples (one AX frame = 96
// samples at 32 kHz): surround side data, only when the game mixes in surround or DPL2 mode.
struct FrameExtras {
    static constexpr int kSamples = 96;
    bool hasSide = false;
    SideSample side[kSamples];
};
// The extras of the AI block at guest address aiAddr (the OUTPUT command's AI buffer).
// Returns false when the DSP has not mixed that buffer.
bool dspFrameExtras(u32 aiAddr, FrameExtras& out);
// The AX output mode of the last command list: 0 stereo, 1 surround, 2 DPL2.
int axMode();

// ---- ARAM (hle_ar.cpp) ----
// Host view of `len` bytes of ARAM at `addr`, or nullptr when out of range.
const u8* aramRead(u32 addr, u32 len);

// ---- Output (audio_out.cpp) ----
// Starts the host device (once). `rate` is the AI DSP sample rate in Hz.
void outStart(double rate);
void outSetRate(double rate);
// Queues one AI DMA block: big-endian s16 stereo frames read from guest memory.
void outPushGuestBlock(u32 addr, u32 bytes);
// Stereo frames of the AI stream pushed so far (= the CF_AUDIO_WAV_RAW sample position), for test logs.
unsigned long long outRawFrames();
// Seconds of audio queued on the host device (0 when there is no device).
double outQueuedSeconds();
// Closes the device stream before Aurora shuts SDL down (SDL_Quit frees every stream; the
// AI DMA thread would otherwise use the freed one; found on Linux).
void outShutdown();
bool outHasDevice();
// Host pause (window lost focus): stop the device and the audio interrupt pacing; on resume
// drop stale queued audio so playback restarts cleanly.
void outSetPaused(bool paused);
bool outPaused();
// Channel count of the host playback device (0 = unknown / no device), for the menu.
int outDeviceChannels();
// What the output is doing now, for the menu ("Stereo, 48000 Hz, windowed-sinc", ...).
const char* outStatus();

// ---- Music / sound-effect / voice split for the volume sliders (audio_dsp.cpp) ----
// An AX voice is followed to the NW4R sound that owns it (AXVPB -> AxVoice -> Voice ->
// Channel -> SeqTrack / WsdPlayer, or StrmPlayer -> the sound -> its SoundPlayer), and the
// sound player's id in rv_forest.brsar decides: BGM / music / fanfare / stream players are
// music, the animal speech players (DOUBUTSUGO, KEYBOARD) voices, the rest sound effects.
// docs/AUDIO.md "Volume sliders".
enum class VoiceKind { Unknown, Music, Effect, Voice };

// ---- Wii Remote speakers on the PC (audio_remote.cpp; fed by hle_wpad.cpp) ----
// WPADControlSpeaker(chan, cmd) was accepted (0 off, 1 on, 2 mute, 3 unmute, 4 play).
void remoteSpeakerCommand(int chan, int cmd);
// WPADSendStreamData(chan, data, len): `len` bytes of 4-bit ADPCM at guest address `data`;
// `audible` = the Remote is connected and its speaker on and not muted.
void remoteStreamData(int chan, u32 data, u32 len, bool audible);
// `n` samples at `outRate` of what the Remote speakers play now (summed, full scale 32768);
// false (and nothing written) when no speaker is playing. Called once per AI block.
bool remoteFetch(float* out, int n, double outRate);

// Debug logging (CF_AUDIO_LOG=1).
extern bool g_log;

}  // namespace recomp::audio
