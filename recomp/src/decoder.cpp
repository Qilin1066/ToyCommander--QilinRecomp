#include "decoder.hpp"

#include <array>
#include <format>
#include <stdexcept>
#include <vector>

namespace qilin::sh4 {

namespace {

constexpr std::string_view kNames[] = {
    "(invalid)",
#define QILIN_SH4_NAME(id, name) name,
    QILIN_SH4_OPS(QILIN_SH4_NAME)
#undef QILIN_SH4_NAME
};
static_assert(std::size(kNames) == static_cast<size_t>(Op::count_));

using enum Opnd;

struct Row {
    Op op;
    uint16_t match, mask;
    std::array<Opnd, 3> ops;
    uint32_t flags;
};

constexpr uint16_t kNM = 0xF00F;   // 4-bit opcode, N, M, 4-bit function
constexpr uint16_t kN = 0xF0FF;    // N only
constexpr uint16_t kI8 = 0xFF00;   // 8-bit opcode and an 8-bit field
constexpr uint16_t kI12 = 0xF000;  // 4-bit opcode and 12 bits of fields
constexpr uint16_t kAll = 0xFFFF;
constexpr uint16_t kBank = 0xF08F;

constexpr uint32_t kLd = kLoad, kSt = kStore, kPriv = kPrivileged;
constexpr uint32_t kF = kFloat;
constexpr uint32_t kFP = kFloat | kFpPr;                // arithmetic: single or double by PR
constexpr uint32_t kFLd = kFloat | kFpSz | kLoad;       // fmov: 32 or 64 bits by SZ
constexpr uint32_t kFSt = kFloat | kFpSz | kStore;
constexpr uint32_t kJump = kBranch | kDelayed;

// No two rows share a word, and a row's fixed bits and operand fields cover
// all 16 bits exactly (both checked when the tables are built).
constexpr Row kRows[] = {
    // ---- data transfer ----
    {Op::mov_imm, 0xE000, kI12, {imm_s, rN}, 0},
    {Op::movw_pc, 0x9000, kI12, {pc_w, rN}, kLd | kPcRelative},
    {Op::movl_pc, 0xD000, kI12, {pc_l, rN}, kLd | kPcRelative},
    {Op::mov, 0x6003, kNM, {rM, rN}, 0},
    {Op::movb_st, 0x2000, kNM, {rM, at_N}, kSt},
    {Op::movw_st, 0x2001, kNM, {rM, at_N}, kSt},
    {Op::movl_st, 0x2002, kNM, {rM, at_N}, kSt},
    {Op::movb_ld, 0x6000, kNM, {at_M, rN}, kLd},
    {Op::movw_ld, 0x6001, kNM, {at_M, rN}, kLd},
    {Op::movl_ld, 0x6002, kNM, {at_M, rN}, kLd},
    {Op::movb_st_dec, 0x2004, kNM, {rM, at_dec_N}, kSt},
    {Op::movw_st_dec, 0x2005, kNM, {rM, at_dec_N}, kSt},
    {Op::movl_st_dec, 0x2006, kNM, {rM, at_dec_N}, kSt},
    {Op::movb_ld_inc, 0x6004, kNM, {at_inc_M, rN}, kLd},
    {Op::movw_ld_inc, 0x6005, kNM, {at_inc_M, rN}, kLd},
    {Op::movl_ld_inc, 0x6006, kNM, {at_inc_M, rN}, kLd},
    {Op::movb_st_disp, 0x8000, kI8, {r0, at_disp_M1}, kSt},
    {Op::movw_st_disp, 0x8100, kI8, {r0, at_disp_M2}, kSt},
    {Op::movl_st_disp, 0x1000, kI12, {rM, at_disp_N4}, kSt},
    {Op::movb_ld_disp, 0x8400, kI8, {at_disp_M1, r0}, kLd},
    {Op::movw_ld_disp, 0x8500, kI8, {at_disp_M2, r0}, kLd},
    {Op::movl_ld_disp, 0x5000, kI12, {at_disp_M4, rN}, kLd},
    {Op::movb_st_r0, 0x0004, kNM, {rM, at_r0_N}, kSt},
    {Op::movw_st_r0, 0x0005, kNM, {rM, at_r0_N}, kSt},
    {Op::movl_st_r0, 0x0006, kNM, {rM, at_r0_N}, kSt},
    {Op::movb_ld_r0, 0x000C, kNM, {at_r0_M, rN}, kLd},
    {Op::movw_ld_r0, 0x000D, kNM, {at_r0_M, rN}, kLd},
    {Op::movl_ld_r0, 0x000E, kNM, {at_r0_M, rN}, kLd},
    {Op::movb_st_gbr, 0xC000, kI8, {r0, at_gbr1}, kSt},
    {Op::movw_st_gbr, 0xC100, kI8, {r0, at_gbr2}, kSt},
    {Op::movl_st_gbr, 0xC200, kI8, {r0, at_gbr4}, kSt},
    {Op::movb_ld_gbr, 0xC400, kI8, {at_gbr1, r0}, kLd},
    {Op::movw_ld_gbr, 0xC500, kI8, {at_gbr2, r0}, kLd},
    {Op::movl_ld_gbr, 0xC600, kI8, {at_gbr4, r0}, kLd},
    {Op::mova, 0xC700, kI8, {pc_l, r0}, kPcRelative},
    {Op::movt, 0x0029, kN, {rN}, 0},
    {Op::swap_b, 0x6008, kNM, {rM, rN}, 0},
    {Op::swap_w, 0x6009, kNM, {rM, rN}, 0},
    {Op::xtrct, 0x200D, kNM, {rM, rN}, 0},
    // ---- arithmetic ----
    {Op::add, 0x300C, kNM, {rM, rN}, 0},
    {Op::add_imm, 0x7000, kI12, {imm_s, rN}, 0},
    {Op::addc, 0x300E, kNM, {rM, rN}, 0},
    {Op::addv, 0x300F, kNM, {rM, rN}, 0},
    {Op::cmp_eq_imm, 0x8800, kI8, {imm_s, r0}, 0},
    {Op::cmp_eq, 0x3000, kNM, {rM, rN}, 0},
    {Op::cmp_hs, 0x3002, kNM, {rM, rN}, 0},
    {Op::cmp_ge, 0x3003, kNM, {rM, rN}, 0},
    {Op::cmp_hi, 0x3006, kNM, {rM, rN}, 0},
    {Op::cmp_gt, 0x3007, kNM, {rM, rN}, 0},
    {Op::cmp_pl, 0x4015, kN, {rN}, 0},
    {Op::cmp_pz, 0x4011, kN, {rN}, 0},
    {Op::cmp_str, 0x200C, kNM, {rM, rN}, 0},
    {Op::div1, 0x3004, kNM, {rM, rN}, 0},
    {Op::div0s, 0x2007, kNM, {rM, rN}, 0},
    {Op::div0u, 0x0019, kAll, {}, 0},
    {Op::dmuls_l, 0x300D, kNM, {rM, rN}, 0},
    {Op::dmulu_l, 0x3005, kNM, {rM, rN}, 0},
    {Op::dt, 0x4010, kN, {rN}, 0},
    {Op::exts_b, 0x600E, kNM, {rM, rN}, 0},
    {Op::exts_w, 0x600F, kNM, {rM, rN}, 0},
    {Op::extu_b, 0x600C, kNM, {rM, rN}, 0},
    {Op::extu_w, 0x600D, kNM, {rM, rN}, 0},
    {Op::mac_l, 0x000F, kNM, {at_inc_M, at_inc_N}, kLd},
    {Op::mac_w, 0x400F, kNM, {at_inc_M, at_inc_N}, kLd},
    {Op::mul_l, 0x0007, kNM, {rM, rN}, 0},
    {Op::muls_w, 0x200F, kNM, {rM, rN}, 0},
    {Op::mulu_w, 0x200E, kNM, {rM, rN}, 0},
    {Op::neg, 0x600B, kNM, {rM, rN}, 0},
    {Op::negc, 0x600A, kNM, {rM, rN}, 0},
    {Op::sub, 0x3008, kNM, {rM, rN}, 0},
    {Op::subc, 0x300A, kNM, {rM, rN}, 0},
    {Op::subv, 0x300B, kNM, {rM, rN}, 0},
    // ---- logic ----
    {Op::and_, 0x2009, kNM, {rM, rN}, 0},
    {Op::and_imm, 0xC900, kI8, {imm_u, r0}, 0},
    {Op::and_b, 0xCD00, kI8, {imm_u, at_r0_gbr}, kLd | kSt},
    {Op::not_, 0x6007, kNM, {rM, rN}, 0},
    {Op::or_, 0x200B, kNM, {rM, rN}, 0},
    {Op::or_imm, 0xCB00, kI8, {imm_u, r0}, 0},
    {Op::or_b, 0xCF00, kI8, {imm_u, at_r0_gbr}, kLd | kSt},
    {Op::tas_b, 0x401B, kN, {at_N}, kLd | kSt},
    {Op::tst, 0x2008, kNM, {rM, rN}, 0},
    {Op::tst_imm, 0xC800, kI8, {imm_u, r0}, 0},
    {Op::tst_b, 0xCC00, kI8, {imm_u, at_r0_gbr}, kLd},
    {Op::xor_, 0x200A, kNM, {rM, rN}, 0},
    {Op::xor_imm, 0xCA00, kI8, {imm_u, r0}, 0},
    {Op::xor_b, 0xCE00, kI8, {imm_u, at_r0_gbr}, kLd | kSt},
    // ---- shift ----
    {Op::rotl, 0x4004, kN, {rN}, 0},
    {Op::rotr, 0x4005, kN, {rN}, 0},
    {Op::rotcl, 0x4024, kN, {rN}, 0},
    {Op::rotcr, 0x4025, kN, {rN}, 0},
    {Op::shad, 0x400C, kNM, {rM, rN}, 0},
    {Op::shld, 0x400D, kNM, {rM, rN}, 0},
    {Op::shal, 0x4020, kN, {rN}, 0},
    {Op::shar, 0x4021, kN, {rN}, 0},
    {Op::shll, 0x4000, kN, {rN}, 0},
    {Op::shlr, 0x4001, kN, {rN}, 0},
    {Op::shll2, 0x4008, kN, {rN}, 0},
    {Op::shlr2, 0x4009, kN, {rN}, 0},
    {Op::shll8, 0x4018, kN, {rN}, 0},
    {Op::shlr8, 0x4019, kN, {rN}, 0},
    {Op::shll16, 0x4028, kN, {rN}, 0},
    {Op::shlr16, 0x4029, kN, {rN}, 0},
    // ---- branch ----
    {Op::bf, 0x8B00, kI8, {br8}, kBranch | kConditional | kPcRelative},
    {Op::bf_s, 0x8F00, kI8, {br8}, kJump | kConditional | kPcRelative},
    {Op::bt, 0x8900, kI8, {br8}, kBranch | kConditional | kPcRelative},
    {Op::bt_s, 0x8D00, kI8, {br8}, kJump | kConditional | kPcRelative},
    {Op::bra, 0xA000, kI12, {br12}, kJump | kPcRelative},
    {Op::braf, 0x0023, kN, {rN}, kJump | kIndirect | kPcRelative},
    {Op::bsr, 0xB000, kI12, {br12}, kJump | kCall | kPcRelative},
    {Op::bsrf, 0x0003, kN, {rN}, kJump | kCall | kIndirect | kPcRelative},
    {Op::jmp, 0x402B, kN, {at_N}, kJump | kIndirect},
    {Op::jsr, 0x400B, kN, {at_N}, kJump | kCall | kIndirect},
    {Op::rts, 0x000B, kAll, {}, kJump | kReturn | kIndirect},
    // ---- system control ----
    {Op::clrmac, 0x0028, kAll, {}, 0},
    {Op::clrs, 0x0048, kAll, {}, 0},
    {Op::clrt, 0x0008, kAll, {}, 0},
    {Op::sets, 0x0058, kAll, {}, 0},
    {Op::sett, 0x0018, kAll, {}, 0},
    {Op::ldc_sr, 0x400E, kN, {rN, sr}, kPriv},
    {Op::ldc_gbr, 0x401E, kN, {rN, gbr}, 0},
    {Op::ldc_vbr, 0x402E, kN, {rN, vbr}, kPriv},
    {Op::ldc_ssr, 0x403E, kN, {rN, ssr}, kPriv},
    {Op::ldc_spc, 0x404E, kN, {rN, spc}, kPriv},
    {Op::ldc_dbr, 0x40FA, kN, {rN, dbr}, kPriv},
    {Op::ldc_bank, 0x408E, kBank, {rN, bank}, kPriv},
    {Op::ldcl_sr, 0x4007, kN, {at_inc_N, sr}, kPriv | kLd},
    {Op::ldcl_gbr, 0x4017, kN, {at_inc_N, gbr}, kLd},
    {Op::ldcl_vbr, 0x4027, kN, {at_inc_N, vbr}, kPriv | kLd},
    {Op::ldcl_ssr, 0x4037, kN, {at_inc_N, ssr}, kPriv | kLd},
    {Op::ldcl_spc, 0x4047, kN, {at_inc_N, spc}, kPriv | kLd},
    {Op::ldcl_dbr, 0x40F6, kN, {at_inc_N, dbr}, kPriv | kLd},
    {Op::ldcl_bank, 0x4087, kBank, {at_inc_N, bank}, kPriv | kLd},
    {Op::stc_sr, 0x0002, kN, {sr, rN}, kPriv},
    {Op::stc_gbr, 0x0012, kN, {gbr, rN}, 0},
    {Op::stc_vbr, 0x0022, kN, {vbr, rN}, kPriv},
    {Op::stc_ssr, 0x0032, kN, {ssr, rN}, kPriv},
    {Op::stc_spc, 0x0042, kN, {spc, rN}, kPriv},
    {Op::stc_sgr, 0x003A, kN, {sgr, rN}, kPriv},
    {Op::stc_dbr, 0x00FA, kN, {dbr, rN}, kPriv},
    {Op::stc_bank, 0x0082, kBank, {bank, rN}, kPriv},
    {Op::stcl_sr, 0x4003, kN, {sr, at_dec_N}, kPriv | kSt},
    {Op::stcl_gbr, 0x4013, kN, {gbr, at_dec_N}, kSt},
    {Op::stcl_vbr, 0x4023, kN, {vbr, at_dec_N}, kPriv | kSt},
    {Op::stcl_ssr, 0x4033, kN, {ssr, at_dec_N}, kPriv | kSt},
    {Op::stcl_spc, 0x4043, kN, {spc, at_dec_N}, kPriv | kSt},
    {Op::stcl_sgr, 0x4032, kN, {sgr, at_dec_N}, kPriv | kSt},
    {Op::stcl_dbr, 0x40F2, kN, {dbr, at_dec_N}, kPriv | kSt},
    {Op::stcl_bank, 0x4083, kBank, {bank, at_dec_N}, kPriv | kSt},
    {Op::lds_mach, 0x400A, kN, {rN, mach}, 0},
    {Op::lds_macl, 0x401A, kN, {rN, macl}, 0},
    {Op::lds_pr, 0x402A, kN, {rN, pr}, 0},
    {Op::ldsl_mach, 0x4006, kN, {at_inc_N, mach}, kLd},
    {Op::ldsl_macl, 0x4016, kN, {at_inc_N, macl}, kLd},
    {Op::ldsl_pr, 0x4026, kN, {at_inc_N, pr}, kLd},
    {Op::sts_mach, 0x000A, kN, {mach, rN}, 0},
    {Op::sts_macl, 0x001A, kN, {macl, rN}, 0},
    {Op::sts_pr, 0x002A, kN, {pr, rN}, 0},
    {Op::stsl_mach, 0x4002, kN, {mach, at_dec_N}, kSt},
    {Op::stsl_macl, 0x4012, kN, {macl, at_dec_N}, kSt},
    {Op::stsl_pr, 0x4022, kN, {pr, at_dec_N}, kSt},
    {Op::ldtlb, 0x0038, kAll, {}, kPriv},
    {Op::movca_l, 0x00C3, kN, {r0, at_N}, kSt | kCache},
    {Op::nop, 0x0009, kAll, {}, 0},
    {Op::ocbi, 0x0093, kN, {at_N}, kCache},
    {Op::ocbp, 0x00A3, kN, {at_N}, kCache},
    {Op::ocbwb, 0x00B3, kN, {at_N}, kCache},
    {Op::pref, 0x0083, kN, {at_N}, kCache},
    {Op::rte, 0x002B, kAll, {}, kJump | kReturn | kIndirect | kPriv},
    {Op::sleep, 0x001B, kAll, {}, kPriv},
    {Op::trapa, 0xC300, kI8, {imm_u}, kBranch},
    // ---- floating point ----
    {Op::fldi0, 0xF08D, kN, {frN}, kFP},
    {Op::fldi1, 0xF09D, kN, {frN}, kFP},
    {Op::fmov, 0xF00C, kNM, {frM, frN}, kF | kFpSz},
    {Op::fmov_ld, 0xF008, kNM, {at_M, frN}, kFLd},
    {Op::fmov_ld_r0, 0xF006, kNM, {at_r0_M, frN}, kFLd},
    {Op::fmov_ld_inc, 0xF009, kNM, {at_inc_M, frN}, kFLd},
    {Op::fmov_st, 0xF00A, kNM, {frM, at_N}, kFSt},
    {Op::fmov_st_dec, 0xF00B, kNM, {frM, at_dec_N}, kFSt},
    {Op::fmov_st_r0, 0xF007, kNM, {frM, at_r0_N}, kFSt},
    {Op::flds, 0xF01D, kN, {frN, fpul}, kF},
    {Op::fsts, 0xF00D, kN, {fpul, frN}, kF},
    {Op::fabs, 0xF05D, kN, {frN}, kFP},
    {Op::fadd, 0xF000, kNM, {frM, frN}, kFP},
    {Op::fcmp_eq, 0xF004, kNM, {frM, frN}, kFP},
    {Op::fcmp_gt, 0xF005, kNM, {frM, frN}, kFP},
    {Op::fdiv, 0xF003, kNM, {frM, frN}, kFP},
    {Op::float_, 0xF02D, kN, {fpul, frN}, kFP},
    {Op::fmac, 0xF00E, kNM, {fr0, frM, frN}, kFP},
    {Op::fmul, 0xF002, kNM, {frM, frN}, kFP},
    {Op::fneg, 0xF04D, kN, {frN}, kFP},
    {Op::fsqrt, 0xF06D, kN, {frN}, kFP},
    {Op::fsub, 0xF001, kNM, {frM, frN}, kFP},
    {Op::ftrc, 0xF03D, kN, {frN, fpul}, kFP},
    {Op::fcnvds, 0xF0BD, 0xF1FF, {drN, fpul}, kFP},
    {Op::fcnvsd, 0xF0AD, 0xF1FF, {fpul, drN}, kFP},
    {Op::fipr, 0xF0ED, kN, {fvM, fvN}, kFP},
    {Op::ftrv, 0xF1FD, 0xF3FF, {xmtrx, fvN}, kFP},
    {Op::fsca, 0xF0FD, 0xF1FF, {fpul, drN}, kFP},
    {Op::fsrra, 0xF07D, kN, {frN}, kFP},
    {Op::frchg, 0xFBFD, kAll, {}, kF | kFpscrWrite},
    {Op::fschg, 0xF3FD, kAll, {}, kF | kFpscrWrite},
    {Op::lds_fpscr, 0x406A, kN, {rN, fpscr}, kF | kFpscrWrite},
    {Op::lds_fpul, 0x405A, kN, {rN, fpul}, kF},
    {Op::ldsl_fpscr, 0x4066, kN, {at_inc_N, fpscr}, kF | kFpscrWrite | kLd},
    {Op::ldsl_fpul, 0x4056, kN, {at_inc_N, fpul}, kF | kLd},
    {Op::sts_fpscr, 0x006A, kN, {fpscr, rN}, kF},
    {Op::sts_fpul, 0x005A, kN, {fpul, rN}, kF},
    {Op::stsl_fpscr, 0x4062, kN, {fpscr, at_dec_N}, kF | kSt},
    {Op::stsl_fpul, 0x4052, kN, {fpul, at_dec_N}, kF | kSt},
};

struct Tables {
    std::array<Spec, static_cast<size_t>(Op::count_)> by_op{};
    std::vector<Spec> ordered;
    std::vector<uint8_t> by_word = std::vector<uint8_t>(65536, 0);  // Op of every word

