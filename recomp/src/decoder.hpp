// SH-4 instruction decoder.
//
// The Dreamcast's CPU is a Hitachi SH7091, an SH7750-family SH-4. Every
// instruction is one 16-bit little-endian word, so the whole encoding space
// is 65,536 words and the decoder is a table over all of it.
//
// The decoder is table-driven: one `Spec` per instruction form, with its fixed
// bits and its operand fields. Every bit a spec doesn't define belongs to one
// of its operands (checked when the table is built), no two specs share a
// word, and anything outside the SH7750's set decodes as invalid: the SH-2A,
// SH-4A and SH-DSP additions other disassemblers accept are not SH-4
// instructions. A retail compiler never emits those, and the recompiler fails
// loudly on them (see docs/roadmap-and-design-targets.md).
//
// Floating-point instructions decode the same whatever FPSCR holds, but what
// some of them do depends on its PR (precision) and SZ (transfer size) bits:
// those carry kFpPr / kFpSz, and their text uses the single-precision names
// (`fr`), as other disassemblers do. Working out the mode at each instruction
// is the analysis passes' job, not the decoder's.
//
// The text form (`disasm`) follows GNU objdump's, which Capstone's follows
// too, so tools/check/verify_decoder.py can compare word by word.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qilin::sh4 {

