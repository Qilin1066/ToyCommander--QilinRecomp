#include "gdrom.hpp"
#include "path.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>

#include <libchdr/chd.h>

#include "bootfile.hpp"

namespace qilin::gdrom {

namespace {

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

std::string trimmed(std::string_view s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) return {};
    return std::string(s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1));
}

// "MM:SS:FF" as a sector count (75 frames a second).
std::expected<uint32_t, std::string> parse_msf(std::string_view s) {
    unsigned v[3] = {};
    size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        const size_t colon = i < 2 ? s.find(':', at) : s.size();
        if (colon == std::string_view::npos) return fail("bad time '" + std::string(s) + "'");
        const auto r = std::from_chars(s.data() + at, s.data() + colon, v[i]);
        if (r.ec != std::errc() || r.ptr != s.data() + colon || colon == at) return fail("bad time '" + std::string(s) + "'");
        at = colon + 1;
    }
    if (v[1] >= 60 || v[2] >= 75) return fail("bad time '" + std::string(s) + "'");
    return (v[0] * 60 + v[1]) * 75 + v[2];
}

constexpr uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};

}  // namespace

std::expected<Disc, std::string> Disc::open_cue(const std::filesystem::path& cue) {
    std::ifstream f(cue);
    if (!f) return fail("cannot open " + qilin::utf8_path(cue));
    Disc disc;
    bool high = false;
    std::filesystem::path file;
    bool file_used = true;
    Track* track = nullptr;
    uint32_t index1 = 0;
    bool have_index1 = false;
    std::vector<std::pair<uint32_t, bool>> index;  // per track: index 1 offset, seen
    std::string raw;
    unsigned line_no = 0;
    const auto where = [&] { return qilin::utf8_path(cue.filename()) + " line " + std::to_string(line_no) + ": "; };
    const auto finish_track = [&]() -> std::expected<void, std::string> {
        if (!track) return {};
        if (!have_index1) return fail(qilin::utf8_path(cue.filename()) + ": track " + std::to_string(track->number) + " has no INDEX 01");
        index.emplace_back(index1, true);
        return {};
    };
    while (std::getline(f, raw)) {
        ++line_no;
        const std::string line = trimmed(raw);
        if (line.empty()) continue;
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "REM") {
            if (line == "REM HIGH-DENSITY AREA") high = true;
            else if (line != "REM SINGLE-DENSITY AREA") continue;
        } else if (key == "FILE") {
            const size_t a = line.find('"'), b = line.rfind('"');
            if (a == std::string::npos || b <= a) return fail(where() + "FILE without a quoted name");
            if (trimmed(std::string_view(line).substr(b + 1)) != "BINARY") return fail(where() + "only BINARY files are supported");
            if (!file_used) return fail(where() + "a FILE with no TRACK");
            file = cue.parent_path() / native_path(line.substr(a + 1, b - a - 1));
            file_used = false;
        } else if (key == "TRACK") {
            unsigned number = 0;
            std::string mode;
            words >> number >> mode;
            if (auto r = finish_track(); !r) return std::unexpected(r.error());
            if (file_used) return fail(where() + "more than one TRACK in a FILE is not supported");
            if (number != disc.tracks_.size() + 1) return fail(where() + "tracks are not numbered in order");
            if (mode != "AUDIO" && mode != "MODE1/2352") return fail(where() + "track mode '" + mode + "' is not supported");
            Track t;
            t.number = number;
            t.audio = mode == "AUDIO";
            t.high_density = high;
            t.file = file;
            file_used = true;
            disc.tracks_.push_back(t);
            track = &disc.tracks_.back();
            have_index1 = false;
        } else if (key == "INDEX") {
            unsigned n = 0;
            std::string time;
            words >> n >> time;
            if (!track) return fail(where() + "INDEX before any TRACK");
            const auto sectors = parse_msf(time);
            if (!sectors) return fail(where() + sectors.error());
            if (n == 0 && *sectors != 0) return fail(where() + "INDEX 00 must be at the start of the file");
            if (n == 1) index1 = *sectors, have_index1 = true;
            else if (n != 0) return fail(where() + "INDEX " + std::to_string(n) + " is not supported");
        } else {
            return fail(where() + "'" + key + "' is not supported");
        }
    }
    if (auto r = finish_track(); !r) return std::unexpected(r.error());
    if (!file_used) return fail(qilin::utf8_path(cue.filename()) + ": a FILE with no TRACK");
    if (disc.tracks_.size() < 3 || !disc.tracks_[2].high_density || disc.tracks_[1].high_density)
        return fail(qilin::utf8_path(cue.filename()) + ": not a GD-ROM dump (the high-density area must start at track 3)");

    // Tracks follow one another within an area; the high-density area starts
    // at sector 45000.
    uint32_t next = 0;
    for (size_t i = 0; i < disc.tracks_.size(); ++i) {
        Track& t = disc.tracks_[i];
        if (t.high_density && (i == 0 || !disc.tracks_[i - 1].high_density)) next = kHighDensityStart;
        std::error_code ec;
        const uintmax_t bytes = std::filesystem::file_size(t.file, ec);
        if (ec) return fail("cannot open " + qilin::utf8_path(t.file));
        if (bytes == 0 || bytes % kRawSector) return fail(qilin::utf8_path(t.file.filename()) + ": its size is not a whole number of 2352-byte sectors");
        if (bytes / kRawSector > 600000) return fail(qilin::utf8_path(t.file.filename()) + ": larger than a GD-ROM");
        t.sectors = static_cast<uint32_t>(bytes / kRawSector);
        if (index[i].first >= t.sectors) return fail(qilin::utf8_path(t.file.filename()) + ": INDEX 01 lies beyond the file");
        t.file_start = next;
        t.start = next + index[i].first;
        next = t.end();
    }
    if (disc.tracks_[2].audio || disc.tracks_[2].start != kHighDensityStart)
        return fail(qilin::utf8_path(cue.filename()) + ": track 3 must be a data track starting at its first sector");
    return disc;
}

