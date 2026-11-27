// GD-ROM image reader: a Redump-style dump (one .cue and a .bin per track),
// a .gdi with its track files, or a .chd (MAME's compressed format, read with
// libchdr).
//
// A GD-ROM has two areas. The single-density area (tracks 1 and 2) is an
// ordinary CD session any drive reads; the game is in the high-density area,
// which starts at sector 45000 with track 3. Data tracks are Mode 1 with raw
// 2352-byte sectors (2048 bytes of data each; a .gdi or .chd may hold only the
// 2048); the other tracks are audio, which this game plays as its music.
//
// A raw data sector's header holds its own address on the disc: a .gdi's and a
// .chd's data tracks are placed by it (their pregaps are kept or dropped
// differently by different tools), and a .cue's are checked against it.
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
#include <memory>
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
    uint32_t sector_bytes = kRawSector;  // as stored: 2352, or 2048 for a cooked data track (.gdi, .chd)
    uint32_t chd_frame = 0;   // .chd: the frame holding file_start
    bool swapped = false;     // audio samples stored big-endian (.chd)
    uint32_t end() const { return file_start + sectors; }
};

class Disc;

// Reads one track's stored sectors, keeping its file (or the .chd's last
// hunk) open between reads. Used by one thread at a time.
class SectorStream {
public:
    SectorStream();
    ~SectorStream();
    SectorStream(SectorStream&&) noexcept;
    SectorStream& operator=(SectorStream&&) noexcept;
    // Sector `sector` of `t` as stored (t.sector_bytes of it) into `out`.
    std::expected<void, std::string> read(const Disc& disc, const Track& t, uint32_t sector, uint8_t* out);
    void close();

private:
    struct Chd;
    const Track* track_ = nullptr;  // the file open, if any
    std::ifstream file_;
    uint32_t next_ = 0;  // the sector the file is at
    std::unique_ptr<Chd> chd_;
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
    // Opens a .cue, .gdi or .chd (by its extension) and checks every track
    // file's size; reads only the data tracks' first sectors.
    static std::expected<Disc, std::string> open(const std::filesystem::path& image);
    // Whether `path` names a kind of image open reads (.cue, .gdi, .chd).
    static bool is_image(const std::filesystem::path& path);

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

    // The .chd, for a disc read from one (else empty).
    const std::filesystem::path& chd() const { return chd_; }

private:
    static std::expected<Disc, std::string> open_cue(const std::filesystem::path& cue);
    static std::expected<Disc, std::string> open_gdi(const std::filesystem::path& gdi);
    static std::expected<Disc, std::string> open_chd(const std::filesystem::path& chd);
    std::expected<void, std::string> place_data_tracks(const std::string& image, bool move);
    std::expected<void, std::string> check_layout(const std::string& image) const;
    std::vector<Track> tracks_;
    std::filesystem::path chd_;
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
    const Track* track_ = nullptr;  // the track last read
    SectorStream stream_;
};

// Reads data sectors as read does, keeping the track's file open between
// reads: a read on from where the last one ended doesn't open or seek
// again. One reader is used by one thread at a time; the disc must outlive it.
class DataReader {
public:
    explicit DataReader(const Disc& disc) : disc_(&disc) {}
    // `count` sectors' 2048 bytes each from `sector` to `out`, which has room
    // for them. On an error, what `out` holds is unspecified.
    std::expected<void, std::string> read(uint32_t sector, uint32_t count, uint8_t* out);

private:
    const Disc* disc_;
    const Track* track_ = nullptr;  // the track last read
    SectorStream stream_;
};

// Parses a bootstrap sector's first 256 bytes.
std::expected<Bootstrap, std::string> parse_bootstrap(const std::vector<uint8_t>& ip);

}  // namespace qilin::gdrom
