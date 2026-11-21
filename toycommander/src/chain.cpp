#include "chain.hpp"

#include "guest.hpp"
#include "system.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kRecords = 0x8C116ABCu, kRecordCount = 256, kHeads = 0x8C117ABCu;
constexpr uint32_t kInUse = 0x80000000u;

bool g_bus_dispatcher_installed = false;

uint32_t head_of(uint32_t event) { return kHeads + (event >> 4) * 4; }

// Kamui 2's callback registration (lifted): r4 = the dispatcher, r5 = 0.
// 0x8C087E34 sets the callback the guest code uses for event 0x900,
// 0x8C087E1C the one for 0x920.
void hook_kamui(rt::Context& c, uint32_t set_callback, uint32_t dispatcher) {
    rt::Context k = c;
    k.r[4] = dispatcher, k.r[5] = 0;
    rt::call_guest(k, set_callback);
}

// 0x8C0B399C: adds handler r5 with argument r7 to event r4's list at
// priority r6 (lower first; a new handler goes before others of its
// priority). Returns the record's index, or 0 when all are in use. Interrupts
// are masked meanwhile in the guest code; nothing runs between here anyway.
void shinobi_chain_add(rt::Context& c) {
    const uint32_t event = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(c.r[4])));
    const uint32_t handler = c.r[5], arg = c.r[7], priority = c.r[6] | kInUse;
    const bool kamui = event == 0x900u || event == 0x920u;
    const bool bus = (event >> 8) == 0xAu || (event >> 8) == 0xBu;
    if (!kamui && !bus) throw rt::Stop{rt::Stop::Unsupported, 0x8C0B399Cu, event};
    const uint32_t head = head_of(event);
    uint32_t next = rt::ld32(head), prev = next;
    while (next != 0 && rt::ld32(next + 12) < priority) prev = next, next = rt::ld32(next + 4);
    uint32_t index = 1;
    while (index < kRecordCount && (rt::ld32(kRecords + index * 16 + 12) & kInUse)) ++index;
    if (index == kRecordCount) {
        c.r[0] = 0;
        return;
    }
    const uint32_t record = kRecords + index * 16;
    if (next == 0 && prev == 0) {
        if (event == 0x900u) hook_kamui(c, 0x8C087E34u, 0x8C0B3B6Eu);
        else if (event == 0x920u) hook_kamui(c, 0x8C087E1Cu, 0x8C0B3BB8u);
        else g_bus_dispatcher_installed = true;  // the guest code puts 0x8C0B3BDE at VBR + 0x22C
        rt::st32(head, record);
        rt::st32(record + 4, 0);
        // A system bus event's level-4 mask bit: normal events (0xA00 + 0x20 n)
        // are bit 12 + n of 0xA05F6920, external ones (0xB00 + 0x20 n) bit n
        // of 0xA05F6924.
        if (bus) {
            const uint32_t n = (event & 0xFFu) >> 5;
            const uint32_t mask = (event >> 8) == 0xAu ? 0xA05F6920u : 0xA05F6924u;
            rt::st32(mask, rt::ld32(mask) | (1u << ((event >> 8) == 0xAu ? n + 12 : n)));
        }
    } else if (rt::ld32(head) == next) {
        rt::st32(record + 4, next);
        rt::st32(head, record);
    } else {
        rt::st32(prev + 4, record);
        rt::st32(record + 4, next);
    }
    rt::st32(record, handler), rt::st32(record + 8, arg), rt::st32(record + 12, priority);
    c.r[0] = index;
}

// 0x8C0B3CA4: takes record r4 (an index chain_add returned) out of its
// event's list and frees it. A record is first looked for as another
// record's next; failing that, as a list's head, and a list left empty
// gives its event back: Kamui 2's callback cleared (events 0x900 and
// 0x920), a system bus event's level-4 mask bit cleared (the guest code also
// puts the default handler back in its slot of the vector table, which the
// port doesn't keep).
void shinobi_chain_remove(rt::Context& c) {
    const uint32_t record = kRecords + (c.r[4] << 4);
    for (uint32_t index = 1; index < kRecordCount; ++index) {
        const uint32_t r = kRecords + index * 16;
        if (rt::ld32(r + 4) == record && (rt::ld32(r + 12) & kInUse)) {
            rt::st32(r + 4, rt::ld32(record + 4));
            rt::st32(record + 12, 0);
            return;
        }
    }
    for (uint32_t slot = 1; slot < 0xC0u; ++slot) {
        const uint32_t head = kHeads + slot * 4;
        if (rt::ld32(head) != record) continue;
        rt::st32(head, rt::ld32(record + 4));
        rt::st32(record + 12, 0);
        if (rt::ld32(head) != 0) return;
        const uint32_t event = slot << 4;
        if (event == 0x900u) hook_kamui(c, 0x8C087E34u, 0);
        else if (event == 0x920u) hook_kamui(c, 0x8C087E1Cu, 0);
        else if ((event >> 8) == 0xAu) rt::st32(0xA05F6920u, rt::ld32(0xA05F6920u) & ~(1u << (((event & 0xFFu) >> 5) + 12)));
        else if ((event >> 8) == 0xBu) rt::st32(0xA05F6924u, rt::ld32(0xA05F6924u) & ~(1u << ((event & 0xFFu) >> 5)));
        return;
    }
}

// 0x8C0B3B6E and 0x8C0B3BB8, the dispatchers Kamui 2 calls back: IMASK to 6
// (left so; the interrupt's return restores SR), then each handler of the
// list in turn, with its argument in r4.
void dispatch(rt::Context& c, uint32_t event) {
    c.sr = (c.sr & ~0xF0u) | 0x60u;
    for (uint32_t r = rt::ld32(head_of(event)); r != 0; r = rt::ld32(r + 4)) {
        c.r[4] = rt::ld32(r + 8);
        rt::call_guest(c, rt::ld32(r));
    }
}
}  // namespace

void raise_bus_event(rt::Context& c, uint32_t event) {
    if (!g_bus_dispatcher_installed || rt::ld32(head_of(event)) == 0) return;
    const rt::Context saved = c;
    dispatch(c, event);
    const uint32_t keep_ret = c.ret;
    c = saved;
    c.ret = keep_ret;
}

namespace {

void shinobi_chain_dispatch_900(rt::Context& c) { dispatch(c, 0x900u); }
void shinobi_chain_dispatch_920(rt::Context& c) { dispatch(c, 0x920u); }

constexpr rt::Provided kNatives[] = {
    {0x8C0B399Cu, "shinobi_chain_add", shinobi_chain_add},
    {0x8C0B3CA4u, "shinobi_chain_remove", shinobi_chain_remove},
    {0x8C0B3B6Eu, "shinobi_chain_dispatch_900", shinobi_chain_dispatch_900},
    {0x8C0B3BB8u, "shinobi_chain_dispatch_920", shinobi_chain_dispatch_920},
};

}  // namespace

void provide_chain() { rt::provide(kNatives); }

}  // namespace toycommander