// ---- .gdi and .chd, and where their data tracks lie ----

namespace {

std::string lower_extension(const std::filesystem::path& p) {
    std::string ext = qilin::utf8_path(p.extension());
    for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

std::FILE* open_for_reading(const std::filesystem::path& p) {
#if defined(_WIN32)
    return _wfopen(p.c_str(), L"rb");
#else
    return std::fopen(p.c_str(), "rb");
#endif
}

// A .chd open for reading, with the hunk last read.
struct ChdFile {
    std::FILE* stdio = nullptr;
    chd_file* chd = nullptr;
    uint32_t hunk_bytes = 0, unit_bytes = 0, hunks = 0, frames_per_hunk = 0;
    std::vector<uint8_t> hunk;
    uint32_t hunk_no = UINT32_MAX;

    ChdFile() = default;
    ChdFile(const ChdFile&) = delete;
    ChdFile& operator=(const ChdFile&) = delete;
    ~ChdFile() {
        if (chd) chd_close(chd);
        if (stdio) std::fclose(stdio);  // chd_open_file never closes it
    }

    std::expected<void, std::string> open(const std::filesystem::path& path) {
        const std::string name = qilin::utf8_path(path.filename());
        stdio = open_for_reading(path);
        if (!stdio) return fail("cannot open " + qilin::utf8_path(path));
        if (const chd_error e = chd_open_file(stdio, CHD_OPEN_READ, nullptr, &chd); e != CHDERR_NONE)
            return fail(name + ": not a CHD libchdr can read (" + chd_error_string(e) + ")");
        const chd_header* h = chd_get_header(chd);
        hunk_bytes = h->hunkbytes, unit_bytes = h->unitbytes, hunks = h->totalhunks;
        // A CD's units are frames: a sector's 2352 bytes and 96 of subcode.
        if (unit_bytes != kRawSector + 96 || hunk_bytes == 0 || hunk_bytes % unit_bytes)
            return fail(name + ": not a CD or GD-ROM image (its units are " + std::to_string(unit_bytes) + " bytes, not 2448)");
        frames_per_hunk = hunk_bytes / unit_bytes;
        hunk.resize(hunk_bytes);
        return {};
    }

    // Frame `frame`'s first `bytes` bytes into `out`.
    std::expected<void, std::string> read_frame(uint64_t frame, uint8_t* out, uint32_t bytes) {
        const uint64_t h = frame / frames_per_hunk;
        if (h >= hunks) return fail("frame " + std::to_string(frame) + " lies beyond the CHD's end");
        if (h != hunk_no) {
            hunk_no = UINT32_MAX;
            if (const chd_error e = chd_read(chd, static_cast<uint32_t>(h), hunk.data()); e != CHDERR_NONE)
                return fail("cannot read hunk " + std::to_string(h) + " of the CHD: " + chd_error_string(e));
            hunk_no = static_cast<uint32_t>(h);
        }
        std::memcpy(out, hunk.data() + (frame % frames_per_hunk) * unit_bytes, bytes);
        return {};
    }
};

// A whole number, all of `s`.
std::optional<uint32_t> number(std::string_view s) {
    uint32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

// A .gdi line's fields: words, or a quoted name with spaces.
std::expected<std::vector<std::string>, std::string> gdi_fields(std::string_view line) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        at = line.find_first_not_of(" \t", at);
        if (at == std::string_view::npos) return out;
        if (line[at] == '"') {
            const size_t end = line.find('"', at + 1);
            if (end == std::string_view::npos) return fail("a quoted name with no closing quote");
            out.emplace_back(line.substr(at + 1, end - at - 1));
            at = end + 1;
        } else {
            const size_t end = std::min(line.find_first_of(" \t", at), line.size());
            out.emplace_back(line.substr(at, end - at));
            at = end;
        }
    }
}

// A raw data sector's header address (BCD minutes, seconds, frames from the
// disc's start, 150 sectors before sector 0), if it holds a decodable one.
std::optional<uint32_t> header_address(const uint8_t* raw) {
    if (!std::equal(std::begin(kSync), std::end(kSync), raw) || (raw[15] != 1 && raw[15] != 2)) return std::nullopt;
    const auto bcd = [](uint8_t b) -> std::optional<uint32_t> {
        if ((b >> 4) > 9 || (b & 15) > 9) return std::nullopt;
        return (b >> 4) * 10u + (b & 15);
    };
    const auto m = bcd(raw[12]), s = bcd(raw[13]), f = bcd(raw[14]);
    if (!m || !s || !f || *s >= 60 || *f >= 75) return std::nullopt;
    const uint32_t msf = (*m * 60 + *s) * 75 + *f;
    if (msf < 150) return std::nullopt;
    return msf - 150;
}

}  // namespace

