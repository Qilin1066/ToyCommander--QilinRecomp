// Build-time generator for the end-to-end lifter tests: lifts the synthetic
// programs of lift_programs.hpp with the real analysis and lifter, into the
// directory given. No game data.
#include <cstdio>
#include <exception>
#include <vector>

#include "cfg.hpp"
#include "lift_output.hpp"
#include "lift_programs.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: qilin_liftgen <out dir>\n", stderr);
        return 2;
    }
    try {
        const qilin::test::Programs p = qilin::test::build();
        std::vector<uint8_t> bytes;
        for (const uint16_t w : p.image) bytes.push_back(static_cast<uint8_t>(w)), bytes.push_back(static_cast<uint8_t>(w >> 8));
        auto boot = qilin::BootFile::parse(bytes);
        if (!boot) {
            std::fprintf(stderr, "qilin_liftgen: %s\n", boot.error().c_str());
            return 1;
        }
        const qilin::Cfg cfg = qilin::analyse(*boot);
        qilin::LiftOptions options;
        options.orphans = true;
        const qilin::ReplacedFunction replaced[] = {{p.entry.at("native_me"), "native_me", "the registry's test"},
                                                    {p.entry.at("native_checked"), "native_checked", "a native footprint's test"},
                                                    {p.entry.at("native_bad"), "native_bad", "a native footprint's test"}};
        options.replaced = replaced;
        // Their footprints: r0 and PR.
        constexpr uint64_t kR0Pr = 1u | 1u << 17;
        const qilin::NativeFootprint natives[] = {{p.entry.at("native_checked"), kR0Pr, "native_checked"}, {p.entry.at("native_bad"), kR0Pr, "native_bad"}};
        options.natives = natives;
        const qilin::LiftSummary sum = qilin::write_lifted(*boot, cfg, {}, argv[1], options, "test");
        std::fputs(sum.report.c_str(), stdout);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "qilin_liftgen: %s\n", e.what());
        return 1;
    }
}
