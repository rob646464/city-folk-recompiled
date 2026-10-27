#pragma once
// Decouples the game's fixed 30 Hz simulation from the presentation rate.
// Each host frame calls advance(); the game then runs `numSimTicks` simulation
// ticks and renders once, interpolating by sample_interpolation_step().
#include <cstdint>

namespace port::game_clock {

constexpr float kSimPeriod = 1.0f / 30.0f;

struct FrameTiming {
    float dt;                   // host seconds since last advance()
    bool interpolating;         // presentation is interpolated between sim ticks
    bool separatePresentation;  // sim and presentation run independently
    int numSimTicks;            // sim ticks to run this host frame
    uint64_t presentationEpoch; // bumps when interpolation history must reset
};
extern FrameTiming g_frameTiming;

void initialize();
void reset();
const FrameTiming& advance();
// 0..1 position of the current presentation frame between the previous and current sim tick.
float sample_interpolation_step();

}  // namespace port::game_clock