bool Disc::is_image(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    return ext == ".cue" || ext == ".gdi" || ext == ".chd";
}

std::expected<Disc, std::string> Disc::open(const std::filesystem::path& image) {
    const std::string ext = lower_extension(image);
    if (ext == ".cue") return open_cue(image);
    if (ext == ".gdi") return open_gdi(image);
    if (ext == ".chd") return open_chd(image);
    return fail(qilin::utf8_path(image.filename()) + " isn't a disc image: choose the dump's .cue, .gdi or .chd");
}

std::expected<Disc, std::string> Disc::open_gdi(const std::filesystem::path& gdi) {
    const std::string name = qilin::utf8_path(gdi.filename());
    std::ifstream f(gdi);
    if (!f) return fail("cannot open " + qilin::utf8_path(gdi));
    std::string raw;
    unsigned line_no = 0;
    std::optional<uint32_t> count;
    Disc disc;
    while (std::getline(f, raw)) {
        ++line_no;
        const std::string line = trimmed(raw);
        if (line.empty()) continue;
        const std::string where = name + " line " + std::to_string(line_no) + ": ";
        if (!count) {
            count = number(line);
            if (!count || *count < 3 || *count > 99) return fail(where + "the first line must be the number of tracks");
            continue;
        }
        const auto fields = gdi_fields(line);
        if (!fields) return fail(where + fields.error());
        // track, first sector, type (0 audio, 4 data), sector size, file, offset
        if (fields->size() != 6) return fail(where + "a track line has six fields: number, sector, type, sector size, file, offset");
        const auto& v = *fields;
        const auto n = number(v[0]), lba = number(v[1]), type = number(v[2]), size = number(v[3]), offset = number(v[5]);
        if (!n || !lba || !type || !size || !offset) return fail(where + "a field that should be a number isn't");
        if (*n != disc.tracks_.size() + 1) return fail(where + "tracks are not numbered in order");
        if (*type != 0 && *type != 4) return fail(where + "track type " + v[2] + " is not supported (0 is audio, 4 data)");
        if (*size != kRawSector && !(*type == 4 && *size == kSectorData))
            return fail(where + "sectors of " + v[3] + " bytes are not supported (2352, or 2048 for data)");
        if (*offset != 0) return fail(where + "a nonzero offset is not supported");
        Track t;
        t.number = *n;
        t.audio = *type == 0;
        t.high_density = *n >= 3;
        t.file = gdi.parent_path() / native_path(v[4]);
        t.sector_bytes = *size;
        t.file_start = t.start = *lba;
        std::error_code ec;
        const uintmax_t bytes = std::filesystem::file_size(t.file, ec);
        if (ec) return fail("cannot open " + qilin::utf8_path(t.file));
        if (bytes == 0 || bytes % t.sector_bytes)
            return fail(qilin::utf8_path(t.file.filename()) + ": its size is not a whole number of " + std::to_string(t.sector_bytes) + "-byte sectors");
        if (bytes / t.sector_bytes > 600000) return fail(qilin::utf8_path(t.file.filename()) + ": larger than a GD-ROM");
        t.sectors = static_cast<uint32_t>(bytes / t.sector_bytes);
        disc.tracks_.push_back(t);
    }
    if (!count) return fail(name + ": empty");
    if (disc.tracks_.size() != *count)
        return fail(name + ": says " + std::to_string(*count) + " tracks but lists " + std::to_string(disc.tracks_.size()));
    if (auto r = disc.place_data_tracks(name, true); !r) return std::unexpected(r.error());
    if (auto r = disc.check_layout(name); !r) return std::unexpected(r.error());
    return disc;
}

