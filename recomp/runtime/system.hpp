// The runtime around the lifted program: the registry of native replacements,
// lookups in the function table, and the one way the runtime stops when it
// meets something it doesn't handle: a report naming what was reached, where
// from, and the guest's registers.
//
// Runtime code: no std::format (macOS 10.15, see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>

#include "guest.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define QILIN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define QILIN_PRINTF(f, a)
#endif

namespace qilin::rt {

// What qilin lift generates (table.cpp): every function by address, and the
// SHA-256 of the boot file it lifted.
extern const FnEntry g_functions[];
extern const size_t g_function_count;
extern const char g_lifted_sha256[];

// Native code for a guest function the port replaces. Arguments are in
// r4-r7 (and fr4-fr11), the result goes in r0 (or fr0), as SHC passes them;
// it returns to the caller by returning. It must leave r8-r15 as they were and
// write no register its guest counterpart doesn't (callers rely on it).
using Native = void (*)(Context&);
struct Provided {
    uint32_t entry;
    const char* name;
    Native fn;
};

// Registers native code. Registering an address twice is a mistake and stops.
void provide(std::span<const Provided> natives);
// The native function's name for an address, or nullptr.
const char* native_name(uint32_t entry);

// The start of the function holding `address` (the table's last entry at or
// below it), or 0.
uint32_t function_containing(uint32_t address);

// Calls a guest function from native code: PR is set to a return address no
// guest code has, and the call must come back to it.
void call_guest(Context& c, uint32_t entry);

// The report: what stopped, where, the caller, the registers.
void report(std::FILE* out, const Stop& s, const Context* c);
// The return addresses on the guest stack from r15 up to `stack_top`, as a
// call chain (memory must be readable there).
void backtrace(std::FILE* out, const Context& c, uint32_t stack_top, unsigned most = 24);
// Ends the program with `code` once the game has started: the output
// flushed, no static destructors (guest threads, the sound mixer and the
// display's threads are still running and would use what they destroy). How
// a game's program ends; any thread may call it.
[[noreturn]] void end(int code);
[[noreturn]] void fatal(const char* format, ...) QILIN_PRINTF(1, 2);  // the message, then end(3)
// Also given fatal's message, before it ends the program: where nobody sees
// stderr (an iPad app), the host shows it.
void on_fatal(void (*show)(const char* message));
void log(const char* format, ...) QILIN_PRINTF(1, 2);

}  // namespace qilin::rt
