// bu's coroutines, over host threads. Shinobi's small thread library
// (0x8C0E0BC8-0x8C0E0F9E) runs guest threads with stacks of their own,
// switched by a full save and restore of the registers (0x8C0A2654,
// 0x8C0A25E0: setjmp and longjmp). Lifted code runs on the host's stack, so
// a switch can't be followed as calls: each guest thread runs on a host
// thread of its own, with a context of its own, and exactly one runs at a
// time, handing over where the guest code would switch. The library is
// lifted but for its two switching functions, which are these natives:
// the scheduler's pass (0x8C0E0ED0) and a thread's yield (0x8C0E0E8E).
// Everything they keep is the guest's own records, as the guest code keeps
// them.
#pragma once

#include "guest.hpp"

namespace toyracer {

// Registers the natives.
void provide_threads();
// When a stop happened in a guest thread: that thread's context (else null),
// for the report.
const qilin::rt::Context* thread_stop_context();

}  // namespace toyracer
