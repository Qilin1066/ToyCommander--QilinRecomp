// Whether the player's data (a disc image, an extracted disc's folder, a ROM
// set) is what this download builds from. The launcher checks only that it
// is the right kind of thing (a file with one of game.txt's extensions, or a
// folder); what it is, the project's recompiler says: game.txt's `identify`
// command (by default `qilin identify <path> --json`), whose contract is in
// ADOPTING.md. It prints one JSON object
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and exits 0 when it is the supported build, 3 when it can be read but
// isn't (the message names what it is and what's needed), 2 when it can't
// be read at all.
#pragma once

#include <filesystem>
#include <string>

#include "kit.hpp"

namespace qilin::launcher {

struct InputCheck {
    bool ok = false;
    std::string message;  // plain words for the player, either way
    std::string title, id, version;  // what the recompiler says it is (may be empty)
};

// Whether `path` has one of game.txt's extensions (any, if it names none).
bool input_name_ok(const Kit& kit, const std::filesystem::path& path);

// Runs game.txt's `identify` on `input` (in work/, its answer kept in
// work/logs/identify.json).
InputCheck check_input(const Kit& kit, const std::filesystem::path& input);

}  // namespace qilin::launcher
