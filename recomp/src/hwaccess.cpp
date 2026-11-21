#include "hwaccess.hpp"

namespace qilin {

using sh4::Insn;
using sh4::Op;

std::string_view hw_region(uint32_t ea) {
    // P4: the SH-4's own space, not translated.
    if (ea >= 0xE0000000u) {
        if (ea < 0xE4000000u) return "store queues";
        if (ea < 0xF0000000u) return "unmapped (P4)";
        if (ea < 0xF8000000u) return "cache and TLB arrays";
        if (ea < 0xFF000000u) return "unmapped (P4)";
        switch (ea >> 20) {
        case 0xFF0: return "CPU: cache and exception control (CCN)";
        case 0xFF2: return "CPU: user break controller (UBC)";
        case 0xFF8: return "CPU: bus state controller (BSC)";
        case 0xFF9: return "CPU: SDRAM mode registers";
        case 0xFFA: return "CPU: DMA controller (DMAC)";
        case 0xFFC: return ea < 0xFFC80000u ? "CPU: clock and power (CPG)" : "CPU: real-time clock (RTC)";
        case 0xFFD: return ea < 0xFFD80000u ? "CPU: interrupt controller (INTC)" : "CPU: timers (TMU)";
        case 0xFFE: return ea < 0xFFE80000u ? "CPU: serial port (SCI)" : "CPU: serial port with FIFO (SCIF)";
        case 0xFFF: return "CPU: H-UDI";
        default: return "CPU: unmapped register";
        }
    }
    // P0-P3: the 29-bit physical address, whatever the cache view.
    const uint32_t pa = ea & 0x1FFFFFFFu;
    const bool cached = ea < 0xA0000000u || ea >= 0xC0000000u;  // P2 (0xA0000000-0xBFFFFFFF) is uncached
    if (pa >= 0x0C000000u && pa < 0x10000000u)  // area 3: 16 MB of RAM, mirrored through the area
        return ea >= 0x8C000000u && ea < 0x8D000000u ? "" : (cached ? "RAM (another view)" : "RAM (uncached)");
    if (pa < 0x00200000u) return "boot ROM";
    if (pa < 0x00220000u) return "flash memory";
    if (pa >= 0x005F6800u && pa < 0x005F6900u) return "system bus registers (DMA, interrupts)";
    if (pa >= 0x005F6900u && pa < 0x005F6C00u) return "system bus registers";
    if (pa >= 0x005F6C00u && pa < 0x005F7000u) return "Maple registers";
    if (pa >= 0x005F7000u && pa < 0x005F7400u) return "GD-ROM registers";
    if (pa >= 0x005F7400u && pa < 0x005F7800u) return "G1 bus registers";
    if (pa >= 0x005F7800u && pa < 0x005F7C00u) return "G2 bus registers";
    if (pa >= 0x005F7C00u && pa < 0x005F8000u) return "PowerVR2 DMA registers";
    if (pa >= 0x005F8000u && pa < 0x005FA000u) return "PowerVR2 registers";
    if (pa >= 0x00600000u && pa < 0x00680000u) return "modem";
    if (pa >= 0x00700000u && pa < 0x00710000u) return "sound chip registers";
    if (pa >= 0x00710000u && pa < 0x00710010u) return "sound chip real-time clock";
    if (pa >= 0x00800000u && pa < 0x00A00000u) return "sound memory";
    if (pa >= 0x01000000u && pa < 0x04000000u) return "expansion (G2)";
    if (pa >= 0x04000000u && pa < 0x08000000u) return pa & 0x01000000u ? "video memory (32-bit view)" : "video memory (64-bit view)";
    if (pa >= 0x10000000u && pa < 0x10800000u) return "tile accelerator: polygons";
    if (pa >= 0x10800000u && pa < 0x11000000u) return "tile accelerator: YUV converter";
    if (pa >= 0x11000000u && pa < 0x12000000u) return "tile accelerator: texture memory";
    if (pa >= 0x12000000u && pa < 0x14000000u) return "tile accelerator (mirror)";
    return "unmapped";
}

std::vector<HwAccess> hardware_accesses(const BootFile& boot, const Cfg& cfg, const Function& f) {
    const auto regs = register_constants(boot, cfg, f);
    std::vector<HwAccess> out;
    for (const auto& [pc, r] : regs) {
        const Insn i = sh4::decode(boot.u16(pc));
        const sh4::Spec& s = sh4::spec(i.op);
        const uint32_t flags = s.flags;
        if (!(flags & (sh4::kLoad | sh4::kStore | sh4::kCache))) continue;
        const uint32_t size = sh4::access_size(i);
        const std::optional<uint32_t> ea = sh4::register_address(i, r);
        if (!ea || hw_region(*ea).empty()) continue;
        HwAccess h;
        h.pc = pc;
        h.ea = *ea;
        h.size = size;
        if (flags & sh4::kCache) h.kind = i.op == Op::movca_l ? HwAccess::store : HwAccess::cache;
        else if ((flags & sh4::kLoad) && (flags & sh4::kStore)) h.kind = HwAccess::modify;
        else h.kind = (flags & sh4::kStore) ? HwAccess::store : HwAccess::load;
        out.push_back(h);
    }
    return out;
}

}  // namespace qilin
