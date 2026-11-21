#include "lifter.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <vector>

namespace qilin {

using sh4::Insn;
using sh4::Op;

std::string fn_name(uint32_t addr) { return std::format("fn_{:08X}", addr); }

namespace {

std::string hex(uint32_t v) { return std::format("0x{:X}u", v); }
std::string label(uint32_t a) { return std::format("L_{:08X}", a); }

struct Fail {
    uint32_t pc;
    std::string why;
};

// The locals generated code keeps guest registers in. r0-r15 are bits 0-15.
enum : uint32_t {
    kT = 1u << 16, kPr = 1u << 17, kGbr = 1u << 18, kMach = 1u << 19, kMacl = 1u << 20, kS = 1u << 21, kQ = 1u << 22, kM = 1u << 23, kFpul = 1u << 24
};

class Emitter {
public:
    Emitter(const BootFile& boot, const Cfg& cfg, const Function& f, const FpModes* modes) : boot_(boot), cfg_(cfg), f_(f), modes_(modes) {}

    LiftResult run() {
        try {
            std::map<uint32_t, std::string> body;
            for (size_t k = 0; k < f_.insns.size(); ++k) {
                const uint32_t pc = f_.insns[k];
                if (is_slot(pc)) continue;  // emitted with its branch
                body[pc] = instruction(pc);
            }
            // A branch into a delay slot runs the slot's instruction on its own and
            // goes on with the next one (the branch before it isn't taken): a
            // copy of the instruction, under the slot's label, then a jump on.
            // The copies go after the body, where nothing falls into them.
            std::map<uint32_t, std::string> slot_copies;
            for (const uint32_t target : std::set<uint32_t>(labels_)) {
                if (body.contains(target) || !is_slot(target)) continue;
                const Insn si = sh4::decode(boot_.u16(target));
                if (sh4::spec(si.op).flags & (sh4::kBranch | sh4::kPcRelative))
                    throw Fail{target, "a branch into a delay slot that holds a branch or a PC-relative instruction"};
                slot_copies[target] = plain(target, si) + "    " + go(target + 2) + "\n";
            }
            // Every label jumped to must be an instruction emitted on its own.
            for (uint32_t target : labels_)
                if (!body.contains(target) && !slot_copies.contains(target))
                    throw Fail{target, std::format("a branch to {:08X}, which isn't an instruction of its own here", target)};
            std::string out = std::format("void {}(rt::Context& c) {{\n", fn_name(f_.entry));
            for (unsigned n = 0; n < 16; ++n)
                if (used_ & (1u << n)) out += std::format("    uint32_t r{} = c.r[{}];\n", n, n);
            for (const auto& [bit, name] : kNamed)
                if (used_ & bit) out += std::format("    uint32_t {} = c.{};\n", name, name);
            for (unsigned n = 0; n < 16; ++n)
                if (fused_ & (1u << n)) out += std::format("    float f{} = c.fr[{}];\n", n, n);
            if (body.begin()->first != f_.entry) {
                labels_.insert(f_.entry);
                out += std::format("    goto {};\n", label(f_.entry));
            }
            for (const auto& [pc, code] : body) {
                if (labels_.contains(pc)) out += label(pc) + ":\n";
                out += code;
            }
            for (const auto& [pc, code] : slot_copies) out += std::format("{}:  // the delay slot at {:08X}, branched to\n{}", label(pc), pc, code);
            out += "}\n";
            // The sync points: the context holds every register the function uses.
            std::string store, load, fstore, fload;
            for (unsigned n = 0; n < 16; ++n)
                if (used_ & (1u << n)) store += std::format(" c.r[{0}] = r{0};", n), load += std::format(" r{0} = c.r[{0}];", n);
            for (const auto& [bit, name] : kNamed)
                if (used_ & bit) store += std::format(" c.{0} = {0};", name), load += std::format(" {0} = c.{0};", name);
            for (unsigned n = 0; n < 16; ++n)
                if (fused_ & (1u << n)) fstore += std::format(" c.fr[{0}] = f{0};", n), fload += std::format(" f{0} = c.fr[{0}];", n);
            store += fstore, load += fload;
            replace_all(out, "@OUT@", store.empty() ? "" : store.substr(1));
            replace_all(out, "@IN@", load.empty() ? "" : load.substr(1));
            replace_all(out, "@FOUT@", fstore.empty() ? "" : fstore.substr(1));
            replace_all(out, "@FIN@", fload.empty() ? "" : fload.substr(1));
            return {true, std::move(out), 0, {}};
        } catch (const Fail& fail) {
            return {false, {}, fail.pc, fail.why};
        }
    }

private:
    static constexpr std::pair<uint32_t, const char*> kNamed[] = {{kT, "t"},       {kPr, "pr"}, {kGbr, "gbr"}, {kMach, "mach"},
                                                                  {kMacl, "macl"}, {kS, "s"},   {kQ, "q"},     {kM, "m"},
                                                                  {kFpul, "fpul"}};

    static void replace_all(std::string& s, std::string_view from, std::string_view to) {
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    }

    std::string R(unsigned n) {
        used_ |= 1u << n;
        return std::format("r{}", n);
    }
    std::string use(uint32_t bit, const char* name) {
        used_ |= bit;
        return name;
    }
    std::string T() { return use(kT, "t"); }
    std::string PR() { return use(kPr, "pr"); }
    std::string GBR() { return use(kGbr, "gbr"); }
    std::string MACH() { return use(kMach, "mach"); }
    std::string MACL() { return use(kMacl, "macl"); }

