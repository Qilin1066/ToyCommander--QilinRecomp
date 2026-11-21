// Stands in for the code qilin lift makes from a game's boot file (an empty
// function table), so a game's program can be linked without it: CI builds
// this to show that the game's library (<game>_game, or the player kit's
// archive) and its system libraries are complete.
#include <cstddef>

#include "guest.hpp"

namespace qilin::rt {
extern const FnEntry g_functions[] = {{0, nullptr}};
extern const size_t g_function_count = 0;
extern const char g_lifted_sha256[] = "";
}  // namespace qilin::rt
