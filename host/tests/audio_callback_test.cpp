// Actual SDL callbacks run concurrently with producers; ctest bounds hangs.
#include "qilin/host/audio.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
int main() {
    using qilin::host::AudioOut;
    // A missing backend must not terminate the game. Real SDL initialisation
    // fails, samples are dropped, then the same object recovers on a retry.
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "qilin-invalid-audio-driver", SDL_HINT_OVERRIDE);
    {
        AudioOut out;
        std::vector<int16_t> silence(4096 * 2, 0);
        out.set_volume(25, true);  // retained during the outage
        out.push(silence.data(), 1600, 32000);
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
        SDL_Delay(1100);
        out.push(silence.data(), 4096, 32000);
        SDL_Delay(100);
        const auto stats = out.take_stats();
        if (stats.open_failures == 0 || stats.recoveries != 1 || stats.outage_ms < 49 || stats.callbacks == 0) {
            std::printf("FAIL outage failures=%u recoveries=%u lost_ms=%.1f callbacks=%u\n",
                        stats.open_failures, stats.recoveries, stats.outage_ms, stats.callbacks);
            return 1;
        }
    }
    if (SDL_WasInit(SDL_INIT_AUDIO)) { std::puts("FAIL leaked audio subsystem after recovery"); return 1; }
    // Do not release another owner's reference when this AudioOut closes.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 1;
    { AudioOut out; }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) { std::puts("FAIL lost another owner's audio subsystem"); return 1; }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    for (int cycle = 0; cycle < 3; ++cycle) {
        AudioOut out;
        std::vector<int16_t> samples(8192 * 2, 1000);
        std::thread producer([&] {
            for (int i = 0; i < 300; ++i) {
                const unsigned rate = i % 2 ? 48000u : 32000u;
                out.push(samples.data(), 4096, rate);
                SDL_Delay(1);
            }
        });
        std::thread second([&] { for (int i = 0; i < 100; ++i) { out.push(samples.data(), 160, 32000); SDL_Delay(2); } });
        for (unsigned i = 0; i < 200; ++i) { out.set_volume(i % 101, i % 3 == 0); SDL_Delay(1); }
        producer.join(); second.join();
        out.push(samples.data(), 8192, 32000);
        SDL_Delay(60);
        const auto stats = out.take_stats();
        if (stats.callbacks == 0 || stats.dropped == 0 || stats.pushes != 401 || stats.ratio < .995 || stats.ratio > 1.005) {
            std::printf("FAIL callbacks=%u drops=%u pushes=%u ratio=%f\n", stats.callbacks, stats.dropped, stats.pushes, stats.ratio);
            return 1;
        }
        out.push(nullptr, 0, 0);
    }
    std::puts("outage recovery, subsystem ownership, concurrent callbacks, formats, volume and shutdown: passed");
    return 0;
}