    std::string go(uint32_t target) {
        labels_.insert(target);
        return std::format("goto {};", label(target));
    }

    bool in_function(uint32_t a) const { return std::ranges::binary_search(f_.insns, a); }

    // A delay slot: the instruction after a delayed branch of this function.
    bool is_slot(uint32_t pc) const {
        if (pc <= boot_.base() || !in_function(pc - 2)) return false;
        const Insn b = sh4::decode(boot_.u16(pc - 2));
        return b.valid() && (sh4::spec(b.op).flags & sh4::kDelayed);
    }

    // A direct call or tail call: the function's C++ name, or through the
    // table if there is no function there (the runtime may have native code
    // for it).
    std::string call_to(uint32_t target, uint32_t pc) const {
        if (cfg_.functions.contains(target)) return std::format("{}(c);", fn_name(target));
        return std::format("rt::call(c, {}, {});", hex(target), hex(pc));
    }

    // mov.x Rm,@-Rn: with Rm and Rn the same register, the value stored is
    // the register before the decrement (as the operand is read before the
    // address is updated; a Dreamcast emulator agrees, tools/dcref).
    template <typename Line>
    void pre_dec(unsigned n, unsigned m, const char* size, const char* store, const Line& line) {
        if (n == m) line(std::format("{{ const uint32_t v = {}; {} -= {}; rt::{}({}, v); }}", R(m), R(n), size, store, R(n)));
        else line(std::format("{} -= {}; rt::{}({}, {});", R(n), size, store, R(n), R(m)));
    }

    // SR as the guest reads it: the context's, with T, S, Q and M from their locals.
    std::string sr_value() {
        return std::format("(c.sr | {} | ({} << 1) | ({} << 8) | ({} << 9))", T(), use(kS, "s"), use(kQ, "q"), use(kM, "m"));
    }
    std::string set_sr(const std::string& v, uint32_t pc) {
        return std::format("{{ const uint32_t sr = {}; rt::set_sr(c, sr, {}); {} = sr & 1u; {} = (sr >> 1) & 1u; {} = (sr >> 8) & 1u; {} = (sr >> 9) & 1u; }}",
                           v, hex(pc), T(), use(kS, "s"), use(kQ, "q"), use(kM, "m"));
    }

    std::string mem(uint32_t pc) const { return std::format("    QILIN_PC({});\n", hex(pc)); }

    // ---- one instruction that isn't a branch ----
    std::string instruction(uint32_t pc) {
        const Insn i = sh4::decode(boot_.u16(pc));
        const sh4::Spec& spec = sh4::spec(i.op);
        if (spec.flags & sh4::kBranch) return branch(pc, i);
        return std::string(plain(pc, i));
    }

