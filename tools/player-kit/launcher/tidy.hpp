// Keeping the download's folder as the release made it.
//
// Unpacking a new version over an old one leaves the old one's files behind
// (the .bat and play.ps1 of the downloads before the launcher, Ninja, files a
// newer kit no longer has). At every start the launcher removes them:
//   - in kit\ and LICENSES\, every file kit\files.txt (written by package.sh:
//     each file the release holds, one per line) doesn't name;
//   - at the top, what older downloads kept there: Play <Title>.bat, the game
//     and its logs (now in work\);
//   - in work\, Ninja's files and the old remembered disc, which moves to the
//     per-user data folder if none is remembered there yet.
// Nothing else is touched: no file outside these, none through a link.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "kit.hpp"

namespace qilin::launcher {

struct Tidied {
    std::vector<std::filesystem::path> removed;  // what went
    std::vector<std::string> problems;           // what couldn't (in use, say): tried again next start
};

Tidied tidy_folder(const Kit& kit);

}  // namespace qilin::launcher
