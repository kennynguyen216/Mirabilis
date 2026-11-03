#pragma once

#include <algorithm>
#include <chrono>

namespace mirabilis {

struct FrameTime {
    double wallSeconds{};
    float simulationSeconds{};
};

// The host supplies time; simulation never inherits a long pause or reload stall.
class FrameClock {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void reset(TimePoint now = Clock::now()) { previous_ = now; }

    FrameTime tick(TimePoint now = Clock::now()) {
        const double elapsed = std::max(0.0, std::chrono::duration<double>(now - previous_).count());
        previous_ = now;
        return {elapsed, static_cast<float>(std::min(elapsed, 0.1))};
    }

private:
    TimePoint previous_{Clock::now()};
};

} // namespace mirabilis