std::expected<Disc, std::string> Disc::open_chd(const std::filesystem::path& path) {
    const std::string name = qilin::utf8_path(path.filename());
    ChdFile chd;
    if (auto r = chd.open(path); !r) return std::unexpected(r.error());
    Disc disc;
    disc.chd_ = path;
    uint64_t frame = 0;   // in the CHD: each track's frames, padded to a multiple of 4
    uint32_t next = 0;    // on the disc
    for (uint32_t i = 0;; ++i) {
        char meta[512] = {};
        uint32_t length = 0;
        if (chd_get_metadata(chd.chd, GDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &length, nullptr, nullptr) != CHDERR_NONE) {
            if (i == 0) {
                const bool cd = chd_get_metadata(chd.chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof meta - 1, &length, nullptr, nullptr) == CHDERR_NONE ||
                                chd_get_metadata(chd.chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof meta - 1, &length, nullptr, nullptr) == CHDERR_NONE;
                return fail(name + (cd ? ": a CD's image, not a GD-ROM's (make it from the .gdi or .cue with chdman createcd)"
                                       : ": no GD-ROM track list (not a disc image?)"));
            }
            break;
        }
        meta[std::min<size_t>(length, sizeof meta - 1)] = 0;
        unsigned n = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
        char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};
        if (std::sscanf(meta, "TRACK:%u TYPE:%31s SUBTYPE:%31s FRAMES:%u PAD:%u PREGAP:%u PGTYPE:%31s PGSUB:%31s POSTGAP:%u", &n, type, subtype,
                        &frames, &pad, &pregap, pgtype, pgsub, &postgap) != 9)
            return fail(name + ": track " + std::to_string(i + 1) + "'s description is malformed");
        if (n != i + 1) return fail(name + ": tracks are not numbered in order");
        Track t;
        t.number = n;
        const std::string_view kind = type;
        if (kind == "AUDIO") t.audio = true, t.swapped = true;  // CD audio is kept big-endian in a CHD
        else if (kind == "MODE1") t.sector_bytes = kSectorData;
        else if (kind != "MODE1_RAW") return fail(name + ": track " + std::to_string(n) + " is " + type + ", which is not supported");
        if (frames == 0 || frames > 600000) return fail(name + ": track " + std::to_string(n) + " has " + std::to_string(frames) + " frames");
        t.high_density = n >= 3;
        if (n == 3) next = kHighDensityStart;
        // A pregap is either in the track's frames ('V' types) or not stored.
        const bool stored = pgtype[0] == 'V';
        if (stored && pregap >= frames) return fail(name + ": track " + std::to_string(n) + "'s pregap is longer than it");
        t.file_start = next + (stored ? 0 : pregap);
        t.start = t.file_start + (stored ? pregap : 0);
        t.sectors = frames;
        t.chd_frame = static_cast<uint32_t>(frame);
        frame += frames + pad;
        if (frame > uint64_t{chd.hunks} * chd.frames_per_hunk) return fail(name + ": its tracks run beyond its data");
        next = t.end() + postgap;
        disc.tracks_.push_back(t);
    }
    if (auto r = disc.place_data_tracks(name, true); !r) return std::unexpected(r.error());
    if (auto r = disc.check_layout(name); !r) return std::unexpected(r.error());
    return disc;
}