// Every SH-4 instruction form: identifier and mnemonic. Forms that share a
// mnemonic (the many `mov.l`s) differ in their operands.
#define QILIN_SH4_OPS(X)                                                                      \
    /* data transfer */                                                                       \
    X(mov_imm, "mov") X(movw_pc, "mov.w") X(movl_pc, "mov.l") X(mov, "mov")                   \
    X(movb_st, "mov.b") X(movw_st, "mov.w") X(movl_st, "mov.l")                               \
    X(movb_ld, "mov.b") X(movw_ld, "mov.w") X(movl_ld, "mov.l")                               \
    X(movb_st_dec, "mov.b") X(movw_st_dec, "mov.w") X(movl_st_dec, "mov.l")                   \
    X(movb_ld_inc, "mov.b") X(movw_ld_inc, "mov.w") X(movl_ld_inc, "mov.l")                   \
    X(movb_st_disp, "mov.b") X(movw_st_disp, "mov.w") X(movl_st_disp, "mov.l")                \
    X(movb_ld_disp, "mov.b") X(movw_ld_disp, "mov.w") X(movl_ld_disp, "mov.l")                \
    X(movb_st_r0, "mov.b") X(movw_st_r0, "mov.w") X(movl_st_r0, "mov.l")                      \
    X(movb_ld_r0, "mov.b") X(movw_ld_r0, "mov.w") X(movl_ld_r0, "mov.l")                      \
    X(movb_st_gbr, "mov.b") X(movw_st_gbr, "mov.w") X(movl_st_gbr, "mov.l")                   \
    X(movb_ld_gbr, "mov.b") X(movw_ld_gbr, "mov.w") X(movl_ld_gbr, "mov.l")                   \
    X(mova, "mova") X(movt, "movt") X(swap_b, "swap.b") X(swap_w, "swap.w")                   \
    X(xtrct, "xtrct")                                                                         \
    /* arithmetic */                                                                          \
    X(add, "add") X(add_imm, "add") X(addc, "addc") X(addv, "addv")                           \
    X(cmp_eq_imm, "cmp/eq") X(cmp_eq, "cmp/eq") X(cmp_hs, "cmp/hs") X(cmp_ge, "cmp/ge")       \
    X(cmp_hi, "cmp/hi") X(cmp_gt, "cmp/gt") X(cmp_pl, "cmp/pl") X(cmp_pz, "cmp/pz")           \
    X(cmp_str, "cmp/str") X(div1, "div1") X(div0s, "div0s") X(div0u, "div0u")                 \
    X(dmuls_l, "dmuls.l") X(dmulu_l, "dmulu.l") X(dt, "dt")                                   \
    X(exts_b, "exts.b") X(exts_w, "exts.w") X(extu_b, "extu.b") X(extu_w, "extu.w")           \
    X(mac_l, "mac.l") X(mac_w, "mac.w") X(mul_l, "mul.l") X(muls_w, "muls.w")                 \
    X(mulu_w, "mulu.w") X(neg, "neg") X(negc, "negc") X(sub, "sub") X(subc, "subc")           \
    X(subv, "subv")                                                                           \
    /* logic */                                                                               \
    X(and_, "and") X(and_imm, "and") X(and_b, "and.b") X(not_, "not")                         \
    X(or_, "or") X(or_imm, "or") X(or_b, "or.b") X(tas_b, "tas.b")                            \
    X(tst, "tst") X(tst_imm, "tst") X(tst_b, "tst.b")                                         \
    X(xor_, "xor") X(xor_imm, "xor") X(xor_b, "xor.b")                                        \
    /* shift */                                                                               \
    X(rotl, "rotl") X(rotr, "rotr") X(rotcl, "rotcl") X(rotcr, "rotcr")                       \
    X(shad, "shad") X(shld, "shld") X(shal, "shal") X(shar, "shar")                           \
    X(shll, "shll") X(shlr, "shlr") X(shll2, "shll2") X(shlr2, "shlr2")                       \
    X(shll8, "shll8") X(shlr8, "shlr8") X(shll16, "shll16") X(shlr16, "shlr16")               \
    /* branch */                                                                              \
    X(bf, "bf") X(bf_s, "bf/s") X(bt, "bt") X(bt_s, "bt/s") X(bra, "bra") X(braf, "braf")     \
    X(bsr, "bsr") X(bsrf, "bsrf") X(jmp, "jmp") X(jsr, "jsr") X(rts, "rts")                   \
    /* system control */                                                                      \
    X(clrmac, "clrmac") X(clrs, "clrs") X(clrt, "clrt") X(sets, "sets") X(sett, "sett")       \
    X(ldc_sr, "ldc") X(ldc_gbr, "ldc") X(ldc_vbr, "ldc") X(ldc_ssr, "ldc")                    \
    X(ldc_spc, "ldc") X(ldc_dbr, "ldc") X(ldc_bank, "ldc")                                    \
    X(ldcl_sr, "ldc.l") X(ldcl_gbr, "ldc.l") X(ldcl_vbr, "ldc.l") X(ldcl_ssr, "ldc.l")        \
    X(ldcl_spc, "ldc.l") X(ldcl_dbr, "ldc.l") X(ldcl_bank, "ldc.l")                           \
    X(stc_sr, "stc") X(stc_gbr, "stc") X(stc_vbr, "stc") X(stc_ssr, "stc")                    \
    X(stc_spc, "stc") X(stc_sgr, "stc") X(stc_dbr, "stc") X(stc_bank, "stc")                  \
    X(stcl_sr, "stc.l") X(stcl_gbr, "stc.l") X(stcl_vbr, "stc.l") X(stcl_ssr, "stc.l")        \
    X(stcl_spc, "stc.l") X(stcl_sgr, "stc.l") X(stcl_dbr, "stc.l") X(stcl_bank, "stc.l")      \
    X(lds_mach, "lds") X(lds_macl, "lds") X(lds_pr, "lds")                                    \
    X(ldsl_mach, "lds.l") X(ldsl_macl, "lds.l") X(ldsl_pr, "lds.l")                           \
    X(sts_mach, "sts") X(sts_macl, "sts") X(sts_pr, "sts")                                    \
    X(stsl_mach, "sts.l") X(stsl_macl, "sts.l") X(stsl_pr, "sts.l")                           \
    X(ldtlb, "ldtlb") X(movca_l, "movca.l") X(nop, "nop") X(ocbi, "ocbi") X(ocbp, "ocbp")     \
    X(ocbwb, "ocbwb") X(pref, "pref") X(rte, "rte") X(sleep, "sleep") X(trapa, "trapa")       \
    /* floating point */                                                                      \
    X(fldi0, "fldi0") X(fldi1, "fldi1") X(fmov, "fmov") X(fmov_ld, "fmov")                    \
    X(fmov_ld_r0, "fmov") X(fmov_ld_inc, "fmov") X(fmov_st, "fmov")                           \
    X(fmov_st_dec, "fmov") X(fmov_st_r0, "fmov") X(flds, "flds") X(fsts, "fsts")              \
    X(fabs, "fabs") X(fadd, "fadd") X(fcmp_eq, "fcmp/eq") X(fcmp_gt, "fcmp/gt")               \
    X(fdiv, "fdiv") X(float_, "float") X(fmac, "fmac") X(fmul, "fmul") X(fneg, "fneg")        \
    X(fsqrt, "fsqrt") X(fsub, "fsub") X(ftrc, "ftrc") X(fcnvds, "fcnvds")                     \
    X(fcnvsd, "fcnvsd") X(fipr, "fipr") X(ftrv, "ftrv") X(fsca, "fsca") X(fsrra, "fsrra")     \
    X(frchg, "frchg") X(fschg, "fschg")                                                       \
    X(lds_fpscr, "lds") X(lds_fpul, "lds") X(ldsl_fpscr, "lds.l") X(ldsl_fpul, "lds.l")       \
    X(sts_fpscr, "sts") X(sts_fpul, "sts") X(stsl_fpscr, "sts.l") X(stsl_fpul, "sts.l")

