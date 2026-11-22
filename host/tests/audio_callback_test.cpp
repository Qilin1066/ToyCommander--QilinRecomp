// Actual SDL callbacks run concurrently with producers; ctest bounds hangs.
#include "qilin/host/audio.hpp"
#include <SDL3/SDL.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
int main() {
    using qilin::host::AudioOut;
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
    std::puts("concurrent callbacks, format changes, volume and shutdown: passed");
    return 0;
}