// Each raw data track as its sectors' headers place it: the first sector with
// a readable header (within the first 1,000: after any blank pregap) says
// where the file starts. With `move`, the track is moved there (a .gdi, a
// .chd); without, a difference is an error. A track with no readable header
// (cooked sectors, or an address past 99 minutes) stays where it was put.
std::expected<void, std::string> Disc::place_data_tracks(const std::string& image, bool move) {
    for (Track& t : tracks_) {
        if (t.audio || t.sector_bytes != kRawSector) continue;
        SectorStream stream;
        uint8_t raw[kRawSector];
        const uint32_t scan = std::min<uint32_t>(t.sectors, 1000);
        for (uint32_t k = 0; k < scan; ++k) {
            if (auto r = stream.read(*this, t, t.file_start + k, raw); !r) return std::unexpected(r.error());
            const auto address = header_address(raw);
            if (!address) continue;
            if (*address < k) return fail(image + ": track " + std::to_string(t.number) + "'s sector headers are malformed");
            const uint32_t file_start = *address - k;
            if (file_start != t.file_start) {
                if (!move)
                    return fail(image + ": track " + std::to_string(t.number) + " should start at sector " + std::to_string(t.file_start) +
                                ", but its sectors say " + std::to_string(file_start));
                t.start = t.start - t.file_start + file_start;
                t.file_start = file_start;
            }
            break;
        }
    }
    return {};
}

// The tracks in order within each area, none overlapping, the high-density
// area starting with a data track at sector 45000.
std::expected<void, std::string> Disc::check_layout(const std::string& image) const {
    if (tracks_.size() < 3 || !tracks_[2].high_density || tracks_[1].high_density)
        return fail(image + ": not a GD-ROM dump (the high-density area must start at track 3)");
    for (size_t i = 1; i < tracks_.size(); ++i) {
        const Track& a = tracks_[i - 1];
        const Track& b = tracks_[i];
        if (a.high_density == b.high_density && b.file_start < a.end())
            return fail(image + ": tracks " + std::to_string(a.number) + " and " + std::to_string(b.number) + " overlap");
        if (b.high_density && b.end() > 600000) return fail(image + ": track " + std::to_string(b.number) + " runs past a GD-ROM's end");
    }
    if (!tracks_[0].high_density && tracks_[1].end() > kHighDensityStart) return fail(image + ": the single-density area runs into the high-density one");
    if (tracks_[2].audio || tracks_[2].start != kHighDensityStart || tracks_[2].file_start != kHighDensityStart)
        return fail(image + ": track 3 must be a data track starting at sector 45000");
    return {};
}

// ---- Reading sectors ----

struct SectorStream::Chd : ChdFile {};

SectorStream::SectorStream() = default;
SectorStream::~SectorStream() = default;
SectorStream::SectorStream(SectorStream&&) noexcept = default;
SectorStream& SectorStream::operator=(SectorStream&&) noexcept = default;

void SectorStream::close() {
    track_ = nullptr;
    file_ = std::ifstream();
    chd_.reset();
}

