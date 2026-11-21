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
enum : uint32_t { kT = 1u << 16, kPr = 1u << 17, kGbr = 1u << 18, kMach = 1u << 19, kMacl = 1u << 20, kS = 1u << 21, kQ = 1u << 22, kM = 1u << 23 };

class Emitter {
public:
    Emitter(const BootFile& boot, const Cfg& cfg, const Function& f) : boot_(boot), cfg_(cfg), f_(f) {}

    LiftResult run() {
        try {
            for (const std::string& p : f_.problems)
                if (p.contains("lands in a delay slot")) throw Fail{f_.entry, "a branch lands in a delay slot"};
            std::map<uint32_t, std::string> body;
            for (size_t k = 0; k < f_.insns.size(); ++k) {
                const uint32_t pc = f_.insns[k];
                if (is_slot(pc)) continue;  // emitted with its branch
                body[pc] = instruction(pc);
            }
            // Every label jumped to must be an instruction emitted on its own.
            for (uint32_t target : labels_)
                if (!body.contains(target))
                    throw Fail{target, std::format("a branch to {:08X}, which isn't an instruction of its own here", target)};
            std::string out = std::format("void {}(rt::Context& c) {{\n", fn_name(f_.entry));
            for (unsigned n = 0; n < 16; ++n)
                if (used_ & (1u << n)) out += std::format("    uint32_t r{} = c.r[{}];\n", n, n);
            for (const auto& [bit, name] : kNamed)
                if (used_ & bit) out += std::format("    uint32_t {} = c.{};\n", name, name);
            if (body.begin()->first != f_.entry) {
                labels_.insert(f_.entry);
                out += std::format("    goto {};\n", label(f_.entry));
            }
            for (const auto& [pc, code] : body) {
                if (labels_.contains(pc)) out += label(pc) + ":\n";
                out += code;
            }
            out += "}\n";
            // The sync points: the context holds every register the function uses.
            std::string store, load;
            for (unsigned n = 0; n < 16; ++n)
                if (used_ & (1u << n)) store += std::format(" c.r[{0}] = r{0};", n), load += std::format(" r{0} = c.r[{0}];", n);
            for (const auto& [bit, name] : kNamed)
                if (used_ & bit) store += std::format(" c.{0} = {0};", name), load += std::format(" {0} = c.{0};", name);
            replace_all(out, "@OUT@", store.empty() ? "" : store.substr(1));
            replace_all(out, "@IN@", load.empty() ? "" : load.substr(1));
            return {true, std::move(out), 0, {}};
        } catch (const Fail& fail) {
            return {false, {}, fail.pc, fail.why};
        }
    }

private:
    static constexpr std::pair<uint32_t, const char*> kNamed[] = {{kT, "t"},       {kPr, "pr"}, {kGbr, "gbr"}, {kMach, "mach"},
                                                                  {kMacl, "macl"}, {kS, "s"},   {kQ, "q"},     {kM, "m"}};

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
        if (spec.flags & sh4::kFloat) throw Fail{pc, std::format("floating point (slice 3): {}", spec.name)};
        if (spec.flags & sh4::kPrivileged) throw Fail{pc, std::format("privileged: {}", text)};
        const unsigned n = i.n(), m = i.m();
        const auto same = [&](const char* what) {
            if (n == m) throw Fail{pc, std::format("{} with the same register twice: {}", what, text)};
        };
        std::string s;
        const auto line = [&](std::string code) { s += "    " + code + "\n"; };
        if ((spec.flags & (sh4::kLoad | sh4::kStore | sh4::kCache)) && !(spec.flags & sh4::kPcRelative)) s += mem(pc);
        const std::string imm_s = hex(static_cast<uint32_t>(i.imm_s()));
        const std::string imm_u = hex(i.imm_u());
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
        case Op::movb_st_dec: same("a pre-decrement store"); line(std::format("{} -= 1u; rt::st8({}, {});", R(n), R(n), R(m))); break;
        case Op::movw_st_dec: same("a pre-decrement store"); line(std::format("{} -= 2u; rt::st16({}, {});", R(n), R(n), R(m))); break;
        case Op::movl_st_dec: same("a pre-decrement store"); line(std::format("{} -= 4u; rt::st32({}, {});", R(n), R(n), R(m))); break;
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
        case Op::nop: break;
        case Op::ocbi:
        case Op::ocbp:
        case Op::ocbwb: line(std::format("(void){};  // {}: no caches to manage", R(n), spec.name)); break;
        case Op::pref: line(std::format("rt::pref({}, {});", R(n), hex(pc))); break;
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
    uint32_t used_ = 0;
    std::set<uint32_t> labels_;
};

}  // namespace

LiftResult Lifter::lift(const Function& f) const { return Emitter(boot_, cfg_, f).run(); }

}  // namespace qilin
