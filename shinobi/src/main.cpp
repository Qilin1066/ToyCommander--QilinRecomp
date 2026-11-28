// The game's program (toycommander, toyracer: each game's library builds this
// with its own build.hpp; docs/naming.md). It was qilin_run in the bring-up.
//
//   qilin_run <the game's .cue>
//
// Reads the boot file from the player's own disc dump, checks that it is the
// build the program was lifted from, maps guest RAM and runs the start-up.
// Anything the port doesn't handle yet stops it with a report naming what was
// reached and from where (exit code 3).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "aica.hpp"
#include "boot.hpp"
#include "bootfile.hpp"
#include "chain.hpp"
#include "gdrom.hpp"
#include "gdrom_bios.hpp"
#include "interrupts.hpp"
#include "library.hpp"
#include "memory.hpp"
#include "build.hpp"
#include "pvr.hpp"
#include "rtc.hpp"
#include "sh4.hpp"
#include "sound.hpp"
#include "system.hpp"
#include "ta.hpp"
#include "threads.hpp"
#include "save_history.hpp"
#include "command_line.hpp"
#include "video.hpp"

#if defined(QILIN_IOS)
// iPadOS and iOS: SDL's own main starts UIKit and calls this file's main
// (renamed SDL_main) on the main thread.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <exception>
#include <filesystem>
#include "path.hpp"
#include "qilin/host/ios.hpp"
#endif

namespace rt = qilin::rt;

namespace {

int run(int argc, char** argv) {
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count(); argv = command_line.values();
    if (argc != 2) {
        std::fprintf(stderr, "usage: " QILIN_APP " <the game's disc image: .cue, .gdi or .chd>\n");
        return 2;
    }
    dreamcast::set_command_line(argc, argv);  // for the menu's "Restore and restart"
    auto disc = qilin::gdrom::Disc::open(qilin::native_path(argv[1]));
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
    shinobi::provide_game_natives();
    shinobi::provide_library();
    shinobi::provide_gdrom_bios();
    shinobi::provide_chain();
    shinobi::provide_rtc();
    shinobi::provide_threads();
    shinobi::insert_disc(*disc);
    dreamcast::map_pvr();
    dreamcast::ta::map();
    dreamcast::map_aica(*disc);
    dreamcast::map_sh4_peripherals();
    dreamcast::video::start();
    dreamcast::sound::start();
    dreamcast::start_interrupts();

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
            // Where the time goes: the functions the guest is in, sampled
            // every 100 microseconds for a second (lifted code's own pc).
            std::map<uint32_t, unsigned> where;
            for (int k = 0; k < 10000; ++k) {
                ++where[rt::function_containing(rt::g_pc)];
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            std::vector<std::pair<unsigned, uint32_t>> top;
            for (const auto& [fn, n] : where) top.emplace_back(n, fn);
            std::ranges::sort(top, std::greater{});
            std::fprintf(stderr, "qilin_run: the guest's time over a second, by function:\n");
            for (size_t k = 0; k < top.size() && k < 12; ++k) std::fprintf(stderr, "  %5.1f%%  fn_%08X\n", top[k].first / 100.0, top[k].second);
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
        shinobi::boot(c, boot_file->image());
    } catch (const rt::Stop& s) {
        if (const rt::Context* t = shinobi::thread_stop_context()) {
            std::fprintf(stderr, "(in a guest thread of bu's thread library)\n");
            rt::report(stderr, s, t);
            rt::end(3);
        }
        rt::report(stderr, s, &c);
        if (c.r[15] >= 0x8C00C000u && c.r[15] < 0x8C00F400u) {
            rt::backtrace(stderr, c, 0x8C00F400u);
            // QILIN_RAM_DUMP=<file>: all 16 MB of RAM at the stop, to compare
            // with the console's (tools/dcref's --game mode).
            if (const char* path = std::getenv("QILIN_RAM_DUMP")) {
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(rt::g_mem + qilin::kRamBase, 1, qilin::kRamSize, f);
                    std::fclose(f);
                }
            }
            // QILIN_DUMP=<hex address>,<bytes>: guest memory at the stop.
            if (const char* d = std::getenv("QILIN_DUMP")) {
                char* end = nullptr;
                const uint32_t from = static_cast<uint32_t>(std::strtoul(d, &end, 16));
                const uint32_t count = end && *end == ',' ? static_cast<uint32_t>(std::strtoul(end + 1, nullptr, 0)) : 64u;
                for (uint32_t a = from & ~3u; a < from + count; a += 16) {
                    std::fprintf(stderr, "    %08X:", a);
                    for (uint32_t k = 0; k < 16; k += 4) std::fprintf(stderr, " %08X", rt::ld32(a + k));
                    std::fprintf(stderr, "\n");
                }
            }
            if (std::getenv("QILIN_STACK"))
                for (uint32_t a = c.r[15]; a < 0x8C00F400u; a += 4) std::fprintf(stderr, "    %08X: %08X\n", a, rt::ld32(a));
        }
#if defined(QILIN_IOS)
        qilin::host::ios::show_fatal("The game reached something this port doesn't do yet.");
#endif
        rt::end(3);  // the sound mixer and the display's threads still run: no static destructors
    }
    std::fprintf(stderr, "qilin_run: the game returned from main\n");
    rt::end(0);
}

}  // namespace