std::expected<void, std::string> SectorStream::read(const Disc& disc, const Track& t, uint32_t sector, uint8_t* out) {
    const uint32_t at = sector - t.file_start;  // the caller keeps it within the track
    if (!disc.chd().empty()) {
        if (!chd_) {
            auto chd = std::make_unique<Chd>();
            if (auto r = chd->open(disc.chd()); !r) return std::unexpected(r.error());
            chd_ = std::move(chd);
        }
        if (auto r = chd_->read_frame(uint64_t{t.chd_frame} + at, out, t.sector_bytes); !r)
            return fail("sector " + std::to_string(sector) + ": " + r.error());
        return {};
    }
    if (track_ != &t) {
        track_ = nullptr;
        file_ = std::ifstream();
        file_.open(t.file, std::ios::binary);
        if (!file_) return fail("cannot open " + qilin::utf8_path(t.file));
        track_ = &t, next_ = t.file_start;
    }
    if (sector != next_) file_.seekg(static_cast<std::streamoff>(uint64_t{at} * t.sector_bytes)), next_ = sector;
    file_.read(reinterpret_cast<char*>(out), t.sector_bytes);
    if (!file_) {
        const std::string why = "cannot read sector " + std::to_string(sector) + " from " + qilin::utf8_path(t.file.filename());
        track_ = nullptr;  // opened again next time
        file_ = std::ifstream();
        return fail(why);
    }
    ++next_;
    return {};
}

namespace {

// The track whose stored sectors hold `sector`, if any.
const Track* track_at(const Disc& disc, uint32_t sector) {
    const Track* found = nullptr;
    for (const Track& t : disc.tracks())
        if (t.high_density == (sector >= kHighDensityStart) && sector >= t.file_start && sector < t.end()) found = &t;
    return found;
}

}  // namespace

std::expected<std::vector<uint8_t>, std::string> Disc::read_sector(uint32_t sector) const { return read(sector, kSectorData); }

std::expected<std::vector<int16_t>, std::string> Disc::read_audio(uint32_t sector, uint32_t count) const {
    std::vector<int16_t> out;
    AudioReader reader(*this);
    if (auto r = reader.read(sector, count, out); !r) return std::unexpected(r.error());
    return out;
}

