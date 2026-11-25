// GD-ROM image reader: a Redump-style dump, one .cue and a .bin per track.
//
// A GD-ROM has two areas. The single-density area (tracks 1 and 2) is an
// ordinary CD session any drive reads; the game is in the high-density area,
// which starts at sector 45000 with track 3. Data tracks are Mode 1 with raw
// 2352-byte sectors (2048 bytes of data each); the other tracks are audio,
// which this game plays as its music.
//
// The high-density area holds the bootstrap ("IP.BIN": the first 16 sectors
// of track 3, with the disc's name, version and boot file name) and an
// ISO 9660 file system whose sector numbers count from the start of the disc.
//
// Everything malformed or unsupported comes back as a message; nothing is
// guessed. Runtime code: no std::format (see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace qilin::gdrom {

inline constexpr uint32_t kRawSector = 2352;
inline constexpr uint32_t kSectorData = 2048;
inline constexpr uint32_t kHighDensityStart = 45000;  // first sector of track 3

struct Track {
    unsigned number = 0;
    bool audio = false;
    bool high_density = false;
    std::filesystem::path file;
    uint32_t file_start = 0;  // disc sector of the file's first sector (its pregap, if it has one)
    uint32_t start = 0;       // disc sector of the track's index 1
    uint32_t sectors = 0;     // in the file, pregap included
    uint32_t end() const { return file_start + sectors; }
};

// The bootstrap's text fields, with trailing spaces removed.
struct Bootstrap {
    std::string hardware, maker, device, areas, peripherals, product, version, date, boot_file, company, title;
};

struct File {
    std::string path;  // "SCENE/SPACE.GHQ": '/' separators, the ";1" version removed
    uint32_t sector = 0;
    uint32_t size = 0;
};

class Disc {
public:
    // Opens the .cue and checks every track file's size; reads no track data.
    static std::expected<Disc, std::string> open(const std::filesystem::path& cue);

    const std::vector<Track>& tracks() const { return tracks_; }

    // One data sector's 2048 bytes; an error for an audio or missing sector,
    // or one whose sync pattern or mode is wrong.
    std::expected<std::vector<uint8_t>, std::string> read_sector(uint32_t sector) const;
    std::expected<std::vector<uint8_t>, std::string> read(uint32_t sector, uint64_t bytes) const;
    // `count` sectors of an audio track from `sector`: 588 frames each of
    // 16-bit left and right samples (interleaved, CD audio's 44.1 kHz). An
    // error for a sector outside an audio track.
    std::expected<std::vector<int16_t>, std::string> read_audio(uint32_t sector, uint32_t count) const;

    std::expected<Bootstrap, std::string> bootstrap() const;
    // Every file of the high-density area's file system, sorted by path.
    std::expected<std::vector<File>, std::string> files() const;
    std::expected<std::vector<uint8_t>, std::string> read_file(const File& f) const { return read(f.sector, f.size); }

private:
    std::vector<Track> tracks_;
};

// Reads audio sectors as read_audio does, keeping the track's file open
// between reads: a stream read on from where it is doesn't open or seek
// again. One reader is used by one thread at a time; the disc must outlive it.
class AudioReader {
public:
    explicit AudioReader(const Disc& disc) : disc_(&disc) {}
    // `count` sectors from `sector` into `out` (replacing what it held).
    std::expected<void, std::string> read(uint32_t sector, uint32_t count, std::vector<int16_t>& out);

private:
    const Disc* disc_;
    const Track* track_ = nullptr;  // the file open, if any
    std::ifstream file_;
    uint32_t next_ = 0;  // the sector the file is at
};

// Parses a bootstrap sector's first 256 bytes.
std::expected<Bootstrap, std::string> parse_bootstrap(const std::vector<uint8_t>& ip);

}  // namespace qilin::gdrom
