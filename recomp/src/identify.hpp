// `qilin identify <disc image or boot file> --json [--build <title>]`: what
// the player's disc is, for the launcher (tools/player-kit/launcher), as the
// contract every project's recompiler keeps (ADOPTING.md there). One JSON
// object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build (`--build`'s, or any
// this port knows without it), 3 when it can be read but isn't (the message
// names what it is and what's needed), 2 when it can't be read at all. The
// message is a plain sentence for the player. Here: the bootstrap names the
// boot file, whose SHA-256 names the build (bootfile.cpp's known builds).
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "bootfile.hpp"

namespace qilin {

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// `want` is a known build's title ("Toy Commander (USA)"), or empty for any;
// `builds` the builds this port knows (the tests give their own).
Identity identify(const std::filesystem::path& input, std::string_view want, std::span<const KnownBuild> builds);
inline Identity identify(const std::filesystem::path& input, std::string_view want) { return identify(input, want, known_builds()); }

std::string to_json(const Identity& identity);

}  // namespace qilin
