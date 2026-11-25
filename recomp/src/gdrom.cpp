#include "gdrom.hpp"
#include "path.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

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

std::expected<Disc, std::string> Disc::open(const std::filesystem::path& cue) {
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

std::expected<std::vector<uint8_t>, std::string> Disc::read_sector(uint32_t sector) const { return read(sector, kSectorData); }

std::expected<std::vector<int16_t>, std::string> Disc::read_audio(uint32_t sector, uint32_t count) const {
    std::vector<int16_t> out;
    AudioReader reader(*this);
    if (auto r = reader.read(sector, count, out); !r) return std::unexpected(r.error());
    return out;
}

std::expected<void, std::string> AudioReader::read(uint32_t sector, uint32_t count, std::vector<int16_t>& out) {
    out.resize(size_t{count} * (kRawSector / 2));
    for (uint32_t n = 0; n < count; ++n, ++sector) {
        if (!track_ || sector < track_->file_start || sector >= track_->end()) {
            track_ = nullptr;
            file_ = std::ifstream();
            const Track* found = nullptr;
            for (const Track& t : disc_->tracks())
                if (t.high_density == (sector >= kHighDensityStart) && sector >= t.file_start && sector < t.end()) found = &t;
            if (!found) return fail("sector " + std::to_string(sector) + " is not on the disc");
            if (!found->audio) return fail("sector " + std::to_string(sector) + " is in data track " + std::to_string(found->number));
            file_.open(found->file, std::ios::binary);
            if (!file_) return fail("cannot open " + qilin::utf8_path(found->file));
            track_ = found, next_ = found->file_start;
        }
        if (sector != next_) file_.seekg(static_cast<std::streamoff>(uint64_t(sector - track_->file_start) * kRawSector)), next_ = sector;
        uint8_t raw[kRawSector];
        file_.read(reinterpret_cast<char*>(raw), kRawSector);
        if (!file_) {
            const std::string why = "cannot read sector " + std::to_string(sector) + " from " + qilin::utf8_path(track_->file.filename());
            track_ = nullptr;  // opened again next time
            return fail(why);
        }
        ++next_;
        int16_t* to = out.data() + size_t{n} * (kRawSector / 2);
        for (uint32_t k = 0; k < kRawSector; k += 2) to[k / 2] = static_cast<int16_t>(raw[k] | raw[k + 1] << 8);
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::string> Disc::read(uint32_t sector, uint64_t bytes) const {
    std::vector<uint8_t> out;
    out.reserve(bytes);
    const Track* track = nullptr;
    std::ifstream f;
    uint8_t raw[kRawSector];
    while (out.size() < bytes) {
        const std::string name = "sector " + std::to_string(sector);
        if (!track || sector < track->file_start || sector >= track->end()) {
            track = nullptr;
            for (const Track& t : tracks_)
                if (t.high_density == (sector >= kHighDensityStart) && sector >= t.file_start && sector < t.end()) track = &t;
            if (!track) return fail(name + " is not on the disc");
            if (track->audio) return fail(name + " is in audio track " + std::to_string(track->number));
            f = std::ifstream(track->file, std::ios::binary);
            if (!f) return fail("cannot open " + qilin::utf8_path(track->file));
            f.seekg(static_cast<std::streamoff>(uint64_t(sector - track->file_start) * kRawSector));
        }
        f.read(reinterpret_cast<char*>(raw), kRawSector);
        if (!f) return fail("cannot read " + name + " from " + qilin::utf8_path(track->file.filename()));
        if (!std::equal(std::begin(kSync), std::end(kSync), raw)) return fail(name + " has no sync pattern: not a raw data sector");
        if (raw[15] != 1) return fail(name + " is mode " + std::to_string(raw[15]) + ", not Mode 1");
        const uint64_t take = std::min<uint64_t>(kSectorData, bytes - out.size());
        out.insert(out.end(), raw + 16, raw + 16 + take);
        ++sector;
    }
    return out;
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
