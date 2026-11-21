// qilin_run: the recompiled game, being brought up (it takes the game's name,
// toycommander, with the first player release: docs/naming.md).
//
//   qilin_run <the game's .cue>
//
// Reads the boot file from the player's own disc dump, checks that it is the
// build the program was lifted from, maps guest RAM and runs the start-up.
// Anything the port doesn't handle yet stops it with a report naming what was
// reached and from where (exit code 3).
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "boot.hpp"
#include "chain.hpp"
#include "bootfile.hpp"
#include "gdrom.hpp"
#include "gdrom_bios.hpp"
#include "interrupts.hpp"
#include "memory.hpp"
#include "natives.hpp"
#include "pvr.hpp"
#include "rtc.hpp"
#include "sh4.hpp"
#include "system.hpp"
#include "ta.hpp"

namespace rt = qilin::rt;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: qilin_run <the game's .cue>\n");
        return 2;
    }
    auto disc = qilin::gdrom::Disc::open(argv[1]);
    if (!disc) rt::fatal("%s", disc.error().c_str());
    const auto ip = disc->bootstrap();
    if (!ip) rt::fatal("%s", ip.error().c_str());
    const auto files = disc->files();
    if (!files) rt::fatal("%s", files.error().c_str());
    const qilin::gdrom::File* boot_entry = nullptr;
    for (const auto& f : *files)
        if (f.path == ip->boot_file) boot_entry = &f;
    if (!boot_entry) rt::fatal("the disc has no %s, its boot file", ip->boot_file.c_str());
    const auto bytes = disc->read_file(*boot_entry);
    if (!bytes) rt::fatal("%s", bytes.error().c_str());
    const auto boot_file = qilin::BootFile::parse(*bytes);
    if (!boot_file) rt::fatal("%s", boot_file.error().c_str());
    if (boot_file->sha256 != rt::g_lifted_sha256)
        rt::fatal("%s isn't the boot file this program was built from (SHA-256 %s, built from %s)", ip->boot_file.c_str(), boot_file->sha256.c_str(),
                  rt::g_lifted_sha256);

    rt::GuestMemory memory;
    if (!memory.reserve()) rt::fatal("cannot reserve 4 GB of address space for the guest");
    // RAM, 16 MB, where the game is linked to use it (the cached view, 0x8C000000).
    if (!memory.commit(qilin::kRamBase, qilin::kRamSize)) rt::fatal("cannot commit the guest's RAM");
    rt::g_mem = memory.base();
#ifdef QILIN_CHECKED_MEMORY
    rt::g_regions[0] = {qilin::kRamBase, qilin::kRamBase + qilin::kRamSize};
    rt::g_region_count = 1;
#endif
    rt::host_fp_setup();
    toycommander::provide_natives();
    toycommander::provide_gdrom_bios();
    toycommander::provide_chain();
    toycommander::provide_rtc();
    toycommander::insert_disc(*disc);
    toycommander::map_pvr();
    toycommander::ta::map();
    toycommander::map_sh4_peripherals();
    toycommander::start_interrupts();

    rt::Context c;
    // QILIN_TIMEOUT=<seconds>: a run that hasn't stopped by then is stuck in
    // a wait (on hardware the port doesn't provide yet, as a rule): report
    // where the guest is and end with exit code 4. Diagnostic only: it reads
    // the running guest's state without stopping it.
    if (const char* t = std::getenv("QILIN_TIMEOUT")) {
        const int seconds = std::atoi(t);
        std::thread([&c, seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
#ifdef QILIN_CHECKED_MEMORY
            const uint32_t pc = rt::g_pc;
#else
            const uint32_t pc = 0;
#endif
            std::fprintf(stderr, "qilin_run: still running after %d s, at %08X (in fn_%08X)\n", seconds, pc, rt::function_containing(pc));
            if (c.r[15] >= 0x8C00C000u && c.r[15] < 0x8C00F400u) rt::backtrace(stderr, c, 0x8C00F400u);
            std::fflush(stderr);
            std::_Exit(4);
        }).detach();
    }
    try {
        toycommander::boot(c, boot_file->image());
    } catch (const rt::Stop& s) {
        rt::report(stderr, s, &c);
        if (c.r[15] >= 0x8C00C000u && c.r[15] < 0x8C00F400u) {
            rt::backtrace(stderr, c, 0x8C00F400u);
            if (std::getenv("QILIN_STACK"))
                for (uint32_t a = c.r[15]; a < 0x8C00F400u; a += 4) std::fprintf(stderr, "    %08X: %08X\n", a, rt::ld32(a));
        }
        return 3;
    }
    std::fprintf(stderr, "qilin_run: the game returned from main\n");
    return 0;
}
