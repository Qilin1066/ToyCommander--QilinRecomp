// Shinobi's interrupt chains (syChain), natively, over the same guest RAM and
// state as the guest code: handlers kept per event in lists ordered by
// priority, each called with its argument when the event happens.
//   0x8C116ABC  256 records of 16 bytes: handler, next, argument,
//               priority | 0x80000000 (in use); record 0 is never used
//   0x8C117ABC  the lists' heads, one word per event code / 16
// Events 0x900 and 0x920 come from Kamui 2's two callbacks (the guest code
// hooks its own dispatchers into Kamui 2 when a list gets its first handler);
// the others (the CPU's exception codes below 0x900, the system bus's events
// above 0x920) are installed in the exception table and are not handled yet.
#pragma once

#include <cstdint>

#include "guest.hpp"

namespace shinobi {

// The system bus's event (0xA00 + 0x20 n: normal status bit n + 12... as
// shinobi_chain_add numbers them) happening: its handlers run, as Shinobi's
// level-4 dispatcher would run them, if any are registered. The registers
// are as they were afterwards, as after an interrupt.
void raise_bus_event(qilin::rt::Context& c, uint32_t event);
// Registers the chain's natives.
void provide_chain();

}  // namespace shinobi