enum class Op : uint16_t {
    invalid,
#define QILIN_SH4_ENUM(id, name) id,
    QILIN_SH4_OPS(QILIN_SH4_ENUM)
#undef QILIN_SH4_ENUM
    count_
};

// Operand kinds, in the order the text prints them. N is the register field
// in bits 11-8, M the one in bits 7-4 (bit 0 is the least significant).
enum class Opnd : uint8_t {
    none,
    rN, rM,             // general register
    r0,                 // the fixed register r0 (defines no bits)
    imm_s, imm_u,       // 8-bit immediate, sign- or zero-extended: "#-1", "#255"
    pc_w, pc_l,         // PC-relative data: pc + 4 + disp*2, (pc & ~3) + 4 + disp*4
    at_N, at_M,         // @rN, @rM
    at_dec_N,           // @-rN
    at_inc_N, at_inc_M, // @rN+, @rM+
    at_disp_M1, at_disp_M2, at_disp_M4,  // @(disp,rM): 4-bit disp times 1, 2, 4
    at_disp_N4,         // @(disp,rN): 4-bit disp times 4
    at_r0_N, at_r0_M,   // @(r0,rN), @(r0,rM)
    at_gbr1, at_gbr2, at_gbr4,  // @(disp,gbr): 8-bit disp times 1, 2, 4
    at_r0_gbr,          // @(r0,gbr) (defines no bits)
    br8, br12,          // branch target: pc + 4 + disp*2, disp 8 or 12 bits signed
    sr, gbr, vbr, ssr, spc, sgr, dbr,  // control registers (define no bits)
    bank,               // rB_bank, B in bits 6-4
    mach, macl, pr, fpul, fpscr,       // system registers (define no bits)
    frN, frM, fr0,      // floating-point register (fr0: fixed)
    drN,                // double register, bits 11-9: dr0, dr2, ... dr14
    fvN, fvM,           // vector fv0/4/8/12, bits 11-10 and 9-8
    xmtrx,              // the back-bank matrix (defines no bits)
};

// Instruction properties, for the analysis passes and the lifter.
enum Flag : uint32_t {
    kBranch = 1u << 0,      // changes the PC
    kConditional = 1u << 1, // ... only if T says so
    kDelayed = 1u << 2,     // the next instruction (the delay slot) runs first
    kCall = 1u << 3,        // sets PR to the return address
    kReturn = 1u << 4,      // rts, rte
    kIndirect = 1u << 5,    // target comes from a register
    kLoad = 1u << 6,
    kStore = 1u << 7,
    kFloat = 1u << 8,       // reads or writes FPU registers, FPUL or FPSCR
    kPrivileged = 1u << 9,  // privileged mode only (SR.MD = 1)
    kPcRelative = 1u << 10, // reads or forms an address from the PC
    kFpPr = 1u << 11,       // behaviour depends on FPSCR.PR
    kFpSz = 1u << 12,       // behaviour depends on FPSCR.SZ
    kFpscrWrite = 1u << 13, // changes FPSCR (so PR, SZ or FR may change)
    kCache = 1u << 14,      // cache and store-queue control
    kSetsT = 1u << 15,      // writes the T bit
};

struct Spec {
    Op op = Op::invalid;
    std::string_view name;
    uint16_t match = 0;  // fixed bits
    uint16_t mask = 0;   // which bits `match` covers
    Opnd operands[3] = {};
    uint32_t flags = 0;
};

// The bits an operand kind occupies, as a mask.
uint16_t field_mask(Opnd o);

struct Insn {
    uint16_t word = 0;
    Op op = Op::invalid;

    bool valid() const { return op != Op::invalid; }

    // Fields. The caller knows which ones the instruction has.
    unsigned n() const { return (word >> 8) & 15; }
    unsigned m() const { return (word >> 4) & 15; }
    int32_t imm_s() const { return static_cast<int8_t>(word & 0xFF); }
    uint32_t imm_u() const { return word & 0xFF; }
    uint32_t disp4() const { return word & 15; }
    uint32_t disp8() const { return word & 0xFF; }
    unsigned bank() const { return (word >> 4) & 7; }

    // Target of bt, bf, bt/s, bf/s, bra, bsr at address pc.
    uint32_t branch_target(uint32_t pc) const;
    // Address mov.w/mov.l @(disp,PC) read, and mova forms, at address pc.
    uint32_t pc_relative(uint32_t pc) const;
};

// The spec of every op (index by Op), and all of them in table order.
const Spec& spec(Op op);
std::span<const Spec> specs();

Insn decode(uint16_t word);

// Text of a valid instruction at address pc, objdump style:
// "mov.l 0x8c010020,r1", "mov.l r3,@(8,r15)", "bt/s 0x8c0100a4", "fmov @r4+,fr2".
std::string disasm(const Insn& insn, uint32_t pc);

}  // namespace qilin::sh4