#if !defined(QILIN_IOS)
int main(int argc, char** argv) { return run(argc, argv); }
#else
namespace {

// The disc: on iPadOS there's no command line, so it's the one disc image
// (.chd, .cue or .gdi, by its name) in the app's Documents, where the Mac
// app's "Play on iPad" copies it and the Files app shows it (On My iPad, the
// game's name). None, or more than one: a message saying what to do, and
// "Check again". Empty in the self test.
std::string disc_in_documents() {
    const std::filesystem::path folder = qilin::host::ios::documents();
    for (;;) {
        std::vector<std::filesystem::path> found;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = qilin::utf8_path(it->path().filename());
            // (Not "._" files: the Mac's resource forks, made by some copies.)
            if (!name.empty() && name[0] != '.' && it->is_regular_file(ec) && qilin::gdrom::Disc::is_image(it->path())) found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        if (found.size() == 1) return qilin::utf8_path(found[0]);
        std::string text;
        if (found.empty()) {
            text = "No disc image here yet. " QILIN_APP_TITLE " is built from your own disc: put its image in this app's folder, "
                   "as a .chd, or a .cue with its .bin tracks, or a .gdi with its tracks. The Mac app's \"Play on iPad\" copies it "
                   "there; or use the Files app (On My iPad, " QILIN_APP_TITLE "). Then press Check again.";
        } else {
            text = "There's more than one disc image in this app's folder:";
            for (const auto& f : found) text += " " + qilin::utf8_path(f.filename()) + ",";
            text.back() = '.';
            text += " Leave only one there (the Files app: On My iPad, " QILIN_APP_TITLE "), then press Check again.";
        }
        if (qilin::host::ios::self_test()) {
            std::fprintf(stderr, "QILIN_IOS_SELFTEST: no disc: %s\n", text.c_str());
            return {};
        }
        if (!qilin::host::ios::ask(found.empty() ? "No disc" : "More than one disc", text.c_str(), "Check again", "Quit")) rt::end(0);
    }
}

std::string g_program, g_disc;
char* g_args[3];

}  // namespace

int main(int argc, char** argv) {
    qilin::host::ios::start();
    rt::on_fatal(qilin::host::ios::show_fatal);
    g_disc = disc_in_documents();
    if (qilin::host::ios::self_test()) return qilin::host::ios::self_test_display();
    g_program = argc > 0 && argv[0] ? argv[0] : QILIN_APP;
    g_args[0] = g_program.data(), g_args[1] = g_disc.data(), g_args[2] = nullptr;
    // On a stack as large as Windows gives the desktop game (iOS gives the
    // main thread 1 MB).
    return qilin::host::ios::run_on_big_stack([](void*) {
        try {
            return run(2, g_args);
        } catch (const std::exception& e) {  // nothing may be thrown past the stack's switch
            rt::fatal("%s", e.what());
        } catch (...) {
            rt::fatal("an exception nobody caught");
        }
    }, nullptr, 16u << 20);
}
#endif
