// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way.
#include "natives.hpp"

#include <cstring>

#include "system.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

// 0x8C04EEE0, the C runtime's clear: zeroes r5 bytes from r4 and returns r4
// in r0. The guest code clears the 32-byte blocks through the store queues
// (zeros written to them with 64-bit fmov from fr0/fr1, then pref), which
// leaves fr0 and fr1 zero when it takes that path: when, after the bytes up to
// the next 32-byte boundary (a whole 32 when r4 is aligned), 32 or more remain.
// The store queues reach RAM because Shinobi's set-up points them there.
void memclr(rt::Context& c) {
    const uint32_t dest = c.r[4], n = c.r[5];
    c.r[0] = dest;
    if (n == 0) return;
    rt::check(dest, n);
    std::memset(rt::g_mem + dest, 0, n);
    const uint32_t head = 32 - (dest & 31);
    if (n >= 32 && n - head >= 32) c.fr[0] = c.fr[1] = 0.0f;
}

// 0x8C0B068A, Shinobi's initialisation, and 0x8C0B06E2, its exit: they walk
// the library handles between the "Lib Handle Start" and "Lib Handle End"
// markers at r4 + 48 (one entry in this build, at 0x8C010040) and register
// each in a table of Shinobi's own (0x8C0B05A0), or remove it (0x8C0B05F0),
// with interrupts masked around it. Only Shinobi reads that table, and the
// port replaces Shinobi: nothing to do. Both return 0 (-1 if the start marker
// is missing).
void shinobi_library_init(rt::Context& c) { c.r[0] = 0; }
void shinobi_library_exit(rt::Context& c) { c.r[0] = 0; }

const rt::Provided kNatives[] = {
    {0x8C04EEE0u, "memclr", memclr},
    {0x8C0B068Au, "shinobi_library_init", shinobi_library_init},
    {0x8C0B06E2u, "shinobi_library_exit", shinobi_library_exit},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }

}  // namespace toycommander
