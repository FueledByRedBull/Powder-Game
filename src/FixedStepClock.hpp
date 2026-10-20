#pragma once

#include <algorithm>

class FixedStepClock {
public:
    // Ticks for one rendered frame. A frame that kept up (at most 1.5 ticks of wall time) may catch up two ticks; after
    // an overrun only one runs. Either way the backlog beyond one tick is dropped, so a tick that costs more than its
    // own duration slows the simulation down instead of compounding into ever longer frames.
    // shortcut: displays below 40 Hz count as overrun and run one tick per frame (slow motion); pass the display
    // interval in if such displays matter.
    int Advance(double elapsed, double fixed_dt) {
        const double frame = std::max(0.0, elapsed);
        accumulated_ += frame;
        const int cap = frame > 1.5 * fixed_dt ? 1 : 2;
        const int steps = std::min(cap, static_cast<int>(accumulated_ / fixed_dt));
        accumulated_ = std::min(accumulated_ - steps * fixed_dt, fixed_dt);
        return steps;
    }

private:
    double accumulated_ = 0.0;
};
