#pragma once

#include <algorithm>

class FixedStepClock {
public:
    // Ticks for one rendered frame: the whole ticks that elapsed, at most two, and only one when the measured tick cost
    // (MeasureTicks) says two would not fit in kCatchUpBudget of a tick, leaving the rest of the frame for rendering.
    // Backlog beyond one tick is dropped. So a tick that fits a frame runs once per frame at the display rate instead of
    // occasionally doubling up into a long frame, and a tick that costs more than a frame slows the simulation down
    // (fewer ticks per second of wall time) instead of compounding into ever longer frames.
    int Advance(double elapsed, double fixed_dt) {
        accumulated_ += std::max(0.0, elapsed);
        const int cap = 2.0 * tick_cost_ > kCatchUpBudget * fixed_dt ? 1 : 2;
        const int steps = std::min(cap, static_cast<int>(accumulated_ / fixed_dt));
        accumulated_ = std::min(accumulated_ - steps * fixed_dt, fixed_dt);
        return steps;
    }

    // Wall seconds the last frame's ticks took. The estimate rises at once and decays slowly, so one slow tick stops
    // catch-up immediately and a brief lull does not restart it.
    void MeasureTicks(double seconds, int ticks) {
        if (ticks <= 0 || !(seconds >= 0.0)) return;
        const double per_tick = seconds / ticks;
        tick_cost_ = per_tick > tick_cost_ ? per_tick : tick_cost_ + 0.05 * (per_tick - tick_cost_);
    }

    static constexpr double kCatchUpBudget = 0.75;

private:
    double accumulated_ = 0.0;
    double tick_cost_ = 0.0;
};
