// Frame times: the intervals between frames shown, over a rolling window,
// and what players compare (average fps, 1% and 0.1% lows, the worst frame).
//
// A "low" here is the frame rate of the average of the slowest 1% (or 0.1%)
// of frame times in the window: stricter than the 99th percentile, so one
// bad frame in a thousand shows. Header-only, no locks: one thread adds,
// the same thread asks (the main loop).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace qilin::host {

class FrameTimes {
public:
    static constexpr size_t kWindow = 1200;  // 20 s at 60 fps

    struct Summary {
        size_t frames = 0;      // in the window
        double average_ms = 0;  // mean frame time
        double fps = 0;         // 1000 / average_ms
        double low1_fps = 0;    // 1000 / mean of the slowest 1% of frame times
        double low01_fps = 0;   // the same for the slowest 0.1% (at least one frame)
        double worst_ms = 0;    // the slowest frame
    };

    void add(double ms) {
        ring_[next_] = static_cast<float>(ms);
        next_ = (next_ + 1) % kWindow;
        if (count_ < kWindow) ++count_;
    }
    void clear() { count_ = next_ = 0; }
    size_t size() const { return count_; }

    // The most recent n frame times, oldest first (for a graph).
    std::vector<float> recent(size_t n) const {
        n = std::min(n, count_);
        std::vector<float> out(n);
        for (size_t i = 0; i < n; ++i) out[i] = ring_[(next_ + kWindow - n + i) % kWindow];
        return out;
    }

    Summary summary() const {
        Summary s;
        s.frames = count_;
        if (count_ == 0) return s;
        std::vector<float> t(ring_.begin(), ring_.begin() + count_);
        double sum = 0;
        for (float v : t) sum += v;
        s.average_ms = sum / double(count_);
        s.fps = s.average_ms > 0 ? 1000.0 / s.average_ms : 0;
        std::sort(t.begin(), t.end(), std::greater<float>());
        s.worst_ms = t[0];
        const auto low = [&](size_t per_thousand) {
            const size_t n = std::max<size_t>(1, count_ * per_thousand / 1000);
            double w = 0;
            for (size_t i = 0; i < n; ++i) w += t[i];
            w /= double(n);
            return w > 0 ? 1000.0 / w : 0.0;
        };
        s.low1_fps = low(10);
        s.low01_fps = low(1);
        return s;
    }

private:
    std::array<float, kWindow> ring_{};
    size_t next_ = 0, count_ = 0;
};

}  // namespace qilin::host
