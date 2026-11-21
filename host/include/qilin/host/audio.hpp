// Sound out: interleaved 16-bit stereo through an SDL audio stream on the
// default playback device. push() may be called from any thread.
//
// The guest makes sound by the host's clock and the sound card plays it by its
// own crystal; the two differ by up to a few hundred parts per million, so a
// plain queue slowly fills (latency creeps up) or drains (gaps). push() keeps
// the queue near kTargetMs by playing up to 0.5% faster or slower (a pitch
// change far below hearing), steered by the queue's smoothed length. Only a
// stall that leaves more than kDropMs queued drops sound.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct SDL_AudioStream;

namespace qilin::host {

class AudioOut {
public:
    static constexpr double kTargetMs = 40.0;  // queued sound aimed for: the latency it adds
    static constexpr double kDropMs = 200.0;   // more than this queued (a stall): drop it

    AudioOut();  // fails loudly if there's no playback device
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Queue `frames` left/right pairs at `rate` Hz.
    void push(const int16_t* left_right, size_t frames, unsigned rate);

    // Since the last call (any thread): how much was queued on average and at
    // most, the playback speed, and how often the queue ran dry or was dropped.
    struct Stats {
        double average_ms = 0, max_ms = 0, ratio = 1;
        uint32_t pushes = 0, empty = 0, dropped = 0;
    };
    Stats take_stats();

private:
    SDL_AudioStream* stream_ = nullptr;
    unsigned rate_ = 0;
    double smoothed_ms_ = kTargetMs, ratio_ = 1.0;  // the guest thread's own
    std::vector<int16_t> silence_;
    std::atomic<uint64_t> sum_us_{0};
    std::atomic<uint32_t> max_us_{0}, pushes_{0}, empty_{0}, dropped_{0}, ratio_ppm_{1000000};
};

// The playback speed for a queue of `smoothed_ms` against `target_ms`: up to
// 0.5% faster when over, slower when under; 0.4% per target's worth of error,
// so 40 ms too much drains in about 10 s.
inline double audio_rate_ratio(double smoothed_ms, double target_ms) {
    const double err = (smoothed_ms - target_ms) / target_ms;
    return 1.0 + std::clamp(0.004 * err, -0.005, 0.005);
}

}  // namespace qilin::host