    Tables() {
        static_assert(static_cast<size_t>(Op::count_) <= 256);
        for (const Row& r : kRows) {
            Spec s;
            s.op = r.op;
            s.name = kNames[static_cast<size_t>(r.op)];
            s.match = r.match;
            s.mask = r.mask;
            uint16_t fields = 0;
            for (size_t i = 0; i < r.ops.size(); ++i) {
                s.operands[i] = r.ops[i];
                const uint16_t f = field_mask(r.ops[i]);
                if (fields & f) throw std::logic_error(std::format("decoder table: {} has overlapping operand fields", s.name));
                fields |= f;
            }
            s.flags = r.flags;
            if ((s.match & ~s.mask) || (s.mask & fields) || uint16_t(s.mask | fields) != 0xFFFF)
                throw std::logic_error(std::format("decoder table: {} ({:04X}) doesn't define all 16 bits exactly once", s.name, s.match));
            Spec& slot = by_op[static_cast<size_t>(s.op)];
            if (slot.op != Op::invalid) throw std::logic_error(std::format("decoder table: {} listed twice", s.name));
            slot = s;
            ordered.push_back(s);
            // Every word of the form: the fixed bits plus each value of the fields.
            uint16_t sub = fields;
            for (;;) {
                uint8_t& w = by_word[s.match | sub];
                if (w) throw std::logic_error(std::format("decoder table: {:04X} decodes two ways", s.match | sub));
                w = static_cast<uint8_t>(s.op);
                if (sub == 0) break;
                sub = static_cast<uint16_t>((sub - 1) & fields);
            }
        }
        for (size_t i = 1; i < by_op.size(); ++i)
            if (by_op[i].op == Op::invalid) throw std::logic_error(std::format("decoder table: no row for {}", kNames[i]));
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

std::string hex(uint32_t a) { return std::format("0x{:08x}", a); }

std::string operand(const Insn& i, Opnd o, uint32_t pc) {
    switch (o) {
    case none: return {};
    case rN: return std::format("r{}", i.n());
    case rM: return std::format("r{}", i.m());
    case r0: return "r0";
    case imm_s: return std::format("#{}", i.imm_s());
    case imm_u: return std::format("#{}", i.imm_u());
    case pc_w:
    case pc_l: return hex(i.pc_relative(pc));
    case at_N: return std::format("@r{}", i.n());
    case at_M: return std::format("@r{}", i.m());
    case at_dec_N: return std::format("@-r{}", i.n());
    case at_inc_N: return std::format("@r{}+", i.n());
    case at_inc_M: return std::format("@r{}+", i.m());
    case at_disp_M1: return std::format("@({},r{})", i.disp4(), i.m());
    case at_disp_M2: return std::format("@({},r{})", i.disp4() * 2, i.m());
    case at_disp_M4: return std::format("@({},r{})", i.disp4() * 4, i.m());
    case at_disp_N4: return std::format("@({},r{})", i.disp4() * 4, i.n());
    case at_r0_N: return std::format("@(r0,r{})", i.n());
    case at_r0_M: return std::format("@(r0,r{})", i.m());
    case at_gbr1: return std::format("@({},gbr)", i.disp8());
    case at_gbr2: return std::format("@({},gbr)", i.disp8() * 2);
    case at_gbr4: return std::format("@({},gbr)", i.disp8() * 4);
    case at_r0_gbr: return "@(r0,gbr)";
    case br8:
    case br12: return hex(i.branch_target(pc));
    case sr: return "sr";
    case gbr: return "gbr";
    case vbr: return "vbr";
    case ssr: return "ssr";
    case spc: return "spc";
    case sgr: return "sgr";
    case dbr: return "dbr";
    case bank: return std::format("r{}_bank", i.bank());
    case mach: return "mach";
    case macl: return "macl";
    case pr: return "pr";
    case fpul: return "fpul";
    case fpscr: return "fpscr";
    case frN: return std::format("fr{}", i.n());
    case frM: return std::format("fr{}", i.m());
    case fr0: return "fr0";
    case drN: return std::format("dr{}", i.n() & 14);
    case fvN: return std::format("fv{}", i.n() & 12);
    case fvM: return std::format("fv{}", (i.n() & 3) * 4);
    case xmtrx: return "xmtrx";
    }
    throw std::logic_error("unknown operand kind");
}

}  // namespace

uint16_t field_mask(Opnd o) {
    switch (o) {
    case rN: case at_N: case at_dec_N: case at_inc_N: case at_r0_N: case frN: return 0x0F00;
    case rM: case at_M: case at_inc_M: case at_r0_M: case frM: return 0x00F0;
    case imm_s: case imm_u: case pc_w: case pc_l: case br8:
    case at_gbr1: case at_gbr2: case at_gbr4:
    case at_disp_M1: case at_disp_M2: case at_disp_M4: return 0x00FF;
    case at_disp_N4: return 0x0F0F;
    case br12: return 0x0FFF;
    case bank: return 0x0070;
    case drN: return 0x0E00;
    case fvN: return 0x0C00;
    case fvM: return 0x0300;
    default: return 0;
    }
}

uint32_t Insn::branch_target(uint32_t pc) const {
    const Spec& s = spec(op);
    if (s.operands[0] == br8) return pc + 4 + static_cast<uint32_t>(imm_s() * 2);
    if (s.operands[0] == br12) return pc + 4 + static_cast<uint32_t>((static_cast<int32_t>(word << 20) >> 20) * 2);
    throw std::logic_error("branch_target of an instruction without one");
}

uint32_t Insn::pc_relative(uint32_t pc) const {
    const Spec& s = spec(op);
    if (s.operands[0] == pc_w) return pc + 4 + disp8() * 2;
    if (s.operands[0] == pc_l) return (pc & ~3u) + 4 + disp8() * 4;
    throw std::logic_error("pc_relative of an instruction without a PC-relative operand");
}

const Spec& spec(Op op) { return tables().by_op[static_cast<size_t>(op)]; }
std::span<const Spec> specs() { return tables().ordered; }

Insn decode(uint16_t word) { return {word, static_cast<Op>(tables().by_word[word])}; }

std::string disasm(const Insn& insn, uint32_t pc) {
    if (!insn.valid()) throw std::logic_error("disasm of an invalid instruction");
    const Spec& s = spec(insn.op);
    std::string text{s.name};
    for (size_t i = 0; i < std::size(s.operands) && s.operands[i] != none; ++i) {
        text += i ? "," : " ";
        text += operand(insn, s.operands[i], pc);
    }
    return text;
}

}  // namespace qilin::sh4
