#pragma once
// Game-specific extension points of the generic runtime.
//
// The runtime (scheduler, HLE SDK, REL linking, frame pacing, audio, GX bridge, menu) is shared
// with the Luigi's Mansion and Super Mario Sunshine ports and knows nothing about City Folk.
// Everything that does lives in src/game/ and reaches the runtime only through these functions
// (LM's skip.h / hidden_mansion.h / coop.h / tags.h hooks map onto them; SMS has the same file).
// Defaults that leave the game untouched are in src/game/cf_game.cpp.
#include "recomp/ctx.h"

struct PADStatus;

namespace recomp::game {

// --- frame loop (guest thread) ---
// Once per presented guest frame, before it is handed to the host. True: the frame is not
// shown and the guest continues straight into the next one (fast-forward).
bool onGuestFrame(Ctx& c);
// Guest frames presented so far, shown or skipped (CF_INPUT_CLOCK=frames times --input).
u32 guestFrames();

// --- input (guest thread, inside PADRead) ---
void filterPad(PADStatus* pads, int count);
// True while the game must not see the player's input (an event holds the player).
bool padHeld();

// --- timing (any thread) ---
// VI retrace / alarm speed multiplier (1 = console speed; > 1 while fast-forwarding).
double retraceSpeed();
// Audio gain multiplier 0..1 (muted while fast-forwarding).
float audioGain();
// Audio channels swapped by the game state (LM: the mirrored Hidden Mansion).
bool audioMirrored();
// The game's own frame rate (LM/SMS 30, City Folk 60): the F1 frame-rate floor ("Original":
// drawn directly); only targets above it are interpolated. Scenes that run slower (a 30 fps
// scene in a 60 fps game) are interpolated by the tapes' time stamps either way.
int nativeFps();

// --- rendering (guest thread, GX HLE) ---
// True while the game renders a pass that must not get the widescreen / FOV projection
// adjustments (e.g. a light's-view shadow map the receivers project with the original matrices).
bool unadjustedPass();
// Two side-by-side views (a split screen): their viewports get the full-screen 3D adjustments.
bool splitScreen();
// A message window is on screen / how text boxes are interpolated (2 blended as one layout,
// 1 locked to the game frame, 0 neither; LM tags.cpp).
bool textBoxShown();
int textMode();
// A host viewer (model viewer) has taken over the frame loop: presentation stays at the
// game's rate.
bool viewerOpen();

// --- enhanced graphics (recomp/enhanced_lighting.h) ---
// World -> view (GX eye space) matrix of the main camera, 3x4 rows. False without a camera.
bool enhViewMatrix(float m[12]);
// The player's position in world space (test lights). False without a player.
bool enhPlayerPos(float p[3]);
// Start of the main 3D scene while enhanced graphics are on: the game sends its own lights.
void enhBeginScene(Ctx& c);
// The room/area box a world position lies in, for limiting lights (false: none).
bool enhRoomBounds(const float p[3], float lo[3], float hi[3]);

// --- host side, inside the Aurora frame ---
void drawOverlay();
// F1 > Enhancements > Debug: the game's debug entries (viewers, warps, ...).
void debugMenu();

// --- disc (any thread): host-served files that replace or add disc files. Entry numbers
// must not collide with the disc's.
s32 overlayEntry(const char* path);  // -1: not overlaid
bool isOverlayEntry(s32 entry);
u32 overlayLength(s32 entry);
s32 overlayRead(s32 entry, void* dst, s32 len, s32 off);

// --- boot (guest thread, after the OS is initialised, before main) ---
void onBoot(Ctx& c);
// Once per guest frame (guest thread), before onGuestFrame.
void onFrame();

}  // namespace recomp::game
