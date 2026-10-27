#pragma once
// Audio back end shared by hle_audio.cpp (AI/DSP SDK + JAudio driver hooks),
// audio_dsp.cpp (our stand-in for the JAudio DSP microcode) and audio_out.cpp
// (host output through SDL3, WAV capture). See docs/AUDIO.md.
#include "recomp/ctx.h"

#include <chrono>

namespace recomp::audio {

// The console's real AI sample rates: the "48 kHz" clock is 54 MHz / 1124 and the
// "32 kHz" rate two thirds of it (docs/AUDIO.md, "Output rate").
constexpr double kConsoleRate48 = 54000000.0 / 1124.0;      // 48042.7 Hz
constexpr double kConsoleRate32 = kConsoleRate48 * 2 / 3;  // 32028.5 Hz

// ---- DSP (audio_dsp.cpp) ----
// One command packet from JAudio's DSPSendCommands(cmds, count).
void dspCommand(const u32* words, u32 count);
// DSPReleaseHalt: the CPU finished its per-subframe channel updates; mix the next
// subframe. Returns true when a subframe was mixed (the DSP interrupt is due).
bool dspRunSubframe();
// Host thread safe: a DSP frame is being mixed (from its sync-frame command to its last
// subframe), and the steady_clock time its last frame finished.
bool dspFrameBusy();
std::chrono::steady_clock::time_point dspFrameEnd();
// The game has mixed the DSP output into the next AI block for the last AI interrupt
// (Jac_imixcopy; hle_audio.cpp waits for it before raising the next one).
void noteGuestMixed();

// Per output sample of the DSP's stereo mix, the parts that did not come from the
// front buses, at the output level: lExtra / rExtra are what the centre, surround and
// rear-right buses added to the left / right output; c, s and rr are those buses on
// their own (c at full level, not the 0.707 it gets in each side). A discrete
// multichannel output takes the extras back out of the fronts and plays c, s and rr on
// their own speakers.
struct SideSample {
    s16 lExtra = 0, rExtra = 0, c = 0, s = 0, rr = 0;
};
// Side data of the DSP output buffer pair starting at guest address bufL (n samples).
// Returns false (and zeros) when the DSP has not mixed that buffer.
bool dspSideData(u32 bufL, SideSample* out, u32 n);

// ---- ARAM (hle_ar.cpp) ----
// Host view of `len` bytes of ARAM at `addr`, or nullptr when out of range.
const u8* aramRead(u32 addr, u32 len);

// ---- Output (audio_out.cpp) ----
// Starts the host device (once). `rate` is the AI DSP sample rate in Hz.
void outStart(double rate);
void outSetRate(double rate);
// Jac_imixcopy interleaved the DSP output buffer pair at bufL into the AI block at `dac`
// (`frames` stereo frames): keeps that buffer's side data for when the block plays.
void outTagBlock(u32 dac, u32 bufL, u32 frames);
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

// ---- Music / sound-effect split for the volume sliders (hle_audio.cpp) ----
// Everything JAudio plays from ARAM is a note of some sequence: the music sequences and the
// sound-effect sequence (LuigiSe.bms) alike, so the DSP source type does not tell them apart.
// voiceKind follows DSP voice -> its dspch_ -> the owning logical channel (jc_) -> its channel
// manager (jcs_, inside the sequence track at +0xD8) -> the track's sequence data, and compares
// that with the sound-effect sequence's data. docs/AUDIO.md "Volume sliders".
enum class VoiceKind { Unknown, Music, Effect };
VoiceKind voiceKind(u32 voice);
// Diagnostics (CF_AUDIO_KIND_LOG): the instrument (bank << 8 | program, -1 unknown) of the note on
// a voice, and the sequence track (seqp_) that plays it (0 unknown).
int voiceBank(u32 voice);
u32 voiceSequence(u32 voice);
// Jaq_SetSeqData_Limit (direct data): a sequence starts from `data` (`size` bytes).
void noteSequenceData(u32 data, u32 size);

// ---- Game sound mode (hle_audio.cpp) ----
// The game's sound mode as last applied through fn_8018A70C (0 mono, 1 stereo,
// 2 surround, as in its options); -1 before the game set one.
int gameSoundMode();

// Debug logging (CF_AUDIO_LOG=1).
extern bool g_log;

}  // namespace recomp::audio
