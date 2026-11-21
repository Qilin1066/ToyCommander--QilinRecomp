#include "ref.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "decoder.hpp"
#include "guest.hpp"

namespace qilin::ref {

namespace {

using sh4::Insn;
using sh4::Op;

constexpr uint32_t kFR = 1u << 21, kSZ = 1u << 20, kPR = 1u << 19;

struct Stopped {
    Outcome o;
};

struct Cpu {
    State& s;
    const Machine& m;

    [[noreturn]] void stop(Outcome::Kind k, uint32_t pc, uint32_t value) { throw Stopped{{k, pc, value, {}}}; }
    [[noreturn]] void error(uint32_t pc, std::string why) { throw Stopped{{Outcome::error, pc, 0, std::move(why)}}; }

    // ---- memory, byte by byte, little-endian ----
    uint32_t pc_now = 0;
    uint8_t* at(uint32_t a, uint32_t n) {
        if (a < m.base || uint64_t(a) + n > uint64_t(m.base) + m.size || !m.allowed(a, n)) stop(Outcome::memory, pc_now, a);
        return m.mem + (a - m.base);
    }
    uint32_t load(uint32_t a, unsigned n) {
        const uint8_t* p = at(a, n);
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= uint32_t(p[i]) << (8 * i);
        return v;
    }
    void store(uint32_t a, uint32_t v, unsigned n) {
        uint8_t* p = at(a, n);
        for (unsigned i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
    static uint32_t sext8(uint32_t v) { return (v & 0x80u) ? v | 0xFFFFFF00u : v & 0xFFu; }
    static uint32_t sext16(uint32_t v) { return (v & 0x8000u) ? v | 0xFFFF0000u : v & 0xFFFFu; }

    // ---- the FPU: values as bit patterns, arithmetic in double, the flush by hand ----
    static float F(uint32_t v) { return std::bit_cast<float>(v); }
    static uint32_t B(float f) { return std::bit_cast<uint32_t>(f); }
    // A denormal is zero (FPSCR.DN = 1), keeping its sign.
    static uint32_t flush32(uint32_t v) { return (v & 0x7F800000u) == 0 ? v & 0x80000000u : v; }
    static uint64_t flush64(uint64_t v) { return (v & 0x7FF0000000000000ull) == 0 ? v & 0x8000000000000000ull : v; }
    static double in32(uint32_t v) { return static_cast<double>(F(flush32(v))); }
    static uint32_t out32(double d) { return flush32(B(static_cast<float>(d))); }
    double dr(unsigned n) { return std::bit_cast<double>(flush64(uint64_t(s.fr[n & 14]) << 32 | s.fr[(n & 14) + 1])); }
    void set_dr(unsigned n, double d) {
        const uint64_t v = flush64(std::bit_cast<uint64_t>(d));
        s.fr[n & 14] = static_cast<uint32_t>(v >> 32), s.fr[(n & 14) + 1] = static_cast<uint32_t>(v);
    }
    bool pr() const { return s.fpscr & kPR; }
    bool sz() const { return s.fpscr & kSZ; }
    // The pair a 64-bit fmov names: DRn, or XDn when the field's low bit is set.
    uint32_t* pair(unsigned field) { return (field & 1u) ? &s.xf[field & 14u] : &s.fr[field & 14u]; }
    void swap_banks() {
        for (unsigned k = 0; k < 16; ++k) std::swap(s.fr[k], s.xf[k]);
    }

    uint32_t sr_now() const { return s.sr | s.t | s.s << 1 | s.q << 8 | s.m << 9; }

    // ---- div1, from its definition: Q and M say whether Rm is subtracted
    // (they agree) or added; the carry or borrow out of bit 31 combines with
    // the bit shifted out into the new Q.
    void div1(unsigned n, unsigned mm) {
        const uint32_t old_q = s.q, out = s.r[n] >> 31;
        const uint32_t shifted = (s.r[n] << 1) | s.t;
        const uint64_t wide = old_q == s.m ? uint64_t(shifted) - s.r[mm] : uint64_t(shifted) + s.r[mm];
        s.r[n] = static_cast<uint32_t>(wide);
        const uint32_t carry = static_cast<uint32_t>(wide >> 32) & 1u;  // borrow or carry out
        // Q is the 33-bit result's sign: the shifted-out bit, flipped by a carry
        // out of an addition or a borrow out of a subtraction.
        s.q = out ^ carry;
        if (s.m) s.q ^= 1u;  // M = 1: the divisor is taken as negative
        s.t = s.q == s.m;
    }

    // ---- one instruction that isn't a branch ----
    void exec(uint32_t pc, const Insn& i) {
        pc_now = pc;
        const unsigned n = i.n(), mm = i.m();
        uint32_t* r = s.r;
        const auto disp4 = i.disp4(), disp8 = i.disp8();
        switch (i.op) {
        case Op::nop: break;
        case Op::mov_imm: r[n] = static_cast<uint32_t>(i.imm_s()); break;
        case Op::movw_pc: r[n] = sext16(load(pc + 4 + disp8 * 2, 2)); break;
        case Op::movl_pc: r[n] = load((pc & ~3u) + 4 + disp8 * 4, 4); break;
        case Op::mova: r[0] = (pc & ~3u) + 4 + disp8 * 4; break;
        case Op::mov: r[n] = r[mm]; break;
        case Op::movb_st: store(r[n], r[mm], 1); break;
        case Op::movw_st: store(r[n], r[mm], 2); break;
        case Op::movl_st: store(r[n], r[mm], 4); break;
        case Op::movb_ld: r[n] = sext8(load(r[mm], 1)); break;
        case Op::movw_ld: r[n] = sext16(load(r[mm], 2)); break;
        case Op::movl_ld: r[n] = load(r[mm], 4); break;
        case Op::movb_st_dec: { const uint32_t v = r[mm]; store(r[n] - 1, v, 1); r[n] -= 1; break; }
        case Op::movw_st_dec: { const uint32_t v = r[mm]; store(r[n] - 2, v, 2); r[n] -= 2; break; }
        case Op::movl_st_dec: { const uint32_t v = r[mm]; store(r[n] - 4, v, 4); r[n] -= 4; break; }
        case Op::movb_ld_inc: { const uint32_t v = sext8(load(r[mm], 1)); r[mm] += 1; r[n] = v; break; }
        case Op::movw_ld_inc: { const uint32_t v = sext16(load(r[mm], 2)); r[mm] += 2; r[n] = v; break; }
        case Op::movl_ld_inc: { const uint32_t v = load(r[mm], 4); r[mm] += 4; r[n] = v; break; }
        case Op::movb_st_disp: store(r[mm] + disp4, r[0], 1); break;
        case Op::movw_st_disp: store(r[mm] + disp4 * 2, r[0], 2); break;
        case Op::movl_st_disp: store(r[n] + disp4 * 4, r[mm], 4); break;
        case Op::movb_ld_disp: r[0] = sext8(load(r[mm] + disp4, 1)); break;
        case Op::movw_ld_disp: r[0] = sext16(load(r[mm] + disp4 * 2, 2)); break;
        case Op::movl_ld_disp: r[n] = load(r[mm] + disp4 * 4, 4); break;
        case Op::movb_st_r0: store(r[0] + r[n], r[mm], 1); break;
        case Op::movw_st_r0: store(r[0] + r[n], r[mm], 2); break;
        case Op::movl_st_r0: store(r[0] + r[n], r[mm], 4); break;
        case Op::movb_ld_r0: r[n] = sext8(load(r[0] + r[mm], 1)); break;
        case Op::movw_ld_r0: r[n] = sext16(load(r[0] + r[mm], 2)); break;
        case Op::movl_ld_r0: r[n] = load(r[0] + r[mm], 4); break;
        case Op::movb_st_gbr: store(s.gbr + disp8, r[0], 1); break;
        case Op::movw_st_gbr: store(s.gbr + disp8 * 2, r[0], 2); break;
        case Op::movl_st_gbr: store(s.gbr + disp8 * 4, r[0], 4); break;
        case Op::movb_ld_gbr: r[0] = sext8(load(s.gbr + disp8, 1)); break;
        case Op::movw_ld_gbr: r[0] = sext16(load(s.gbr + disp8 * 2, 2)); break;
        case Op::movl_ld_gbr: r[0] = load(s.gbr + disp8 * 4, 4); break;
        case Op::movt: r[n] = s.t; break;
        case Op::swap_b: r[n] = (r[mm] & 0xFFFF0000u) | (r[mm] & 0xFFu) << 8 | (r[mm] >> 8 & 0xFFu); break;
        case Op::swap_w: r[n] = r[mm] << 16 | r[mm] >> 16; break;
        case Op::xtrct: r[n] = r[n] >> 16 | r[mm] << 16; break;
        case Op::add: r[n] += r[mm]; break;
        case Op::add_imm: r[n] += static_cast<uint32_t>(i.imm_s()); break;
        case Op::addc: { const uint64_t v = uint64_t(r[n]) + r[mm] + s.t; r[n] = static_cast<uint32_t>(v); s.t = static_cast<uint32_t>(v >> 32); break; }
        case Op::addv: {
            const int64_t v = int64_t(int32_t(r[n])) + int32_t(r[mm]);
            r[n] = static_cast<uint32_t>(v), s.t = v > INT32_MAX || v < INT32_MIN;
            break;
        }
        case Op::cmp_eq_imm: s.t = r[0] == static_cast<uint32_t>(i.imm_s()); break;
        case Op::cmp_eq: s.t = r[n] == r[mm]; break;
        case Op::cmp_hs: s.t = r[n] >= r[mm]; break;
        case Op::cmp_ge: s.t = int32_t(r[n]) >= int32_t(r[mm]); break;
        case Op::cmp_hi: s.t = r[n] > r[mm]; break;
        case Op::cmp_gt: s.t = int32_t(r[n]) > int32_t(r[mm]); break;
        case Op::cmp_pl: s.t = int32_t(r[n]) > 0; break;
        case Op::cmp_pz: s.t = int32_t(r[n]) >= 0; break;
        case Op::cmp_str: {
            bool any = false;
            for (int k = 0; k < 4; ++k) any |= ((r[n] >> (8 * k)) & 0xFF) == ((r[mm] >> (8 * k)) & 0xFF);
            s.t = any;
            break;
        }
        case Op::div1: div1(n, mm); break;
        case Op::div0s: s.q = r[n] >> 31, s.m = r[mm] >> 31, s.t = s.q != s.m; break;
        case Op::div0u: s.q = s.m = s.t = 0; break;
        case Op::dmuls_l: {
            const uint64_t p = static_cast<uint64_t>(int64_t(int32_t(r[n])) * int32_t(r[mm]));
            s.mach = static_cast<uint32_t>(p >> 32), s.macl = static_cast<uint32_t>(p);
            break;
        }
        case Op::dmulu_l: { const uint64_t p = uint64_t(r[n]) * r[mm]; s.mach = static_cast<uint32_t>(p >> 32), s.macl = static_cast<uint32_t>(p); break; }
        case Op::dt: r[n] -= 1, s.t = r[n] == 0; break;
        case Op::exts_b: r[n] = sext8(r[mm]); break;
        case Op::exts_w: r[n] = sext16(r[mm]); break;
        case Op::extu_b: r[n] = r[mm] & 0xFFu; break;
        case Op::extu_w: r[n] = r[mm] & 0xFFFFu; break;
        case Op::mac_l:
        case Op::mac_w: {
            const bool l = i.op == Op::mac_l;
            const unsigned size = l ? 4 : 2;
            const int64_t a = l ? int32_t(load(r[n], 4)) : int16_t(load(r[n], 2));
            const int64_t b = l ? int32_t(load(r[mm], 4)) : int16_t(load(r[mm], 2));
            r[n] += size, r[mm] += size;
            if (s.s) stop(Outcome::unsupported, pc, 0);
            const uint64_t acc = (uint64_t(s.mach) << 32 | s.macl) + static_cast<uint64_t>(a * b);
            s.mach = static_cast<uint32_t>(acc >> 32), s.macl = static_cast<uint32_t>(acc);
            break;
        }
        case Op::mul_l: s.macl = r[n] * r[mm]; break;
        case Op::muls_w: s.macl = static_cast<uint32_t>(int32_t(int16_t(r[n])) * int32_t(int16_t(r[mm]))); break;
        case Op::mulu_w: s.macl = (r[n] & 0xFFFFu) * (r[mm] & 0xFFFFu); break;
        case Op::neg: r[n] = 0u - r[mm]; break;
        case Op::negc: { const uint64_t v = 0ull - r[mm] - s.t; r[n] = static_cast<uint32_t>(v); s.t = static_cast<uint32_t>(v >> 63); break; }
        case Op::sub: r[n] -= r[mm]; break;
        case Op::subc: { const uint64_t v = uint64_t(r[n]) - r[mm] - s.t; r[n] = static_cast<uint32_t>(v); s.t = static_cast<uint32_t>(v >> 63); break; }
        case Op::subv: {
            const int64_t v = int64_t(int32_t(r[n])) - int32_t(r[mm]);
            r[n] = static_cast<uint32_t>(v), s.t = v > INT32_MAX || v < INT32_MIN;
            break;
        }
        case Op::and_: r[n] &= r[mm]; break;
        case Op::and_imm: r[0] &= i.imm_u(); break;
        case Op::and_b: { const uint32_t a = s.gbr + r[0]; store(a, load(a, 1) & i.imm_u(), 1); break; }
        case Op::not_: r[n] = ~r[mm]; break;
        case Op::or_: r[n] |= r[mm]; break;
        case Op::or_imm: r[0] |= i.imm_u(); break;
        case Op::or_b: { const uint32_t a = s.gbr + r[0]; store(a, load(a, 1) | i.imm_u(), 1); break; }
        case Op::tas_b: { const uint32_t v = load(r[n], 1); s.t = v == 0; store(r[n], v | 0x80u, 1); break; }
        case Op::tst: s.t = (r[n] & r[mm]) == 0; break;
        case Op::tst_imm: s.t = (r[0] & i.imm_u()) == 0; break;
        case Op::tst_b: s.t = (load(s.gbr + r[0], 1) & i.imm_u()) == 0; break;
        case Op::xor_: r[n] ^= r[mm]; break;
        case Op::xor_imm: r[0] ^= i.imm_u(); break;
        case Op::xor_b: { const uint32_t a = s.gbr + r[0]; store(a, load(a, 1) ^ i.imm_u(), 1); break; }
        case Op::rotl: s.t = r[n] >> 31, r[n] = r[n] << 1 | s.t; break;
        case Op::rotr: s.t = r[n] & 1u, r[n] = r[n] >> 1 | s.t << 31; break;
        case Op::rotcl: { const uint32_t out = r[n] >> 31; r[n] = r[n] << 1 | s.t; s.t = out; break; }
        case Op::rotcr: { const uint32_t out = r[n] & 1u; r[n] = r[n] >> 1 | s.t << 31; s.t = out; break; }
        case Op::shad:
        case Op::shld: {
            // The shift count is Rm's low 5 bits, to the left when Rm is
            // positive or zero; to the right by 32 minus them when negative
            // (a count of 32 when they are zero).
            const int32_t sh = int32_t(r[mm]);
            const unsigned k = r[mm] & 31u;
            if (sh >= 0) r[n] <<= k;
            else if (i.op == Op::shad) r[n] = k == 0 ? (int32_t(r[n]) < 0 ? 0xFFFFFFFFu : 0u) : static_cast<uint32_t>(int32_t(r[n]) >> (32 - k));
            else r[n] = k == 0 ? 0u : r[n] >> (32 - k);
            break;
        }
        case Op::shal:
        case Op::shll: s.t = r[n] >> 31, r[n] <<= 1; break;
        case Op::shar: s.t = r[n] & 1u, r[n] = static_cast<uint32_t>(int32_t(r[n]) >> 1); break;
        case Op::shlr: s.t = r[n] & 1u, r[n] >>= 1; break;
        case Op::shll2: r[n] <<= 2; break;
        case Op::shlr2: r[n] >>= 2; break;
        case Op::shll8: r[n] <<= 8; break;
        case Op::shlr8: r[n] >>= 8; break;
        case Op::shll16: r[n] <<= 16; break;
        case Op::shlr16: r[n] >>= 16; break;
        case Op::clrmac: s.mach = s.macl = 0; break;
        case Op::clrs: s.s = 0; break;
        case Op::sets: s.s = 1; break;
        case Op::clrt: s.t = 0; break;
        case Op::sett: s.t = 1; break;
        case Op::ldc_gbr: s.gbr = r[n]; break;
        case Op::ldcl_gbr: { const uint32_t v = load(r[n], 4); r[n] += 4; s.gbr = v; break; }
        case Op::stc_gbr: r[n] = s.gbr; break;
        case Op::stcl_gbr: store(r[n] - 4, s.gbr, 4), r[n] -= 4; break;
        case Op::lds_mach: s.mach = r[n]; break;
        case Op::lds_macl: s.macl = r[n]; break;
        case Op::lds_pr: s.pr = r[n]; break;
        case Op::ldsl_mach: { const uint32_t v = load(r[n], 4); r[n] += 4; s.mach = v; break; }
        case Op::ldsl_macl: { const uint32_t v = load(r[n], 4); r[n] += 4; s.macl = v; break; }
        case Op::ldsl_pr: { const uint32_t v = load(r[n], 4); r[n] += 4; s.pr = v; break; }
        case Op::sts_mach: r[n] = s.mach; break;
        case Op::sts_macl: r[n] = s.macl; break;
        case Op::sts_pr: r[n] = s.pr; break;
        case Op::stsl_mach: store(r[n] - 4, s.mach, 4), r[n] -= 4; break;
        case Op::stsl_macl: store(r[n] - 4, s.macl, 4), r[n] -= 4; break;
        case Op::stsl_pr: store(r[n] - 4, s.pr, 4), r[n] -= 4; break;
        // SR: the system bits (MD, RB, BL, FD) can't change under lifted code; the rest can.
        case Op::stc_sr: r[n] = sr_now(); break;
        case Op::stcl_sr: store(r[n] - 4, sr_now(), 4), r[n] -= 4; break;
        case Op::ldc_sr:
        case Op::ldcl_sr: {
            const uint32_t v = i.op == Op::ldc_sr ? r[n] : load(r[n], 4);
            if ((v & 0x70008000u) != 0x40000000u) stop(Outcome::unsupported, pc, v);
            if (i.op == Op::ldcl_sr) r[n] += 4;
            s.sr = v & ~0x303u, s.t = v & 1u, s.s = (v >> 1) & 1u, s.q = (v >> 8) & 1u, s.m = (v >> 9) & 1u;
            break;
        }
        case Op::movca_l: store(r[n], r[0], 4); break;
        case Op::ocbi:
        case Op::ocbp:
        case Op::ocbwb: break;  // caches: nothing to see
        case Op::pref:
            if ((r[n] & 0xFC000000u) == 0xE0000000u) stop(Outcome::unsupported, pc, r[n]);
            break;
        // ---- FPU transfers ----
        case Op::fmov:
            if (sz()) {
                uint32_t *d = pair(n), *src = pair(mm);
                const uint32_t hi = src[0], lo = src[1];
                d[0] = hi, d[1] = lo;
            } else {
                s.fr[n] = s.fr[mm];
            }
            break;
        case Op::fmov_ld:
        case Op::fmov_ld_r0:
        case Op::fmov_ld_inc: {
            const uint32_t a = i.op == Op::fmov_ld_r0 ? r[0] + r[mm] : r[mm];
            if (sz()) {
                const uint32_t hi = load(a, 4), lo = load(a + 4, 4);
                uint32_t* d = pair(n);
                d[0] = hi, d[1] = lo;
            } else {
                s.fr[n] = load(a, 4);
            }
            if (i.op == Op::fmov_ld_inc) r[mm] += sz() ? 8 : 4;
            break;
        }
        case Op::fmov_st:
        case Op::fmov_st_r0:
        case Op::fmov_st_dec: {
            const unsigned size = sz() ? 8 : 4;
            const uint32_t a = i.op == Op::fmov_st_r0 ? r[0] + r[n] : i.op == Op::fmov_st_dec ? r[n] - size : r[n];
            if (sz()) {
                const uint32_t* src = pair(mm);
                store(a, src[0], 4), store(a + 4, src[1], 4);
            } else {
                store(a, s.fr[mm], 4);
            }
            if (i.op == Op::fmov_st_dec) r[n] -= size;
            break;
        }
        case Op::flds: s.fpul = s.fr[n]; break;
        case Op::fsts: s.fr[n] = s.fpul; break;
        case Op::lds_fpul: s.fpul = r[n]; break;
        case Op::ldsl_fpul: { const uint32_t v = load(r[n], 4); r[n] += 4; s.fpul = v; break; }
        case Op::sts_fpul: r[n] = s.fpul; break;
        case Op::stsl_fpul: store(r[n] - 4, s.fpul, 4), r[n] -= 4; break;
        case Op::sts_fpscr: r[n] = s.fpscr; break;
        case Op::stsl_fpscr: store(r[n] - 4, s.fpscr, 4), r[n] -= 4; break;
        case Op::lds_fpscr:
        case Op::ldsl_fpscr: {
            const uint32_t v = i.op == Op::lds_fpscr ? r[n] : load(r[n], 4);
            if (v & 0x00000F80u) stop(Outcome::unsupported, pc, v);  // exception enables: not modelled
            if (i.op == Op::ldsl_fpscr) r[n] += 4;
            if ((v ^ s.fpscr) & kFR) swap_banks();
            s.fpscr = v;
            break;
        }
        case Op::fschg: s.fpscr ^= kSZ; break;
        case Op::frchg: s.fpscr ^= kFR, swap_banks(); break;
        // ---- FPU arithmetic ----
        case Op::fadd: pr() ? set_dr(n, dr(n) + dr(mm)) : void(s.fr[n] = out32(in32(s.fr[n]) + in32(s.fr[mm]))); break;
        case Op::fsub: pr() ? set_dr(n, dr(n) - dr(mm)) : void(s.fr[n] = out32(in32(s.fr[n]) - in32(s.fr[mm]))); break;
        case Op::fmul: pr() ? set_dr(n, dr(n) * dr(mm)) : void(s.fr[n] = out32(in32(s.fr[n]) * in32(s.fr[mm]))); break;
        case Op::fdiv: pr() ? set_dr(n, dr(n) / dr(mm)) : void(s.fr[n] = out32(in32(s.fr[n]) / in32(s.fr[mm]))); break;
        case Op::fsqrt: pr() ? set_dr(n, std::sqrt(dr(n))) : void(s.fr[n] = out32(std::sqrt(in32(s.fr[n])))); break;
        case Op::fabs: s.fr[pr() ? n & 14 : n] &= 0x7FFFFFFFu; break;
        case Op::fneg: s.fr[pr() ? n & 14 : n] ^= 0x80000000u; break;
        case Op::fcmp_eq: s.t = pr() ? dr(n) == dr(mm) : in32(s.fr[n]) == in32(s.fr[mm]); break;
        case Op::fcmp_gt: s.t = pr() ? dr(n) > dr(mm) : in32(s.fr[n]) > in32(s.fr[mm]); break;
        case Op::fldi0:
        case Op::fldi1:
            if (pr()) error(pc, "fldi in double precision");
            s.fr[n] = i.op == Op::fldi0 ? 0u : 0x3F800000u;
            break;
        case Op::float_:
            if (pr()) set_dr(n, static_cast<double>(int32_t(s.fpul)));
            else s.fr[n] = out32(static_cast<double>(int32_t(s.fpul)));
            break;
        case Op::ftrc: {
            const double v = pr() ? dr(n) : in32(s.fr[n]);
            if (!(v > -2147483649.0 && v < 2147483648.0)) stop(Outcome::unsupported, pc, 0);
            s.fpul = static_cast<uint32_t>(static_cast<int32_t>(std::trunc(v)));
            break;
        }
        case Op::fcnvsd:
            if (!pr()) error(pc, "fcnvsd in single precision");
            set_dr(n, in32(s.fpul));
            break;
        case Op::fcnvds:
            if (!pr()) error(pc, "fcnvds in single precision");
            s.fpul = out32(dr(n));
            break;
        // ---- the port's own definitions (guest.hpp): the wiring is what is checked ----
        case Op::fmac:
            if (pr()) error(pc, "fmac in double precision");
            s.fr[n] = B(rt::fmac(F(s.fr[0]), F(s.fr[mm]), F(s.fr[n])));
            break;
        case Op::fsrra: s.fr[n] = B(rt::fsrra(F(flush32(s.fr[n])))); break;
        case Op::fipr: {
            const unsigned vn = n & 12u, vm = (n & 3u) * 4u;
            s.fr[vn + 3] = B(rt::fipr(F(s.fr[vm]), F(s.fr[vm + 1]), F(s.fr[vm + 2]), F(s.fr[vm + 3]), F(s.fr[vn]), F(s.fr[vn + 1]), F(s.fr[vn + 2]),
                                      F(s.fr[vn + 3])));
            break;
        }
        case Op::ftrv: {
            const unsigned vn = n & 12u;
            float xf[16];
            for (unsigned k = 0; k < 16; ++k) xf[k] = F(s.xf[k]);
            float v[4];
            for (unsigned k = 0; k < 4; ++k) v[k] = F(s.fr[vn + k]);
            rt::ftrv(xf, v[0], v[1], v[2], v[3]);
            for (unsigned k = 0; k < 4; ++k) s.fr[vn + k] = B(v[k]);
            break;
        }
        case Op::fsca: {
            float sn, cs;
            rt::fsca(s.fpul, sn, cs);
            s.fr[n & 14] = B(sn), s.fr[(n & 14) + 1] = B(cs);
            break;
        }
        default: error(pc, std::string("not in the reference: ") + std::string(sh4::spec(i.op).name));
        }
    }
};

}  // namespace

Outcome run(State& s, const Machine& m, uint32_t entry, unsigned limit) {
    Cpu cpu{s, m};
    struct Frame {
        uint32_t call_pc, back;
    };
    std::vector<Frame> calls;
    uint32_t pc = entry;
    try {
        for (unsigned steps = 0; steps < limit; ++steps) {
            const Insn i = sh4::decode(static_cast<uint16_t>(cpu.load(pc, 2)));
            if (!i.valid()) cpu.error(pc, "an invalid instruction");
            const uint32_t flags = sh4::spec(i.op).flags;
            if (!(flags & sh4::kBranch)) {
                cpu.exec(pc, i);
                pc += 2;
                continue;
            }
            const bool delayed = flags & sh4::kDelayed;
            uint32_t target = 0;
            bool taken = true;
            // What the branch reads, before its slot runs.
            switch (i.op) {
            case Op::bt: case Op::bt_s: taken = s.t != 0; target = i.branch_target(pc); break;
            case Op::bf: case Op::bf_s: taken = s.t == 0; target = i.branch_target(pc); break;
            case Op::bra: case Op::bsr: target = i.branch_target(pc); break;
            case Op::braf: case Op::bsrf: target = pc + 4 + s.r[i.n()]; break;
            case Op::jmp: case Op::jsr: target = s.r[i.n()]; break;
            case Op::rts: target = s.pr; break;
            default: cpu.error(pc, std::string("a branch the reference doesn't know: ") + std::string(sh4::spec(i.op).name));
            }
            if (delayed) {
                const Insn slot = sh4::decode(static_cast<uint16_t>(cpu.load(pc + 2, 2)));
                if (!slot.valid() || (sh4::spec(slot.op).flags & sh4::kBranch)) cpu.error(pc + 2, "a branch in a delay slot");
                cpu.exec(pc + 2, slot);
            }
            const uint32_t next = pc + (delayed ? 4 : 2);
            if (!taken) {
                pc = next;
                continue;
            }
            switch (i.op) {
            case Op::bsr: case Op::bsrf: case Op::jsr:
                s.pr = next;  // whatever is at the target
                if (!m.is_function(target)) cpu.stop(Outcome::bad_jump, pc, target);
                calls.push_back({pc, next});
                pc = target;
                break;
            case Op::braf: case Op::jmp:
                // Indirect: through the function table, as a tail call.
                if (!m.is_function(target)) cpu.stop(Outcome::bad_jump, pc, target);
                pc = target;
                break;
            case Op::rts:
                if (calls.empty()) return {Outcome::returned, pc, target, {}};
                if (target != calls.back().back) cpu.stop(Outcome::bad_return, calls.back().call_pc, target);
                calls.pop_back();
                pc = target;
                break;
            default: pc = target; break;
            }
        }
        cpu.error(pc, "too many instructions");
    } catch (const Stopped& stopped) {
        return stopped.o;
    }
}

}  // namespace qilin::ref
