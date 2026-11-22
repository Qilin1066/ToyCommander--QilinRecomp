// The saves' history (from Punch-Out's, its v0.22.0): before a save file in
// the data folder changes, a flushed copy of it is kept in save-backups/
// beside it, the five latest of each. The settings menu lists them; one
// picked is restored at the next start (restore-save.txt names it), before
// the game reads anything, the present save backed up first so a restore can
// itself be undone. The files are the memory card (memory_card_a1.bin) and
// the console's flash memory (flash.bin); nothing else can be named.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dreamcast {

struct SaveBackup {
    std::filesystem::path file;  // the copy, in save-backups/
    std::string label;           // "Memory card / 2026-11-22 10:41:07 UTC"
};

// A flushed copy of `folder`/`name` into the history (nothing if the save
// doesn't exist yet), the oldest beyond five removed. False, with `error`
// saying why, if the copy couldn't be made.
bool backup_save(const std::filesystem::path& folder, const std::string& name, std::string& error);
// Every copy, the newest first.
std::vector<SaveBackup> save_backups(const std::filesystem::path& folder);
// Ask for a copy to be restored at the next start.
bool request_save_restore(const std::filesystem::path& folder, const std::filesystem::path& backup, std::string& error);
// At start-up, before the saves are read: the restore asked for, if any.
// False, with `error`, if one was asked for and couldn't be done (the request
// is kept, for the next start and for a look at it).
bool apply_save_restore(const std::filesystem::path& folder, std::string& error);

// Write a whole file safely: into a temporary file beside it, flushed to
// the disk, then renamed over it, so a failure or a crash leaves the old one.
bool write_file_safely(const std::filesystem::path& file, const uint8_t* data, size_t n, std::string& error);

// Start this program again with its command line (the menu's "Restore and
// restart"); main records the command line first.
void set_command_line(int argc, char** argv);
[[noreturn]] void restart_program();

}  // namespace dreamcast
