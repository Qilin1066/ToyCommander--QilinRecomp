// Identifying the player's disc (disc.hpp).
#include "disc.hpp"

#include <cctype>
#include <system_error>

#include "bootfile.hpp"
#include "gdrom.hpp"
#include "sha256.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "V1.022" as "1.022".
std::string plain_version(std::string_view v) {
    if (!v.empty() && (v.front() == 'V' || v.front() == 'v')) v.remove_prefix(1);
    return std::string(v);
}

}  // namespace

bool disc_image_name(const fs::path& path) {
    // gdrom::Disc::is_image says the same once the disc reader reads all three.
    const std::string ext = lower(path.extension().string());
    return ext == ".cue" || ext == ".gdi" || ext == ".chd";
}

DiscCheck check_disc(const Kit& kit, const fs::path& disc) {
    const std::string wanted = "This download is for " + kit.wanted_disc() + ".";
    std::error_code ec;
    if (!fs::is_regular_file(disc, ec)) return {false, "Can't find " + show(disc) + ". Choose your disc again."};
    if (!disc_image_name(disc))
        return {false, "That's not a disc image. Choose your disc's .cue, .gdi or .chd file (a .cue with its .bin tracks beside it)."};

    auto image = gdrom::Disc::open(disc);
    if (!image) return {false, "Can't read this disc image: " + image.error() + "."};
    const auto ip = image->bootstrap();
    if (!ip) return {false, "This disc image has no Dreamcast bootstrap (" + ip.error() + "). " + wanted};
    const auto files = image->files();
    if (!files) return {false, "Can't read this disc's files: " + files.error() + "."};
    const gdrom::File* boot = nullptr;
    for (const gdrom::File& f : *files)
        if (f.path == ip->boot_file) boot = &f;
    const std::string says = (ip->title.empty() ? std::string("a Dreamcast disc") : ip->title) +
                             (ip->product.empty() ? "" : ", product " + ip->product) +
                             (ip->version.empty() ? "" : ", version " + plain_version(ip->version));
    if (!boot) return {false, "This disc (" + says + ") has no " + ip->boot_file + ", the file it starts. " + wanted};
    const auto bytes = image->read_file(*boot);
    if (!bytes) return {false, "Can't read " + ip->boot_file + " from this disc: " + bytes.error() + "."};

    const KnownBuild* build = known_build(sha256_hex(*bytes));
    if (!build)
        return {false, "This disc isn't a version this download knows. Its bootstrap says it is " + says + ". " + wanted};
    if (build->title != kit.build)
        return {false, "This is " + std::string(build->title) + ". " + wanted};
    return {true, "This is " + std::string(build->title) + ", " + std::string(build->product) + " version " +
                      plain_version(build->version) + ": the right disc."};
}

}  // namespace qilin::launcher
