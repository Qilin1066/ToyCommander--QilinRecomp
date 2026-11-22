// Sound out: interleaved 16-bit stereo through an SDL audio stream on the
// default playback device. push() may be called from any thread.
//
// The sound waits in a queue of our own (AudioQueue), which the device's
// thread empties as it plays (SDL asks for what it needs: the stream itself
// holds next to nothing), so what is queued is known exactly, and it stays
// bounded whatever happens on either side:
// - The guest makes sound by the host's clock and the sound card plays it by
//   its own crystal; the two differ by up to a few hundred parts per million,
//   so a plain queue slowly fills (latency creeps up) or drains (gaps).
//   push() keeps it near the target by playing up to 0.5% faster or slower (a
//   pitch change far below hearing), steered by the queue's smoothed length.
// - Whenever one side stops for a moment (the game for a long disc read, the
//   sound device for a dropout under load) the queue is left longer by that
//   much once both run again, and at 0.5% that takes minutes to play off: heard
//   as sound that lags the picture. Past the cap, the oldest sound is dropped
//   down to the target: the time is already lost, and the lag isn't kept.
// - Run dry, it plays nothing until the target's worth has gathered again,
//   one gap rather than many small ones.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace qilin::host {

// The queue's rules, on their own (no device): frames are left/right pairs.
class AudioQueue {
public:
    AudioQueue(size_t target_frames, size_t cap_frames) { set_limits(target_frames, cap_frames); }

    size_t size() const { return (data_.size() - head_) / 2; }
    bool playing() const { return playing_; }
    void set_limits(size_t target_frames, size_t cap_frames) { target_ = std::max(size_t{1}, target_frames), cap_ = std::max(cap_frames, target_); }

    // Add sound. Over the cap, the oldest goes, down to the target: returns how many frames that was.
    size_t push(const int16_t* lr, size_t frames) {
        if (!frames) return 0;
        const size_t before = size();
        size_t dropped = 0;
        if (frames > cap_ || before > cap_ - frames) {
            dropped = before + frames - target_;
            if (frames >= target_) {
                // A large batch never allocates more than the retained target.
                data_.assign(lr + (frames - target_) * 2, lr + frames * 2);
                head_ = 0;
                return dropped;
            }
            head_ += dropped * 2;
        }
        if (head_ > 0 && (head_ * 2 >= data_.size() || data_.size() + frames * 2 > cap_ * 2)) {
            data_.erase(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(head_));
            head_ = 0;
        }
        data_.insert(data_.end(), lr, lr + frames * 2);
        return dropped;
    }

    // Take up to `frames` for the device: none until the target's worth is there (at the start, and
    // after running dry). Returns how many were written to `out`; `ran_dry` is set when the device
    // wanted more than there was while playing.
    size_t pull(int16_t* out, size_t frames, bool& ran_dry) {
        ran_dry = false;
        if (!frames) return 0;
        if (!playing_) {
            if (size() < target_) return 0;
            playing_ = true;
        }
        const size_t n = std::min(frames, size());
        if (n) std::memcpy(out, data_.data() + head_, n * 2 * sizeof(int16_t));
        head_ += n * 2;
        if (n < frames) playing_ = false, ran_dry = true;
        return n;
    }

    void clear() { data_.clear(), head_ = 0, playing_ = false; }

private:
    std::vector<int16_t> data_;
    size_t head_ = 0;  // index of the oldest sample
    size_t target_, cap_;
    bool playing_ = false;
};

class AudioOut {
public:
    // Queued sound aimed for, the latency it adds: 40 ms, or more where the device takes its
    // sound in pieces too large for that (twice a piece and 10 ms). The cap is twice the target.
    static constexpr double kTargetMs = 40.0;

    AudioOut();  // fails loudly if there's no playback device
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Queue `frames` left/right pairs at `rate` Hz.
    void push(const int16_t* left_right, size_t frames, unsigned rate);

    void set_volume(unsigned percent, bool muted = false);

    double target_ms() const { return target_ms_; }

    // Since the last call (any thread): how much was queued on average and at
    // most, the playback speed, how often the queue ran dry, and how often and
    // how much of it was dropped for being over the cap.
    struct Stats {
        double average_ms = 0, max_ms = 0, ratio = 1, dropped_ms = 0;
        uint32_t pushes = 0, empty = 0, dropped = 0, callbacks = 0;
    };
    Stats take_stats();

private:
    void feed(SDL_AudioStream* stream, int bytes);

    SDL_AudioStream* stream_ = nullptr;
    unsigned rate_ = 0;
    double target_ms_ = kTargetMs, device_period_ms_ = 0;
    double smoothed_ms_ = kTargetMs, ratio_ = 1.0;  // the pushing thread's own
    // SDL stream lock protects the queue, format and resampler state, including callbacks.
    float gain_ = 1.0f;
    AudioQueue queue_{0, 0};
    std::vector<int16_t> feeding_;  // the device thread's own
    std::atomic<uint64_t> sum_us_{0}, dropped_us_{0};
    std::atomic<uint32_t> max_us_{0}, pushes_{0}, empty_{0}, dropped_{0}, ratio_ppm_{1000000}, callbacks_{0};
};

// The playback speed for a queue of `smoothed_ms` against `target_ms`: up to
// 0.5% faster when over, slower when under; 0.4% per target's worth of error,
// so 40 ms too much drains in about 10 s.
inline double audio_rate_ratio(double smoothed_ms, double target_ms) {
    const double err = (smoothed_ms - target_ms) / target_ms;
    return 1.0 + std::clamp(0.004 * err, -0.005, 0.005);
}

}  // namespace qilin::host
