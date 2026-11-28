// `qilin identify` (identify.hpp).
#include "identify.hpp"

#include <system_error>

#include "gdrom.hpp"
#include "path.hpp"
#include "sha256.hpp"

namespace qilin {

namespace fs = std::filesystem;

namespace {

// "V1.022" as "1.022".
std::string plain_version(std::string_view v) {
    if (!v.empty() && (v.front() == 'V' || v.front() == 'v')) v.remove_prefix(1);
    return std::string(v);
}

// "Toy Commander (USA), MK-57020 version 1.022"
std::string describe(const KnownBuild& b) {
    return std::string(b.title) + ", " + std::string(b.product) + " version " + plain_version(b.version);
}

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[u >> 4];
            out += kHex[u & 15];
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

}  // namespace

Identity identify(const fs::path& input, std::string_view want, std::span<const KnownBuild> builds) {
    const KnownBuild* wanted = nullptr;
    if (!want.empty()) {
        for (const KnownBuild& b : builds)
            if (b.title == want) wanted = &b;
        if (!wanted) return answer(2, "This recompiler knows no build called " + std::string(want) + ".");
    }
    const std::string for_text = wanted ? " This download is for " + describe(*wanted) + "." : std::string();
    const std::string shown = utf8_path(input);
    std::error_code ec;
    if (!fs::exists(input, ec)) return answer(2, "Can't find " + shown + ".");
    if (fs::is_directory(input, ec)) return answer(2, shown + " is a folder, not a disc image." + for_text);

    // The boot file's bytes: from the disc (the file its bootstrap names), or
    // the boot file itself.
    std::vector<uint8_t> bytes;
    std::string says;  // what the bootstrap says the disc is
    gdrom::Bootstrap ip;
    if (gdrom::Disc::is_image(input)) {
        auto disc = gdrom::Disc::open(input);
        if (!disc) return answer(2, "Can't read this disc image: " + disc.error() + ".");
        auto boot = disc->bootstrap();
        if (!boot) return answer(3, "This disc image has no Dreamcast bootstrap (" + boot.error() + ")." + for_text);
        ip = std::move(*boot);
        says = (ip.title.empty() ? std::string("a Dreamcast disc") : ip.title) + (ip.product.empty() ? "" : ", product " + ip.product) +
               (ip.version.empty() ? "" : ", version " + plain_version(ip.version));
        const auto files = disc->files();
        if (!files) return answer(2, "Can't read this disc's files: " + files.error() + ".");
        const gdrom::File* file = nullptr;
        for (const gdrom::File& f : *files)
            if (f.path == ip.boot_file) file = &f;
        if (!file) {
            Identity i = answer(3, "This disc (" + says + ") has no " + ip.boot_file + ", the file it starts." + for_text);
            i.title = ip.title;
            i.id = ip.product;
            i.version = plain_version(ip.version);
            return i;
        }
        auto read = disc->read_file(*file);
        if (!read) return answer(2, "Can't read " + ip.boot_file + " from this disc: " + read.error() + ".");
        bytes = std::move(*read);
    } else {
        auto boot = BootFile::load(input);
        if (!boot) return answer(2, "Can't read " + shown + " as a disc image (.cue, .gdi or .chd) or a boot file: " + boot.error() + ".");
        bytes.assign(boot->image().begin(), boot->image().end());
    }

    const std::string sha = sha256_hex(bytes);
    const KnownBuild* build = nullptr;
    for (const KnownBuild& b : builds)
        if (b.sha256 == sha) build = &b;
    if (!build) {
        Identity i = answer(3, says.empty() ? "This boot file isn't a version this download knows (SHA-256 " + sha + ")." + for_text
                                            : "This disc isn't a version this download knows. Its bootstrap says it is " + says + "." + for_text);
        i.title = ip.title;
        i.id = ip.product;
        i.version = plain_version(ip.version);
        return i;
    }
    Identity i = answer(0, "");
    i.title = std::string(build->title);
    i.id = std::string(build->product);
    i.version = plain_version(build->version);
    if (wanted && wanted != build) {
        i.code = 3;
        i.ok = false;
        i.message = "This is " + std::string(build->title) + "." + for_text;
        return i;
    }
    i.message = "This is " + describe(*build) + ": the right disc.";
    return i;
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " + json_string(i.id) +
           ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

}  // namespace qilin
