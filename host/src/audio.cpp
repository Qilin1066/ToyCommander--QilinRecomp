#include "qilin/host/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace qilin::host {

namespace {
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
    std::fflush(nullptr);
    std::_Exit(70);
}
}  // namespace

AudioOut::AudioOut() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) die("SDL audio can't start");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 32000};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) die("no sound output device");
    rate_ = 32000;
    if (!SDL_ResumeAudioStreamDevice(stream_)) die("the sound output device won't start");
}

AudioOut::~AudioOut() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    if (rate != rate_) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) die("the sound rate can't change");
        rate_ = rate;
    }
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) die("the sound queue can't be read");
    double ms = double(queued) / 4.0 * 1000.0 / double(rate);
    if (ms > kDropMs) {
        SDL_ClearAudioStream(stream_);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        ms = 0, smoothed_ms_ = kTargetMs;
    }
    if (queued == 0) {
        // Ran dry (or the start): a cushion of silence first, so the device has
        // the target's worth queued at once rather than gap after small gap.
        empty_.fetch_add(1, std::memory_order_relaxed);
        const double sound_ms = double(frames) * 1000.0 / double(rate);
        if (sound_ms < kTargetMs) {
            silence_.assign(static_cast<size_t>((kTargetMs - sound_ms) * double(rate) / 1000.0) * 2, 0);
            if (!SDL_PutAudioStreamData(stream_, silence_.data(), static_cast<int>(silence_.size() * 2)))
                die("sound can't be queued");
            ms = kTargetMs - sound_ms, smoothed_ms_ = kTargetMs;
        }
    }
    // Smoothed over about 50 pushes (a quarter second at 5 ms each): the drift is slow.
    smoothed_ms_ += (ms - smoothed_ms_) * 0.02;
    const double ratio = audio_rate_ratio(smoothed_ms_, kTargetMs);
    if (std::fabs(ratio - ratio_) >= 0.0001) {  // SDL rebuilds its resampler: not for every push
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) die("the sound speed can't change");
        ratio_ = ratio;
        ratio_ppm_.store(static_cast<uint32_t>(std::lround(ratio * 1e6)), std::memory_order_relaxed);
    }
    const auto us = static_cast<uint32_t>(ms * 1000.0);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    pushes_.fetch_add(1, std::memory_order_relaxed);
    uint32_t m = max_us_.load(std::memory_order_relaxed);
    while (us > m && !max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    if (!SDL_PutAudioStreamData(stream_, lr, static_cast<int>(frames * 4))) die("sound can't be queued");
}

AudioOut::Stats AudioOut::take_stats() {
    Stats s;
    s.pushes = pushes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = sum_us_.exchange(0, std::memory_order_relaxed);
    s.average_ms = s.pushes ? double(sum) / double(s.pushes) / 1000.0 : 0;
    s.max_ms = max_us_.exchange(0, std::memory_order_relaxed) / 1000.0;
    s.empty = empty_.exchange(0, std::memory_order_relaxed);
    s.dropped = dropped_.exchange(0, std::memory_order_relaxed);
    s.ratio = ratio_ppm_.load(std::memory_order_relaxed) / 1e6;
    return s;
}

}  // namespace qilin::host
