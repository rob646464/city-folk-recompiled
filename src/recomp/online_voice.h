#pragma once
// Wii Speak voice chat from the PC microphone (docs/ONLINE.md "Wii Speak voice chat").
// The game's PMIC library (Wii Speak driver over /dev/usb/oh0) is answered at its API by
// online_voice.cpp; the game's own VCT code encodes, sends (DWC unreliable over the room),
// receives, mixes and plays the voices and shows who is speaking.

namespace recomp::voice {

// F1 > Online "Voice chat (Wii Speak)" section; true when a setting changed.
bool drawSection();
// Host thread, every frame (online::drawOverlay): push-to-talk state, key capture, the
// capture device's life time, the level meter.
void tick();

}  // namespace recomp::voice
