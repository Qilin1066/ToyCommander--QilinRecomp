#include "qilin/host/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace qilin::host {

namespace {
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
    std::fflush(nullptr);
    std::_Exit(70);
}
// SDL holds this recursive lock while calling feed(). Public methods take
// the lifecycle mutex before this lock; callbacks never take that mutex.
struct StreamLock {
    SDL_AudioStream* stream;
    explicit StreamLock(SDL_AudioStream* s) : stream(s) { if (!SDL_LockAudioStream(stream)) die("sound lock failed"); }
    ~StreamLock() { SDL_UnlockAudioStream(stream); }
};
size_t frames_of(double ms, unsigned rate) { return static_cast<size_t>(ms * double(rate) / 1000.0); }
}  // namespace

AudioOut::AudioOut() {
    rate_ = 32000;
    (void)try_open();
}

void AudioOut::close() {
    // The callback never takes device_mutex_, so joining it here cannot
    // invert the public-method lock order (device mutex -> SDL stream).
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    queue_.clear();
    if (audio_initialized_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    audio_initialized_ = false;
}

void AudioOut::unavailable(const char* reason) {
    if (!warned_) std::fprintf(stderr, "sound unavailable: %s: %s (retrying)\n", reason, SDL_GetError());
    warned_ = true;
    open_failures_.fetch_add(1, std::memory_order_relaxed);
    retry_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
}

void AudioOut::mark_failed(const char* reason) {
    // SDL errors are thread-local: report them on the callback/producer that
    // saw the failure, before the next producer schedules reopening.
    if (!failed_.exchange(true, std::memory_order_acq_rel))
        std::fprintf(stderr, "sound stream failure: %s: %s\n", reason, SDL_GetError());
}

bool AudioOut::try_open() {
    if (std::chrono::steady_clock::now() < retry_at_) return false;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { unavailable("SDL audio cannot start"); return false; }
    audio_initialized_ = true;
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate_)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, [](void* self, SDL_AudioStream* stream, int additional, int) {
        static_cast<AudioOut*>(self)->feed(stream, additional);
    }, this);
    if (!stream_) { unavailable("no playback device"); close(); return false; }
    SDL_AudioSpec device{};
    int piece = 0;
    device_period_ms_ = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &piece) && device.freq > 0 && piece > 0)
        device_period_ms_ = double(piece) * 1000.0 / double(device.freq);
    target_ms_ = std::max(kTargetMs, 2.0 * device_period_ms_ + 10.0);
    smoothed_ms_ = target_ms_;
    ratio_ = 1;
    ratio_ppm_.store(1000000, std::memory_order_relaxed);
    queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
    failed_.store(false, std::memory_order_release);
    if (!SDL_SetAudioStreamGain(stream_, gain_) || !SDL_ResumeAudioStreamDevice(stream_)) {
        unavailable("the playback device cannot start"); close(); return false;
    }
    if (paused_) (void)SDL_PauseAudioStreamDevice(stream_);  // opened while paused: stays quiet
    if (warned_) {
        std::fprintf(stderr, "sound output recovered\n");
        recoveries_.fetch_add(1, std::memory_order_relaxed);
        warned_ = false;
    }
    return true;
}

AudioOut::~AudioOut() { const std::lock_guard lock(device_mutex_); close(); }