    std::string plain(uint32_t pc, const Insn& i) {
        const sh4::Spec& spec = sh4::spec(i.op);
        const std::string text = sh4::disasm(i, pc);
        // SR, but for T, S, Q and M, is held in the context: lifted code
        // reads it, and writes its interrupt mask (guest.hpp's set_sr).
        const bool sr_op = i.op == Op::stc_sr || i.op == Op::ldc_sr || i.op == Op::stcl_sr || i.op == Op::ldcl_sr;
        if ((spec.flags & sh4::kPrivileged) && !sr_op) throw Fail{pc, std::format("privileged: {}", text)};
        if (spec.flags & sh4::kFloat) return fpu(pc, i);
        const unsigned n = i.n(), m = i.m();
        const auto same = [&](const char* what) {
            if (n == m) throw Fail{pc, std::format("{} with the same register twice: {}", what, text)};
        };
        std::string s;
        const auto line = [&](std::string code) { s += "    " + code + "\n"; };
        if ((spec.flags & (sh4::kLoad | sh4::kStore | sh4::kCache)) && !(spec.flags & sh4::kPcRelative)) s += mem(pc);
        const std::string imm_s = hex(static_cast<uint32_t>(i.imm_s()));
        const std::string imm_u = hex(i.imm_u());
        // One register as both operands: what the expression would compute is
        // known, and writing it out would compare or assign a variable with itself.
        if (n == m) switch (i.op) {
            case Op::mov: case Op::and_: case Op::or_: line(std::format("(void){};  // {}: no change", R(n), text)); return s;
            case Op::xor_: line(std::format("{} = 0u;", R(n))); return s;
            case Op::cmp_eq: case Op::cmp_hs: case Op::cmp_ge: line(std::format("{} = 1u;  // {}", T(), text)); return s;
            case Op::cmp_hi: case Op::cmp_gt: line(std::format("{} = 0u;  // {}", T(), text)); return s;
            default: break;
            }
        switch (i.op) {
        // ---- data transfer ----
        case Op::mov_imm: line(std::format("{} = {};", R(n), imm_s)); break;
        case Op::movw_pc: line(std::format("{} = {};", R(n), hex(static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(i.pc_relative(pc))))))); break;
        case Op::movl_pc: line(std::format("{} = {};", R(n), hex(boot_.u32(i.pc_relative(pc))))); break;
        case Op::mova: line(std::format("{} = {};", R(0), hex(i.pc_relative(pc)))); break;
        case Op::mov: line(std::format("{} = {};", R(n), R(m))); break;
        case Op::movb_st: line(std::format("rt::st8({}, {});", R(n), R(m))); break;
        case Op::movw_st: line(std::format("rt::st16({}, {});", R(n), R(m))); break;
        case Op::movl_st: line(std::format("rt::st32({}, {});", R(n), R(m))); break;
        case Op::movb_ld: line(std::format("{} = rt::ld8s({});", R(n), R(m))); break;
        case Op::movw_ld: line(std::format("{} = rt::ld16s({});", R(n), R(m))); break;
        case Op::movl_ld: line(std::format("{} = rt::ld32({});", R(n), R(m))); break;
        case Op::movb_st_dec: pre_dec(n, m, "1u", "st8", line); break;
        case Op::movw_st_dec: pre_dec(n, m, "2u", "st16", line); break;
        case Op::movl_st_dec: pre_dec(n, m, "4u", "st32", line); break;
        case Op::movb_ld_inc: same("a post-increment load"); line(std::format("{} = rt::ld8s({}); {} += 1u;", R(n), R(m), R(m))); break;
        case Op::movw_ld_inc: same("a post-increment load"); line(std::format("{} = rt::ld16s({}); {} += 2u;", R(n), R(m), R(m))); break;
        case Op::movl_ld_inc: same("a post-increment load"); line(std::format("{} = rt::ld32({}); {} += 4u;", R(n), R(m), R(m))); break;
        case Op::movb_st_disp: line(std::format("rt::st8({} + {}, {});", R(m), hex(i.disp4()), R(0))); break;
        case Op::movw_st_disp: line(std::format("rt::st16({} + {}, {});", R(m), hex(i.disp4() * 2), R(0))); break;
        case Op::movl_st_disp: line(std::format("rt::st32({} + {}, {});", R(n), hex(i.disp4() * 4), R(m))); break;
        case Op::movb_ld_disp: line(std::format("{} = rt::ld8s({} + {});", R(0), R(m), hex(i.disp4()))); break;
        case Op::movw_ld_disp: line(std::format("{} = rt::ld16s({} + {});", R(0), R(m), hex(i.disp4() * 2))); break;
        case Op::movl_ld_disp: line(std::format("{} = rt::ld32({} + {});", R(n), R(m), hex(i.disp4() * 4))); break;
        case Op::movb_st_r0: line(std::format("rt::st8({} + {}, {});", R(0), R(n), R(m))); break;
        case Op::movw_st_r0: line(std::format("rt::st16({} + {}, {});", R(0), R(n), R(m))); break;
        case Op::movl_st_r0: line(std::format("rt::st32({} + {}, {});", R(0), R(n), R(m))); break;
        case Op::movb_ld_r0: line(std::format("{} = rt::ld8s({} + {});", R(n), R(0), R(m))); break;
        case Op::movw_ld_r0: line(std::format("{} = rt::ld16s({} + {});", R(n), R(0), R(m))); break;
        case Op::movl_ld_r0: line(std::format("{} = rt::ld32({} + {});", R(n), R(0), R(m))); break;
        case Op::movb_st_gbr: line(std::format("rt::st8({} + {}, {});", GBR(), hex(i.disp8()), R(0))); break;
        case Op::movw_st_gbr: line(std::format("rt::st16({} + {}, {});", GBR(), hex(i.disp8() * 2), R(0))); break;
        case Op::movl_st_gbr: line(std::format("rt::st32({} + {}, {});", GBR(), hex(i.disp8() * 4), R(0))); break;
        case Op::movb_ld_gbr: line(std::format("{} = rt::ld8s({} + {});", R(0), GBR(), hex(i.disp8()))); break;
        case Op::movw_ld_gbr: line(std::format("{} = rt::ld16s({} + {});", R(0), GBR(), hex(i.disp8() * 2))); break;
        case Op::movl_ld_gbr: line(std::format("{} = rt::ld32({} + {});", R(0), GBR(), hex(i.disp8() * 4))); break;
        case Op::movt: line(std::format("{} = {};", R(n), T())); break;
        case Op::swap_b: line(std::format("{} = ({} & 0xFFFF0000u) | (({} & 0xFFu) << 8) | (({} >> 8) & 0xFFu);", R(n), R(m), R(m), R(m))); break;
        case Op::swap_w: line(std::format("{} = ({} << 16) | ({} >> 16);", R(n), R(m), R(m))); break;
        case Op::xtrct: line(std::format("{} = ({} >> 16) | ({} << 16);", R(n), R(n), R(m))); break;
        // ---- arithmetic ----
        case Op::add: line(std::format("{} += {};", R(n), R(m))); break;
        case Op::add_imm: line(std::format("{} += {};", R(n), imm_s)); break;
        case Op::addc: line(std::format("rt::addc({}, {}, {});", R(n), R(m), T())); break;
        case Op::addv: line(std::format("rt::addv({}, {}, {});", R(n), R(m), T())); break;
        case Op::cmp_eq_imm: line(std::format("{} = {} == {};", T(), R(0), imm_s)); break;
        case Op::cmp_eq: line(std::format("{} = {} == {};", T(), R(n), R(m))); break;
        case Op::cmp_hs: line(std::format("{} = {} >= {};", T(), R(n), R(m))); break;
        case Op::cmp_ge: line(std::format("{} = static_cast<int32_t>({}) >= static_cast<int32_t>({});", T(), R(n), R(m))); break;
        case Op::cmp_hi: line(std::format("{} = {} > {};", T(), R(n), R(m))); break;
        case Op::cmp_gt: line(std::format("{} = static_cast<int32_t>({}) > static_cast<int32_t>({});", T(), R(n), R(m))); break;
        case Op::cmp_pl: line(std::format("{} = static_cast<int32_t>({}) > 0;", T(), R(n))); break;
        case Op::cmp_pz: line(std::format("{} = static_cast<int32_t>({}) >= 0;", T(), R(n))); break;
        case Op::cmp_str: line(std::format("{} = rt::cmpstr({}, {});", T(), R(n), R(m))); break;
        case Op::div1: line(std::format("rt::div1({}, {}, {}, {}, {});", R(n), R(m), use(kQ, "q"), use(kM, "m"), T())); break;
        case Op::div0s: line(std::format("{} = {} >> 31; {} = {} >> 31; {} = {} ^ {};", use(kQ, "q"), R(n), use(kM, "m"), R(m), T(), use(kQ, "q"), use(kM, "m"))); break;
        case Op::div0u: line(std::format("{} = 0u; {} = 0u; {} = 0u;", use(kQ, "q"), use(kM, "m"), T())); break;
        case Op::dmuls_l:
            line(std::format("{{ const int64_t p = int64_t(static_cast<int32_t>({})) * static_cast<int32_t>({}); {} = static_cast<uint32_t>(uint64_t(p) >> 32); {} = static_cast<uint32_t>(p); }}",
                             R(n), R(m), MACH(), MACL()));
            break;
        case Op::dmulu_l:
            line(std::format("{{ const uint64_t p = uint64_t({}) * {}; {} = static_cast<uint32_t>(p >> 32); {} = static_cast<uint32_t>(p); }}", R(n), R(m), MACH(), MACL()));
            break;
        case Op::dt: line(std::format("{} -= 1u; {} = {} == 0u;", R(n), T(), R(n))); break;
        case Op::exts_b: line(std::format("{} = static_cast<uint32_t>(static_cast<int8_t>({}));", R(n), R(m))); break;
        case Op::exts_w: line(std::format("{} = static_cast<uint32_t>(static_cast<int16_t>({}));", R(n), R(m))); break;
        case Op::extu_b: line(std::format("{} = {} & 0xFFu;", R(n), R(m))); break;
        case Op::extu_w: line(std::format("{} = {} & 0xFFFFu;", R(n), R(m))); break;
        case Op::mac_l:
            same("mac.l");
            line(std::format("{{ const int64_t p = int64_t(static_cast<int32_t>(rt::ld32({}))) * static_cast<int32_t>(rt::ld32({})); {} += 4u; {} += 4u; "
                             "rt::mac({}, {}, p, {}, {}); }}",
                             R(n), R(m), R(n), R(m), MACH(), MACL(), use(kS, "s"), hex(pc)));
            break;
        case Op::mac_w:
            same("mac.w");
            line(std::format("{{ const int64_t p = int64_t(static_cast<int32_t>(rt::ld16s({}))) * static_cast<int32_t>(rt::ld16s({})); {} += 2u; {} += 2u; "
                             "rt::mac({}, {}, p, {}, {}); }}",
                             R(n), R(m), R(n), R(m), MACH(), MACL(), use(kS, "s"), hex(pc)));
            break;
        case Op::mul_l: line(std::format("{} = {} * {};", MACL(), R(n), R(m))); break;
        case Op::muls_w:
            line(std::format("{} = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({})) * static_cast<int16_t>({}));", MACL(), R(n), R(m)));
            break;
        case Op::mulu_w: line(std::format("{} = ({} & 0xFFFFu) * ({} & 0xFFFFu);", MACL(), R(n), R(m))); break;
        case Op::neg: line(std::format("{} = 0u - {};", R(n), R(m))); break;
        case Op::negc: line(std::format("rt::negc({}, {}, {});", R(n), R(m), T())); break;
        case Op::sub: line(std::format("{} -= {};", R(n), R(m))); break;
        case Op::subc: line(std::format("rt::subc({}, {}, {});", R(n), R(m), T())); break;
        case Op::subv: line(std::format("rt::subv({}, {}, {});", R(n), R(m), T())); break;
        // ---- logic ----
        case Op::and_: line(std::format("{} &= {};", R(n), R(m))); break;
        case Op::and_imm: line(std::format("{} &= {};", R(0), imm_u)); break;
        case Op::and_b: line(std::format("{{ const uint32_t a = {} + {}; rt::st8(a, rt::ld8(a) & {}); }}", GBR(), R(0), imm_u)); break;
        case Op::not_: line(std::format("{} = ~{};", R(n), R(m))); break;
        case Op::or_: line(std::format("{} |= {};", R(n), R(m))); break;
        case Op::or_imm: line(std::format("{} |= {};", R(0), imm_u)); break;
        case Op::or_b: line(std::format("{{ const uint32_t a = {} + {}; rt::st8(a, rt::ld8(a) | {}); }}", GBR(), R(0), imm_u)); break;
        case Op::tas_b: line(std::format("{} = rt::tasb({});", T(), R(n))); break;
        case Op::tst: line(std::format("{} = ({} & {}) == 0u;", T(), R(n), R(m))); break;
        case Op::tst_imm: line(std::format("{} = ({} & {}) == 0u;", T(), R(0), imm_u)); break;
        case Op::tst_b: line(std::format("{} = (rt::ld8({} + {}) & {}) == 0u;", T(), GBR(), R(0), imm_u)); break;
        case Op::xor_: line(std::format("{} ^= {};", R(n), R(m))); break;
        case Op::xor_imm: line(std::format("{} ^= {};", R(0), imm_u)); break;
        case Op::xor_b: line(std::format("{{ const uint32_t a = {} + {}; rt::st8(a, rt::ld8(a) ^ {}); }}", GBR(), R(0), imm_u)); break;
        // ---- shift ----
        case Op::rotl: line(std::format("{} = {} >> 31; {} = ({} << 1) | {};", T(), R(n), R(n), R(n), T())); break;
        case Op::rotr: line(std::format("{} = {} & 1u; {} = ({} >> 1) | ({} << 31);", T(), R(n), R(n), R(n), T())); break;
        case Op::rotcl: line(std::format("{{ const uint32_t out = {} >> 31; {} = ({} << 1) | {}; {} = out; }}", R(n), R(n), R(n), T(), T())); break;
        case Op::rotcr: line(std::format("{{ const uint32_t out = {} & 1u; {} = ({} >> 1) | ({} << 31); {} = out; }}", R(n), R(n), R(n), T(), T())); break;
        case Op::shad: line(std::format("{} = rt::shad({}, {});", R(n), R(n), R(m))); break;
        case Op::shld: line(std::format("{} = rt::shld({}, {});", R(n), R(n), R(m))); break;
        case Op::shal:
        case Op::shll: line(std::format("{} = {} >> 31; {} <<= 1;", T(), R(n), R(n))); break;
        case Op::shar: line(std::format("{} = {} & 1u; {} = static_cast<uint32_t>(static_cast<int32_t>({}) >> 1);", T(), R(n), R(n), R(n))); break;
        case Op::shlr: line(std::format("{} = {} & 1u; {} >>= 1;", T(), R(n), R(n))); break;
        case Op::shll2: line(std::format("{} <<= 2;", R(n))); break;
        case Op::shlr2: line(std::format("{} >>= 2;", R(n))); break;
        case Op::shll8: line(std::format("{} <<= 8;", R(n))); break;
        case Op::shlr8: line(std::format("{} >>= 8;", R(n))); break;
        case Op::shll16: line(std::format("{} <<= 16;", R(n))); break;
        case Op::shlr16: line(std::format("{} >>= 16;", R(n))); break;
        // ---- system ----
        case Op::clrmac: line(std::format("{} = 0u; {} = 0u;", MACH(), MACL())); break;
        case Op::clrs: line(std::format("{} = 0u;", use(kS, "s"))); break;
        case Op::sets: line(std::format("{} = 1u;", use(kS, "s"))); break;
        case Op::clrt: line(std::format("{} = 0u;", T())); break;
        case Op::sett: line(std::format("{} = 1u;", T())); break;
        case Op::ldc_gbr: line(std::format("{} = {};", GBR(), R(n))); break;
        case Op::ldcl_gbr: line(std::format("{} = rt::ld32({}); {} += 4u;", GBR(), R(n), R(n))); break;
        case Op::stc_gbr: line(std::format("{} = {};", R(n), GBR())); break;
        case Op::stcl_gbr: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), GBR())); break;
        case Op::lds_mach: line(std::format("{} = {};", MACH(), R(n))); break;
        case Op::lds_macl: line(std::format("{} = {};", MACL(), R(n))); break;
        case Op::lds_pr: line(std::format("{} = {};", PR(), R(n))); break;
        case Op::ldsl_mach: line(std::format("{} = rt::ld32({}); {} += 4u;", MACH(), R(n), R(n))); break;
        case Op::ldsl_macl: line(std::format("{} = rt::ld32({}); {} += 4u;", MACL(), R(n), R(n))); break;
        case Op::ldsl_pr: line(std::format("{} = rt::ld32({}); {} += 4u;", PR(), R(n), R(n))); break;
        case Op::sts_mach: line(std::format("{} = {};", R(n), MACH())); break;
        case Op::sts_macl: line(std::format("{} = {};", R(n), MACL())); break;
        case Op::sts_pr: line(std::format("{} = {};", R(n), PR())); break;
        case Op::stsl_mach: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), MACH())); break;
        case Op::stsl_macl: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), MACL())); break;
        case Op::stsl_pr: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), PR())); break;
        case Op::movca_l: line(std::format("rt::st32({}, {});", R(n), R(0))); break;
        case Op::stc_sr: line(std::format("{} = {};", R(n), sr_value())); break;
        case Op::stcl_sr: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), sr_value())); break;
        case Op::ldc_sr: line(set_sr(R(n), pc)); break;
        case Op::ldcl_sr: line(std::format("{{ const uint32_t v = rt::ld32({}); {} += 4u; {} }}", R(n), R(n), set_sr("v", pc))); break;
        case Op::nop: break;
        case Op::ocbi:
        case Op::ocbp:
        case Op::ocbwb: line(std::format("(void){};  // {}: no caches to manage", R(n), spec.name)); break;
        case Op::pref: line(std::format("rt::pref({}, {});", R(n), hex(pc))); break;
        default: throw Fail{pc, std::format("not lifted: {}", text)};
        }
        return s;
    }

    // ---- the FPU ----
    std::string F(unsigned n) {
        fused_ |= 1u << n;
        return std::format("f{}", n);
    }
    std::string FPUL() { return use(kFpul, "fpul"); }
    // A 64-bit register of fmov with SZ = 1: DRn (this bank's pair) or, with
    // the field's low bit set, XDn (the back bank's).
    std::pair<std::string, std::string> pair64(unsigned field) {
        const unsigned k = field & 14u;
        if (field & 1u) return {std::format("c.xf[{}]", k), std::format("c.xf[{}]", k + 1)};
        return {F(k), F(k + 1)};
    }
    std::string D(unsigned n, uint32_t pc) {
        if (n & 1u) throw Fail{pc, "a double-precision operation on an odd register"};
        return std::format("rt::dbl({}, {})", F(n), F(n + 1));
    }

    std::string fpu(uint32_t pc, const Insn& i) {
        const sh4::Spec& spec = sh4::spec(i.op);
        const std::string text = sh4::disasm(i, pc);
        const unsigned n = i.n(), m = i.m();
        // The mode the instruction runs in, as fpmode proved it.
        bool pr = false, sz = false;
        if (spec.flags & (sh4::kFpPr | sh4::kFpSz)) {
            if (!modes_ || !modes_->at.contains(pc)) throw Fail{pc, std::format("FPU mode not proven: {}", text)};
            const KnownBits& k = modes_->at.at(pc);
            if ((spec.flags & sh4::kFpPr) && !k.known(fpscr::kPR | fpscr::kRM | fpscr::kDN))
                throw Fail{pc, std::format("FPU mode not proven: {}", text)};
            if ((spec.flags & sh4::kFpSz) && !k.known(fpscr::kSZ)) throw Fail{pc, std::format("FPU mode not proven: {}", text)};
            if (spec.flags & sh4::kFpPr) {
                if (k.value & fpscr::kRM) throw Fail{pc, std::format("rounding towards zero: {}", text)};
                if (!(k.value & fpscr::kDN)) throw Fail{pc, std::format("denormals not flushed (DN = 0): {}", text)};
                pr = k.value & fpscr::kPR;
            }
            sz = k.value & fpscr::kSZ;
        }
        std::string s;
        const auto line = [&](std::string code) { s += "    " + code + "\n"; };
        if (spec.flags & (sh4::kLoad | sh4::kStore)) s += mem(pc);
        // Binary arithmetic: FRn = FRn op FRm, or DRn = DRn op DRm.
        const auto arith = [&](const char* op) {
            if (pr) line(std::format("rt::set_dbl({}, {}, {} {} {});", F(n & 14), F((n & 14) + 1), D(n, pc), op, D(m, pc)));
            else line(std::format("{} = {} {} {};", F(n), F(n), op, F(m)));
        };
        // A 64-bit fmov between memory and a pair: FRn at the lower address.
        const auto load64 = [&](unsigned field, const std::string& a) {
            const auto [hi, lo] = pair64(field);
            line(std::format("{{ const uint32_t a = {}; {} = rt::ldf(a); {} = rt::ldf(a + 4u); }}", a, hi, lo));
        };
        const auto store64 = [&](unsigned field, const std::string& a) {
            const auto [hi, lo] = pair64(field);
            line(std::format("{{ const uint32_t a = {}; rt::stf(a, {}); rt::stf(a + 4u, {}); }}", a, hi, lo));
        };
        switch (i.op) {
        // ---- transfers ----
        case Op::fmov:
            if (sz) {
                const auto [dh, dl] = pair64(n);
                const auto [sh, sl] = pair64(m);
                line(std::format("{{ const float hi = {}, lo = {}; {} = hi; {} = lo; }}", sh, sl, dh, dl));
            } else if (n != m) {
                line(std::format("{} = {};", F(n), F(m)));
            }
            break;
        case Op::fmov_ld: sz ? load64(n, R(m)) : line(std::format("{} = rt::ldf({});", F(n), R(m))); break;
        case Op::fmov_ld_r0: sz ? load64(n, R(0) + " + " + R(m)) : line(std::format("{} = rt::ldf({} + {});", F(n), R(0), R(m))); break;
        case Op::fmov_ld_inc:
            sz ? load64(n, R(m)) : line(std::format("{} = rt::ldf({});", F(n), R(m)));
            line(std::format("{} += {};", R(m), sz ? "8u" : "4u"));
            break;
        case Op::fmov_st: sz ? store64(m, R(n)) : line(std::format("rt::stf({}, {});", R(n), F(m))); break;
        case Op::fmov_st_r0: sz ? store64(m, R(0) + " + " + R(n)) : line(std::format("rt::stf({} + {}, {});", R(0), R(n), F(m))); break;
        case Op::fmov_st_dec:
            line(std::format("{} -= {};", R(n), sz ? "8u" : "4u"));
            sz ? store64(m, R(n)) : line(std::format("rt::stf({}, {});", R(n), F(m)));
            break;
        case Op::flds: line(std::format("{} = rt::bits({});", FPUL(), F(n))); break;
        case Op::fsts: line(std::format("{} = rt::flt({});", F(n), FPUL())); break;
        case Op::lds_fpul: line(std::format("{} = {};", FPUL(), R(n))); break;
        case Op::ldsl_fpul: line(std::format("{} = rt::ld32({}); {} += 4u;", FPUL(), R(n), R(n))); break;
        case Op::sts_fpul: line(std::format("{} = {};", R(n), FPUL())); break;
        case Op::stsl_fpul: line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), FPUL())); break;
        // ---- FPSCR ----
        case Op::sts_fpscr: line(std::format("{} = c.fpscr;", R(n))); break;
        case Op::stsl_fpscr: line(std::format("{} -= 4u; rt::st32({}, c.fpscr);", R(n), R(n))); break;
        case Op::lds_fpscr: line(std::format("@FOUT@ rt::set_fpscr(c, {}, {}); @FIN@", R(n), hex(pc))); break;
        case Op::ldsl_fpscr: line(std::format("@FOUT@ rt::set_fpscr(c, rt::ld32({}), {}); {} += 4u; @FIN@", R(n), hex(pc), R(n))); break;
        case Op::fschg: line("c.fpscr ^= rt::kFpscrSZ;"); break;
        case Op::frchg: line("@FOUT@ rt::frchg(c); @FIN@"); break;
        // ---- arithmetic ----
        case Op::fadd: arith("+"); break;
        case Op::fsub: arith("-"); break;
        case Op::fmul: arith("*"); break;
        case Op::fdiv: arith("/"); break;
        case Op::fsqrt:
            if (pr) line(std::format("rt::set_dbl({}, {}, std::sqrt({}));", F(n & 14), F((n & 14) + 1), D(n, pc)));
            else line(std::format("{} = std::sqrt({});", F(n), F(n)));
            break;
        case Op::fabs: line(std::format("{} = rt::fabs_bits({});", F(pr ? n & 14 : n), F(pr ? n & 14 : n))); break;
        case Op::fneg: line(std::format("{} = rt::fneg_bits({});", F(pr ? n & 14 : n), F(pr ? n & 14 : n))); break;
        case Op::fcmp_eq:
            line(pr ? std::format("{} = {} == {};", T(), D(n, pc), D(m, pc)) : std::format("{} = {} == {};", T(), F(n), F(m)));
            break;
        case Op::fcmp_gt:
            line(pr ? std::format("{} = {} > {};", T(), D(n, pc), D(m, pc)) : std::format("{} = {} > {};", T(), F(n), F(m)));
            break;
        case Op::fldi0:
        case Op::fldi1:
            if (pr) throw Fail{pc, std::format("{} in double precision", spec.name)};
            line(std::format("{} = {};", F(n), i.op == Op::fldi0 ? "0.0f" : "1.0f"));
            break;
        case Op::float_:
            if (pr) line(std::format("rt::set_dbl({}, {}, static_cast<double>(static_cast<int32_t>({})));", F(n & 14), F((n & 14) + 1), FPUL()));
            else line(std::format("{} = static_cast<float>(static_cast<int32_t>({}));", F(n), FPUL()));
            break;
        case Op::ftrc: line(std::format("{} = rt::ftrc({}, {});", FPUL(), pr ? D(n, pc) : F(n), hex(pc))); break;
        case Op::fcnvsd:
            if (!pr) throw Fail{pc, "fcnvsd in single precision"};
            line(std::format("rt::set_dbl({}, {}, static_cast<double>(rt::flt({})));", F(n & 14), F((n & 14) + 1), FPUL()));
            break;
        case Op::fcnvds:
            if (!pr) throw Fail{pc, "fcnvds in single precision"};
            line(std::format("{} = rt::bits(static_cast<float>({}));", FPUL(), D(n, pc)));
            break;
        // ---- the approximate instructions: definitions of our own (guest.hpp) ----
        case Op::fmac:
            if (pr) throw Fail{pc, "fmac in double precision"};
            line(std::format("{} = rt::fmac({}, {}, {});", F(n), F(0), F(m), F(n)));
            break;
        case Op::fsrra:
            if (pr) throw Fail{pc, "fsrra in double precision"};
            line(std::format("{} = rt::fsrra({});", F(n), F(n)));
            break;
        case Op::fipr: {
            if (pr) throw Fail{pc, "fipr in double precision"};
            const unsigned vn = n & 12u, vm = (n & 3u) * 4u;
            line(std::format("{} = rt::fipr({}, {}, {}, {}, {}, {}, {}, {});", F(vn + 3), F(vm), F(vm + 1), F(vm + 2), F(vm + 3), F(vn), F(vn + 1),
                             F(vn + 2), F(vn + 3)));
            break;
        }
        case Op::ftrv: {
            if (pr) throw Fail{pc, "ftrv in double precision"};
            const unsigned vn = n & 12u;
            line(std::format("rt::ftrv(c.xf, {}, {}, {}, {});", F(vn), F(vn + 1), F(vn + 2), F(vn + 3)));
            break;
        }
        case Op::fsca:
            if (pr) throw Fail{pc, "fsca in double precision"};
            line(std::format("rt::fsca({}, {}, {});", FPUL(), F(n & 14u), F((n & 14u) + 1)));
            break;
        default: throw Fail{pc, std::format("not lifted: {}", text)};
        }
        return s;
    }

    // ---- a branch, with its delay slot ----
    // A delay slot. Its instruction runs after the branch has read its
    // operands, which the emitted code does by reading them first. Where the
    // branch itself reads or sets PR (rts, the calls, a call by loaded PR),
    // a slot that touches PR would depend on an ordering no clean source here
    // pins down: those stop. (A tail call that restores PR in its slot,
    // `jmp @r2 ; lds.l @r15+,pr`, is ordinary: jmp doesn't use PR.)
    std::string slot(uint32_t branch_pc, bool branch_uses_pr) {
        const uint32_t pc = branch_pc + 2;
        const Insn i = sh4::decode(boot_.u16(pc));
        const sh4::Spec& spec = sh4::spec(i.op);
        if (spec.flags & sh4::kPcRelative) throw Fail{pc, "a PC-relative instruction in a delay slot"};
        if (branch_uses_pr) switch (i.op) {
            case Op::lds_pr: case Op::ldsl_pr: case Op::sts_pr: case Op::stsl_pr:
                throw Fail{pc, "PR read or written in the delay slot of a branch that uses PR"};
            default: break;
            }
        return plain(pc, i);
    }

    std::string branch(uint32_t pc, const Insn& i) {
        const sh4::Spec& spec = sh4::spec(i.op);
        if (spec.flags & sh4::kPrivileged) throw Fail{pc, std::format("privileged: {}", sh4::disasm(i, pc))};
        const bool delayed = spec.flags & sh4::kDelayed;
        const uint32_t next = pc + (delayed ? 4 : 2);
        const auto tail = f_.tail_jumps.find(pc);
        std::string s = "    {\n";
        const auto line = [&](std::string code) { s += "        " + code + "\n"; };
        const bool uses_pr = (spec.flags & sh4::kCall) || i.op == Op::rts || f_.call_returns.contains(pc);
        const auto with_slot = [&] {
            if (delayed) s += indent(slot(pc, uses_pr));
        };
        const auto leave_to = [&](uint32_t target) {  // a tail call
            line("@OUT@");
            line(call_to(target, pc));
            line("return;");
        };
        const auto jump_to = [&](uint32_t target) {
            if (tail != f_.tail_jumps.end()) leave_to(tail->second);
            else line(go(target));
        };
        switch (i.op) {
        case Op::bt:
        case Op::bf: {
            const std::string cond = i.op == Op::bt ? std::format("{} != 0u", T()) : std::format("{} == 0u", T());
            if (tail != f_.tail_jumps.end()) {
                line(std::format("if ({}) {{", cond));
                leave_to(tail->second);
                line("}");
            } else {
                line(std::format("if ({}) {}", cond, go(i.branch_target(pc))));
            }
            break;
        }
        case Op::bt_s:
        case Op::bf_s: {
            line(std::format("const bool taken = {} {} 0u;", T(), i.op == Op::bt_s ? "!=" : "=="));
            with_slot();
            if (tail != f_.tail_jumps.end()) {
                line("if (taken) {");
                leave_to(tail->second);
                line("}");
            } else {
                line(std::format("if (taken) {}", go(i.branch_target(pc))));
            }
            break;
        }
        case Op::bra:
            with_slot();
            jump_to(i.branch_target(pc));
            break;
        case Op::braf: {
            line(std::format("const uint32_t off = {};", R(i.n())));
            with_slot();
            if (const auto it = f_.computed_branches.find(pc); it != f_.computed_branches.end()) {
                line(go(it->second));
                break;
            }
            const auto sw = std::ranges::find(f_.switches, pc, &Switch::braf);
            if (sw != f_.switches.end()) {
                // Keyed on the offset the table gave, whatever the index was.
                std::map<uint32_t, uint32_t> cases;
                for (uint32_t t : sw->targets) cases[t - next] = t;
                line("switch (off) {");
                for (const auto& [o, t] : cases) line(std::format("case {}: {}", hex(o), go(t)));
                line(std::format("default: rt::bad_jump({}, {} + off);", hex(pc), hex(next)));
                line("}");
                break;
            }
            line("@OUT@");
            line(std::format("rt::call(c, {} + off, {});", hex(next), hex(pc)));
            line("return;");
            break;
        }
        case Op::bsr:
        case Op::bsrf:
        case Op::jsr: {
            const auto direct = f_.calls.find(pc);
            if (direct == f_.calls.end()) {
                if (i.op == Op::jsr) line(std::format("const uint32_t target = {};", R(i.n())));
                else if (i.op == Op::bsrf) line(std::format("const uint32_t target = {} + {};", hex(next), R(i.n())));
                else throw Fail{pc, "a bsr without a target"};
            }
            with_slot();
            line(std::format("{} = {};", PR(), hex(next)));
            line("@OUT@");
            line(direct != f_.calls.end() ? call_to(direct->second, pc) : std::format("rt::call(c, target, {});", hex(pc)));
            line("@IN@");
            line(std::format("if (c.ret != {}) rt::bad_return({}, c.ret);", hex(next), hex(pc)));
            break;
        }
        case Op::jmp: {
            const bool to_return = std::ranges::binary_search(f_.jmp_returns, pc);
            const auto back = f_.call_returns.find(pc);
            const auto direct = f_.calls.find(pc);
            if (tail == f_.tail_jumps.end() && !(back != f_.call_returns.end() && direct != f_.calls.end()))
                line(std::format("const uint32_t target = {};", R(i.n())));
            with_slot();
            if (to_return) {
                line("c.ret = target;");
                line("@OUT@");
                line("return;");
            } else if (back != f_.call_returns.end()) {
                // A call made by loading PR by hand: it returns where PR points.
                line("@OUT@");
                line(direct != f_.calls.end() ? call_to(direct->second, pc) : std::format("rt::call(c, target, {});", hex(pc)));
                line("@IN@");
                line(std::format("if (c.ret != {}) rt::bad_return({}, c.ret);", hex(back->second), hex(pc)));
                line(go(back->second));
            } else if (tail != f_.tail_jumps.end()) {
                leave_to(tail->second);
            } else {
                line("@OUT@");
                line(std::format("rt::call(c, target, {});", hex(pc)));
                line("return;");
            }
            break;
        }
        case Op::rts:
            line(std::format("const uint32_t target = {};", PR()));
            with_slot();
            line("c.ret = target;");
            line("@OUT@");
            line("return;");
            break;
        default: throw Fail{pc, std::format("not lifted: {}", sh4::disasm(i, pc))};
        }
        (void)next;
        return s + "    }\n";
    }

    static std::string indent(const std::string& code) {
        std::string out;
        size_t at = 0;
        while (at < code.size()) {
            const size_t end = code.find('\n', at);
            out += "    " + code.substr(at, end - at + 1);
            at = end + 1;
        }
        return out;
    }

    const BootFile& boot_;
    const Cfg& cfg_;
    const Function& f_;
    const FpModes* modes_;
    uint32_t used_ = 0;
    uint32_t fused_ = 0;  // fr0-fr15 of the current bank
    std::set<uint32_t> labels_;
};

}  // namespace

LiftResult Lifter::lift(const Function& f) const {
    const auto m = modes_.find(f.entry);
    return Emitter(boot_, cfg_, f, m == modes_.end() ? nullptr : &m->second).run();
}

}  // namespace qilin
