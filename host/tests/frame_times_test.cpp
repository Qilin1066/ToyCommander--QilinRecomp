// FrameTimes (averages, lows, the rolling window) and the sound queue's steering.
#include "qilin/host/frame_times.hpp"
#include "qilin/host/audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

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

    // The sound queue itself (frames of left and right; 40 aimed for, 80 the cap).
    {
        using qilin::host::AudioQueue;
        AudioQueue q(40, 80);
        std::vector<int16_t> in(400), out(400);
        for (size_t i = 0; i < in.size(); ++i) in[i] = static_cast<int16_t>(i);
        bool dry = false;
        // Nothing plays until the target's worth has gathered.
        check(q.push(in.data(), 30) == 0 && q.pull(out.data(), 10, dry) == 0 && !dry && !q.playing(), "sound waits for its cushion");
        check(q.push(in.data() + 60, 10) == 0 && q.pull(out.data(), 10, dry) == 10 && !dry && q.playing(), "sound starts at the target");
        check(out[0] == 0 && out[1] == 1 && out[19] == 19 && q.size() == 30, "sound comes out oldest first");
        // Steady: 5 in, 5 out, nothing lost and nothing dry.
        size_t dropped = 0, dries = 0;
        for (int i = 0; i < 1000; ++i) {
            dropped += q.push(in.data(), 5);
            q.pull(out.data(), 5, dry);
            dries += dry;
        }
        check(dropped == 0 && dries == 0 && q.size() == 30, "a steady sound queue keeps its length");
        // The device stops for a while (a dropout): the queue passes the cap once and comes back
        // to the target, instead of staying that much longer.
        dropped = 0;
        for (int i = 0; i < 20; ++i) dropped += q.push(in.data(), 5);
        check(dropped == 90 && q.size() == 40, "sound over the cap is dropped to the target");
        // The game stops for a while: the queue runs dry once, plays nothing until the cushion is
        // back, and a burst that catches up is cut to the target.
        size_t got = 0;
        dries = 0;
        for (int i = 0; i < 20; ++i) got += q.pull(out.data(), 5, dry), dries += dry;
        check(dries == 1 && !q.playing() && q.size() == 0, "the sound queue runs dry once");
        check(q.push(in.data(), 20) == 0 && q.pull(out.data(), 5, dry) == 0 && !dry, "after a gap, the cushion first");
        dropped = 0;
        for (int i = 0; i < 20; ++i) dropped += q.push(in.data(), 5);
        check(q.size() <= 80 && dropped > 0 && q.pull(out.data(), 5, dry) == 5, "a catching-up burst is cut to the target");
        (void)got;
    }

    if (failures) return EXIT_FAILURE;
    std::puts("frame_times_test: ok");
    return EXIT_SUCCESS;
}
