#include "qilin/host/image.hpp"

#include <SDL3/SDL.h>

#include <cstring>

namespace qilin::host {

namespace {

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

bool save_png(const std::filesystem::path& file, uint32_t width, uint32_t height, const uint32_t* rgba, std::string& error) {
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return error = "an empty or too large picture", false;
    SDL_Surface* s = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32, const_cast<uint32_t*>(rgba),
                                           static_cast<int>(width * 4));
    if (!s) return error = SDL_GetError(), false;
    const bool ok = SDL_SavePNG(s, utf8(file).c_str());
    if (!ok) error = SDL_GetError();
    SDL_DestroySurface(s);
    return ok;
}

bool load_png(const std::filesystem::path& file, uint32_t& width, uint32_t& height, std::vector<uint32_t>& rgba, std::string& error) {
    SDL_Surface* loaded = SDL_LoadPNG(utf8(file).c_str());
    if (!loaded) return error = SDL_GetError(), false;
    SDL_Surface* s = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!s) return error = SDL_GetError(), false;
    bool ok = s->w > 0 && s->h > 0 && s->w <= 16384 && s->h <= 16384;
    if (ok) {
        width = static_cast<uint32_t>(s->w), height = static_cast<uint32_t>(s->h);
        rgba.resize(size_t{width} * height);
        const auto* row = static_cast<const uint8_t*>(s->pixels);
        for (uint32_t y = 0; y < height; ++y) std::memcpy(rgba.data() + size_t{y} * width, row + size_t{y} * static_cast<size_t>(s->pitch), size_t{width} * 4);
    } else {
        error = "an empty or too large picture (at most 16384 pixels each way)";
    }
    SDL_DestroySurface(s);
    return ok;
}

}  // namespace qilin::host