std::expected<void, std::string> AudioReader::read(uint32_t sector, uint32_t count, std::vector<int16_t>& out) {
    out.resize(size_t{count} * (kRawSector / 2));
    uint8_t raw[kRawSector];
    for (uint32_t n = 0; n < count; ++n, ++sector) {
        if (!track_ || sector < track_->file_start || sector >= track_->end()) {
            track_ = track_at(*disc_, sector);
            if (!track_) return fail("sector " + std::to_string(sector) + " is not on the disc");
            if (!track_->audio) {
                const unsigned number = track_->number;
                track_ = nullptr;
                return fail("sector " + std::to_string(sector) + " is in data track " + std::to_string(number));
            }
        }
        if (auto r = stream_.read(*disc_, *track_, sector, raw); !r) {
            track_ = nullptr;
            return r;
        }
        int16_t* to = out.data() + size_t{n} * (kRawSector / 2);
        if (track_->swapped)
            for (uint32_t k = 0; k < kRawSector; k += 2) to[k / 2] = static_cast<int16_t>(raw[k] << 8 | raw[k + 1]);
        else
            for (uint32_t k = 0; k < kRawSector; k += 2) to[k / 2] = static_cast<int16_t>(raw[k] | raw[k + 1] << 8);
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::string> Disc::read(uint32_t sector, uint64_t bytes) const {
    const uint64_t sectors = (bytes + kSectorData - 1) / kSectorData;
    if (sectors > 0xFFFFFFFFu) return fail("a read of " + std::to_string(bytes) + " bytes");
    std::vector<uint8_t> out(sectors * kSectorData);
    DataReader reader(*this);
    if (auto r = reader.read(sector, static_cast<uint32_t>(sectors), out.data()); !r) return std::unexpected(r.error());
    out.resize(bytes);
    return out;
}

std::expected<void, std::string> DataReader::read(uint32_t sector, uint32_t count, uint8_t* out) {
    uint8_t raw[kRawSector];
    for (uint32_t n = 0; n < count; ++n, ++sector) {
        const auto name = [&] { return "sector " + std::to_string(sector); };
        if (!track_ || sector < track_->file_start || sector >= track_->end()) {
            track_ = track_at(*disc_, sector);
            if (!track_) return fail(name() + " is not on the disc");
            if (track_->audio) {
                const unsigned number = track_->number;
                track_ = nullptr;
                return fail(name() + " is in audio track " + std::to_string(number));
            }
        }
        uint8_t* to = out + size_t{n} * kSectorData;
        if (track_->sector_bytes == kSectorData) {  // cooked: the data alone
            if (auto r = stream_.read(*disc_, *track_, sector, to); !r) {
                track_ = nullptr;
                return r;
            }
            continue;
        }
        if (auto r = stream_.read(*disc_, *track_, sector, raw); !r) {
            track_ = nullptr;
            return r;
        }
        if (!std::equal(std::begin(kSync), std::end(kSync), raw)) return fail(name() + " has no sync pattern: not a raw data sector");
        if (raw[15] != 1) return fail(name() + " is mode " + std::to_string(raw[15]) + ", not Mode 1");
        std::memcpy(to, raw + 16, kSectorData);
    }
    return {};
}

std::expected<Bootstrap, std::string> parse_bootstrap(const std::vector<uint8_t>& ip) {
    if (ip.size() < 256) return fail("the bootstrap is shorter than its 256-byte header");
    const auto field = [&](size_t at, size_t n) { return trimmed(std::string_view(reinterpret_cast<const char*>(ip.data()) + at, n)); };
    Bootstrap b;
    b.hardware = field(0, 16);
    if (b.hardware != "SEGA SEGAKATANA") return fail("not a Dreamcast bootstrap: it doesn't start with 'SEGA SEGAKATANA'");
    b.maker = field(16, 16);
    b.device = field(32, 16);
    b.areas = field(48, 8);
    b.peripherals = field(56, 8);
    b.product = field(64, 10);
    b.version = field(74, 6);
    b.date = field(80, 16);
    b.boot_file = field(96, 16);
    b.company = field(112, 16);
    b.title = field(128, 128);
    if (b.boot_file.empty()) return fail("the bootstrap names no boot file");
    return b;
}

std::expected<Bootstrap, std::string> Disc::bootstrap() const {
    const auto ip = read_sector(kHighDensityStart);
    if (!ip) return std::unexpected(ip.error());
    return parse_bootstrap(*ip);
}

std::expected<std::vector<File>, std::string> Disc::files() const {
    const auto pvd = read_sector(kHighDensityStart + 16);
    if (!pvd) return std::unexpected(pvd.error());
    if ((*pvd)[0] != 1 || std::string_view(reinterpret_cast<const char*>(pvd->data()) + 1, 5) != "CD001")
        return fail("no ISO 9660 volume descriptor at sector " + std::to_string(kHighDensityStart + 16));
    if (read_le16(pvd->data() + 128) != kSectorData) return fail("the file system's block size is not 2048");

    struct Dir {
        std::string path;
        uint32_t sector, size;
    };
    std::vector<Dir> todo{{"", read_le32(pvd->data() + 156 + 2), read_le32(pvd->data() + 156 + 10)}};
    std::vector<File> out;
    size_t visited = 0;
    while (!todo.empty()) {
        const Dir dir = todo.back();
        todo.pop_back();
        if (++visited > 10000 || dir.size > (16u << 20)) return fail("the file system's directories are malformed");
        const auto data = read(dir.sector, dir.size);
        if (!data) return fail("directory '" + dir.path + "': " + data.error());
        const std::vector<uint8_t>& d = *data;
        for (size_t o = 0; o < d.size();) {
            const size_t len = d[o];
            if (len == 0) {  // records don't cross sectors: the rest of this one is padding
                o = (o / kSectorData + 1) * kSectorData;
                continue;
            }
            if (len < 34 || o + len > d.size() || size_t(33) + d[o + 32] > len)
                return fail("directory '" + dir.path + "' has a malformed record");
            const uint32_t sector = read_le32(&d[o + 2]), size = read_le32(&d[o + 10]);
            const bool is_dir = d[o + 25] & 2;
            if (d[o + 25] & 0x80) return fail("directory '" + dir.path + "': multi-extent files are not supported");
            std::string name(reinterpret_cast<const char*>(&d[o + 33]), d[o + 32]);
            o += len;
            if (name == std::string(1, '\0') || name == "\1") continue;  // this directory, its parent
            if (const size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
            if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
                return fail("directory '" + dir.path + "' has an unusable file name");
            const std::string path = dir.path.empty() ? name : dir.path + "/" + name;
            if (is_dir) todo.push_back({path, sector, size});
            else out.push_back({path, sector, size});
        }
    }
    std::ranges::sort(out, {}, &File::path);
    return out;
}

}  // namespace qilin::gdrom
