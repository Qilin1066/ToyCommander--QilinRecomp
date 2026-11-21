// FrameTimes (averages, lows, the rolling window) and the sound queue's steering.
#include "qilin/host/frame_times.hpp"
#include "qilin/host/audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using qilin::host::FrameTimes;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static bool near(double a, double b) { return std::fabs(a - b) < 1e-3; }

int main() {
    FrameTimes t;
    check(t.summary().frames == 0 && t.summary().fps == 0, "empty");

    // 990 frames at 16 ms and 10 at 50 ms: the 1% low is exactly the 50 ms frames.
    for (int i = 0; i < 990; ++i) t.add(16.0);
    for (int i = 0; i < 10; ++i) t.add(50.0);
    auto s = t.summary();
    check(s.frames == 1000, "count");
    check(near(s.average_ms, (990 * 16.0 + 500.0) / 1000.0), "average");
    check(near(s.low1_fps, 20.0), "1% low");
    check(near(s.low01_fps, 20.0), "0.1% low");
    check(near(s.worst_ms, 50.0), "worst");

    // The window rolls: after 1200 more even frames the slow ones are gone.
    for (size_t i = 0; i < FrameTimes::kWindow; ++i) t.add(10.0);
    s = t.summary();
    check(s.frames == FrameTimes::kWindow, "window size");
    check(near(s.fps, 100.0) && near(s.low01_fps, 100.0) && near(s.worst_ms, 10.0), "rolled out");

    // One bad frame in a few: the 0.1% low takes at least one frame.
    t.clear();
    for (int i = 0; i < 9; ++i) t.add(10.0);
    t.add(40.0);
    s = t.summary();
    check(near(s.low01_fps, 25.0) && near(s.low1_fps, 25.0), "small sample lows");

    const auto r = t.recent(3);
    check(r.size() == 3 && r[0] == 10.0f && r[2] == 40.0f, "recent, oldest first");
    check(t.recent(100).size() == 10, "recent clamps");

    // The sound queue's steering: on target plays at 1x; over plays faster,
    // under slower; never beyond 0.5%.
    using qilin::host::audio_rate_ratio;
    check(near(audio_rate_ratio(40, 40), 1.0), "sound on target");
    check(audio_rate_ratio(80, 40) > 1.0 && near(audio_rate_ratio(80, 40), 1.004), "sound over target");
    check(audio_rate_ratio(0, 40) < 1.0 && near(audio_rate_ratio(0, 40), 0.996), "sound under target");
    check(near(audio_rate_ratio(1000, 40), 1.005) && near(audio_rate_ratio(-1000, 40), 0.995), "sound speed bounded");

    if (failures) return EXIT_FAILURE;
    std::puts("frame_times_test: ok");
    return EXIT_SUCCESS;
}
