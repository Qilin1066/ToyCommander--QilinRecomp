// iPadOS and iOS (QILIN_IOS builds only): what the system needs that the
// desktops don't. docs/platforms.md has the design ("The game on iPadOS").
//
// Runtime code: no std::format.
#pragma once

#include <cstddef>
#include <filesystem>

namespace qilin::host::ios {

// First thing in main: SDL's hints for a full-screen game (landscape, the
// home indicator hidden and the screen's edges kept for the game), SDL's
// video (for messages before the window opens), the data folder made, and
// the log (stderr) moved into it as log.txt, unless QILIN_LOG=console or
// the self test.
void start();

// QILIN_IOS_SELFTEST=1 (CI, in the Simulator): the start-up runs as far as
// it can without a disc, logging instead of showing messages, then opens the
// window and draws with MoltenVK (self_test_display).
bool self_test();

// The app's Documents folder: what the Files app shows as On My iPad, <the
// game's name>. The disc image goes there.
std::filesystem::path documents();

// A message with one button, waited for. In the self test: logged only.
void tell(const char* title, const char* text);
// A message with two buttons: true for `yes`. In the self test: logged, false.
bool ask(const char* title, const char* text, const char* yes, const char* no);
// For rt::on_fatal and die(): what stopped the game, shown before the app ends.
void show_fatal(const char* message);

// The main thread's stack is 1 MB on iOS, too little for lifted code, which
// nests as deeply as the guest's calls (the desktops give it 8 or 16 MB):
// runs body(arg) on a stack of `bytes` of its own, still on the main thread
// (UIKit's and SDL's), and returns what it returns. body must not throw.
int run_on_big_stack(int (*body)(void*), void* arg, size_t bytes);

// The self test's second half: the window, MoltenVK's device and a few
// frames presented. Returns 0, or ends the program through die().
int self_test_display();

}  // namespace qilin::host::ios
