// Fixed-step simulation clock with variable-rate rendering ("fix your timestep").
// Each frame: n = clock.advance(realDelta); run n simulation ticks of clock.step() seconds;
// render with clock.alpha() blending previous/current simulation state.
#pragma once

#include <algorithm>
#include <cstdint>

namespace ou::engine {

class FrameClock {
public:
    explicit FrameClock(double stepSeconds = 1.0 / 60.0, int maxStepsPerFrame = 8)
        : step_(stepSeconds), maxSteps_(maxStepsPerFrame) {}

    // Adds real elapsed time; returns the number of fixed ticks due now. When more than
    // maxStepsPerFrame ticks are due (hitch, debugger pause) the excess time is dropped so
    // the simulation slows down instead of spiralling. Dropping never changes what a tick computes:
    // every tick is the same fixed step fed that tick's input, so dropped time only delays the
    // world against the wall clock (docs/GAMEPLAY.md "Timing").
    int advance(double realDeltaSeconds) {
        accumulator_ += std::max(0.0, realDeltaSeconds);
        // A billionth of a tick of slack: render rates dividing the tick rate (240 Hz = 4 frames a tick) land on
        // the tick instead of a frame late through the rounding of 1/240 + 1/240 + ...
        int steps = static_cast<int>(accumulator_ / step_ + 1e-9);
        if (steps > maxSteps_) {
            steps = maxSteps_;
            accumulator_ = 0.0;
            dropped_ += 1;
        } else {
            accumulator_ = std::max(0.0, accumulator_ - steps * step_);
        }
        ticks_ += static_cast<uint64_t>(steps);
        return steps;
    }

    // Fraction of a tick elapsed since the last simulated state, in [0, 1).
    double alpha() const { return accumulator_ / step_; }
    double step() const { return step_; }
    uint64_t ticks() const { return ticks_; }
    uint64_t droppedFrames() const { return dropped_; }

private:
    double step_;
    int maxSteps_;
    double accumulator_ = 0.0;
    uint64_t ticks_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace ou::engine