// The device's thread: SDL needs `bytes` more of the stream's own format.
void AudioOut::feed(SDL_AudioStream* stream, int bytes) {
    if (bytes <= 0) return;
    const size_t frames = (static_cast<size_t>(bytes) + 3) / 4;
    feeding_.resize(frames * 2);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    bool ran_dry;
    const size_t n = queue_.pull(feeding_.data(), frames, ran_dry);
    if (ran_dry) empty_.fetch_add(1, std::memory_order_relaxed);
    if (n && !SDL_PutAudioStreamData(stream, feeding_.data(), static_cast<int>(n * 4))) mark_failed("cannot queue samples");  // (short of it, the device plays silence)
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    if (!frames) return;
    if (!lr || rate == 0 || rate > 192000 || frames > size_t(std::numeric_limits<int>::max()) / 4)
        die("invalid sound buffer or sample rate");
    const std::lock_guard device_lock(device_mutex_);
    if (failed_.exchange(false, std::memory_order_acq_rel)) { unavailable("the audio stream failed"); close(); }
    if (!stream_) {
        rate_ = rate;
        if (!try_open()) {
            outage_us_.fetch_add(static_cast<uint64_t>(double(frames) * 1e6 / double(rate)), std::memory_order_relaxed);
            return;
        }
    }
    const StreamLock stream_lock(stream_);
    size_t before, dropped;
    {
        if (rate != rate_) {  // what is queued was made for the old rate: it goes
            if (!SDL_ClearAudioStream(stream_)) { mark_failed("cannot clear old samples"); return; }
            smoothed_ms_ = target_ms_;
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
            if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) { mark_failed("cannot change format"); return; }
            rate_ = rate;
            queue_.clear();
            queue_.set_limits(frames_of(target_ms_, rate_), frames_of(2.0 * target_ms_, rate_));
        }
        before = queue_.size();
        dropped = queue_.push(lr, frames);
    }
    if (dropped) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        dropped_us_.fetch_add(static_cast<uint64_t>(double(dropped) * 1e6 / double(rate)), std::memory_order_relaxed);
        smoothed_ms_ = target_ms_;
    }
    const double ms = double(before) * 1000.0 / double(rate);
    // Smoothed over about 50 pushes (a quarter second at 5 ms each): the drift is slow.
    smoothed_ms_ += ((dropped ? target_ms_ : ms) - smoothed_ms_) * 0.02;
    const double ratio = audio_rate_ratio(smoothed_ms_, target_ms_);
    if (std::fabs(ratio - ratio_) >= 0.0001) {  // SDL rebuilds its resampler: not for every push
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) { mark_failed("cannot change rate correction"); return; }
        ratio_ = ratio;
        ratio_ppm_.store(static_cast<uint32_t>(std::lround(ratio * 1e6)), std::memory_order_relaxed);
    }
    const auto us = static_cast<uint32_t>(ms * 1000.0);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    pushes_.fetch_add(1, std::memory_order_relaxed);
    uint32_t m = max_us_.load(std::memory_order_relaxed);
    while (us > m && !max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
}

void AudioOut::set_volume(unsigned percent, bool muted) {
    const std::lock_guard device_lock(device_mutex_);
    const float gain = muted ? 0.0f : float(std::min(percent, 100u)) / 100.0f;
    if (gain == gain_) return;
    gain_ = gain;  // retained while unavailable, and reapplied before resume
    if (!stream_) return;
    const StreamLock stream_lock(stream_);
    if (!SDL_SetAudioStreamGain(stream_, gain)) mark_failed("cannot change volume");
}

void AudioOut::set_paused(bool paused) {
    const std::lock_guard device_lock(device_mutex_);
    if (paused == paused_) return;
    paused_ = paused;  // kept for a device opened again meanwhile (try_open)
    if (!stream_) return;
    if (!(paused ? SDL_PauseAudioStreamDevice(stream_) : SDL_ResumeAudioStreamDevice(stream_)))
        mark_failed(paused ? "cannot pause the device" : "cannot resume the device");
}

AudioOut::Stats AudioOut::take_stats() {
    Stats s;
    s.open_failures = open_failures_.exchange(0, std::memory_order_relaxed);
    s.recoveries = recoveries_.exchange(0, std::memory_order_relaxed);
    s.outage_ms = double(outage_us_.exchange(0, std::memory_order_relaxed)) / 1000.0;
    s.pushes = pushes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = sum_us_.exchange(0, std::memory_order_relaxed);
    s.average_ms = s.pushes ? double(sum) / double(s.pushes) / 1000.0 : 0;
    s.max_ms = max_us_.exchange(0, std::memory_order_relaxed) / 1000.0;
    s.empty = empty_.exchange(0, std::memory_order_relaxed);
    s.dropped = dropped_.exchange(0, std::memory_order_relaxed);
    s.callbacks = callbacks_.exchange(0, std::memory_order_relaxed);
    s.dropped_ms = double(dropped_us_.exchange(0, std::memory_order_relaxed)) / 1000.0;
    s.ratio = ratio_ppm_.load(std::memory_order_relaxed) / 1e6;
    return s;
}

}  // namespace qilin::host
