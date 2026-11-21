// `qilin info`: the boot file as JSON. Developer tools only (std::format).
#include <format>

#include "bootfile.hpp"

namespace qilin {

std::string to_json(const BootFile& boot) {
    const KnownBuild* b = known_build(boot.sha256);
    std::string s = "{\n";
    s += std::format("  \"sha256\": \"{}\",\n", boot.sha256);
    if (b)
        s += std::format("  \"build\": {{\"title\": \"{}\", \"product\": \"{}\", \"version\": \"{}\"}},\n", b->title, b->product,
                         b->version);
    else
        s += "  \"build\": null,\n";
    s += std::format("  \"size\": {},\n", boot.size());
    s += std::format("  \"base\": \"0x{:08X}\",\n", boot.base());
    s += std::format("  \"end\": \"0x{:08X}\",\n", boot.end());
    s += std::format("  \"entry\": \"0x{:08X}\"\n", boot.entry());
    s += "}\n";
    return s;
}

}  // namespace qilin
