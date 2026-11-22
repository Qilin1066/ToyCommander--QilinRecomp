// The saves' history (dreamcast/src/save_history.cpp): copies kept before a
// save, five of each, listed; a restore asked for is done at the next start,
// the present save kept first; what isn't a copy in the history is refused.
// (From Punch-Out's.)
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "save_history.hpp"
#include "system.hpp"

// The runtime's messages (no runtime here).
namespace qilin::rt {
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::exit(70);
}
void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
}  // namespace qilin::rt

namespace {

namespace fs = std::filesystem;
int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
bool write(const fs::path& p, const std::string& text) {
    std::string error;
    return dreamcast::write_file_safely(p, reinterpret_cast<const uint8_t*>(text.data()), text.size(), error);
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "dreamcast_save_history_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path card = dir / "memory_card_a1.bin";
    std::string error;

    check(dreamcast::backup_save(dir, "memory_card_a1.bin", error) && dreamcast::save_backups(dir).empty(),
          "no save yet: nothing to keep, and no error");
    check(!dreamcast::backup_save(dir, "notes.txt", error), "only the saves this program keeps");
    check(write(card, "save 0") && read(card) == "save 0" && !fs::exists(dir / "memory_card_a1.bin.tmp"),
          "a whole file written through a temporary one");

    // Seven saves: the five latest copies stay, the newest first.
    for (int k = 0; k < 7; ++k) {
        check(dreamcast::backup_save(dir, "memory_card_a1.bin", error), "a copy kept before a save");
        write(card, "save " + std::to_string(k + 1));
    }
    std::vector<dreamcast::SaveBackup> list = dreamcast::save_backups(dir);
    check(list.size() == 5, "five copies kept");
    check(list.size() == 5 && read(list[0].file) == "save 6" && read(list[4].file) == "save 2", "the newest first, the oldest two gone");
    check(!list.empty() && list[0].label.rfind("Memory card / ", 0) == 0 && list[0].label.ends_with(" UTC"), "labelled with the save and the time");

    // A restore of the oldest kept: at the next start, the present save kept first.
    check(dreamcast::request_save_restore(dir, list[4].file, error) && fs::exists(dir / "restore-save.txt"), "a restore asked for");
    check(dreamcast::apply_save_restore(dir, error) && read(card) == "save 2", "the copy restored at the start");
    check(!fs::exists(dir / "restore-save.txt"), "the request done and removed");
    list = dreamcast::save_backups(dir);
    check(list.size() == 5 && read(list[0].file) == "save 7", "the save it replaced is kept, so the restore can be undone");
    check(dreamcast::apply_save_restore(dir, error), "no request: nothing to do");

    // What isn't a copy in the history is refused, asked for or written by hand.
    check(!dreamcast::request_save_restore(dir, card, error), "the save itself isn't a copy");
    check(!dreamcast::request_save_restore(dir, dir / "save-backups" / "memory_card_a1.bin.history" / ".." / ".." / "x.bak", error),
          "nor a path out of the history");
    write(dir / "restore-save.txt", "../memory_card_a1.bin\n");
    check(!dreamcast::apply_save_restore(dir, error) && read(card) == "save 2" && fs::exists(dir / "restore-save.txt"),
          "a request naming something else: refused, the save unchanged, the request kept");
    fs::remove(dir / "restore-save.txt");

    // The flash memory has a history of its own.
    write(dir / "flash.bin", "flash");
    check(dreamcast::backup_save(dir, "flash.bin", error), "the flash memory's copy");
    list = dreamcast::save_backups(dir);
    bool flash = false;
    for (const auto& b : list) flash = flash || b.label.rfind("Console flash memory / ", 0) == 0;
    check(flash && list.size() == 6, "listed beside the memory card's");

    fs::remove_all(dir);
    std::printf("save_history_test: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
