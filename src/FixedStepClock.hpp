#pragma once

#include <algorithm>

class FixedStepClock {
public:
    int Advance(double elapsed, double fixed_dt) {
        // Bound catch-up work after a stall instead of accumulating input latency.
        accumulated_ = std::min(accumulated_ + std::max(0.0, elapsed), fixed_dt * 4.0);
        const int steps = static_cast<int>(accumulated_ / fixed_dt);
        accumulated_ -= steps * fixed_dt;
        return steps;
    }

private:
    double accumulated_ = 0.0;
};
