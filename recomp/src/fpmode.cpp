#include "fpmode.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <tuple>
#include <vector>

namespace qilin {

using sh4::Insn;
using sh4::Op;

KnownBits compose(const KnownBits& b, const KnownBits& base) {
    KnownBits out{b.mask, b.value, 0, 0};
    // Bits that are known in the base settle b's bits that follow it.
    const uint32_t settled = b.smask & base.mask;
    out.mask |= settled;
    out.value |= (base.value ^ b.sinv) & settled;
    // Bits that follow the base's own entry stay symbolic.
    const uint32_t chained = b.smask & base.smask;
    out.smask = chained;
    out.sinv = (b.sinv ^ base.sinv) & chained;
    return out;
}

KnownBits meet(const KnownBits& a, const KnownBits& b) {
    KnownBits out;
    out.mask = a.mask & b.mask & ~(a.value ^ b.value);
    out.value = a.value & out.mask;
    out.smask = a.smask & b.smask & ~(a.sinv ^ b.sinv);
    out.sinv = a.sinv & out.smask;
    return out;
}

namespace {

using fpscr::kTracked;

constexpr KnownBits kDefaultMode{kTracked, fpscr::kDefault, 0, 0};
constexpr KnownBits kEntryMode{0, 0, kTracked, 0};  // every tracked bit as on entry

KnownBits constant(uint32_t v) { return {0xFFFFFFFFu, v, 0, 0}; }
KnownBits tracked(const KnownBits& b) { return {b.mask & kTracked, b.value & kTracked, b.smask & kTracked, b.sinv & kTracked}; }

// Bitwise operations on partly known, partly symbolic values: a bit is known,
// or equal to (or the inverse of) the function's entry bit at the same
// position, or unknown.
KnownBits band(const KnownBits& a, const KnownBits& b) {
    KnownBits out;
    const uint32_t zero = (a.mask & ~a.value) | (b.mask & ~b.value);
    const uint32_t both1 = a.mask & a.value & b.mask & b.value;
    out.mask = zero | both1;
    out.value = both1;
    // One side known 1: the other side's symbol; both the same symbol: it.
    const uint32_t free = ~out.mask;
    const uint32_t from_b = free & a.mask & a.value & b.smask;
    const uint32_t from_a = free & b.mask & b.value & a.smask;
    const uint32_t same = free & a.smask & b.smask & ~(a.sinv ^ b.sinv);
    out.smask = from_a | from_b | same;
    out.sinv = (from_b & b.sinv) | ((from_a | same) & a.sinv);
    return out;
}
KnownBits bnot(const KnownBits& a) { return {a.mask, ~a.value & a.mask, a.smask, ~a.sinv & a.smask}; }
KnownBits bor(const KnownBits& a, const KnownBits& b) { return bnot(band(bnot(a), bnot(b))); }
KnownBits bxor(const KnownBits& a, const KnownBits& b) {
    const uint32_t kk = a.mask & b.mask;    // both known: known
    const uint32_t ss = a.smask & b.smask;  // both symbols: e^x ^ e^y = x^y, known
    const uint32_t ks = a.mask & b.smask;   // one known: the other's symbol, flipped by it
    const uint32_t sk = a.smask & b.mask;
    KnownBits out;
    out.mask = kk | ss;
    out.value = ((a.value ^ b.value) & kk) | ((a.sinv ^ b.sinv) & ss);
    out.smask = ks | sk;
    out.sinv = ((b.sinv ^ a.value) & ks) | ((a.sinv ^ b.value) & sk);
    return out;
}
// Shifts move bits away from their position: their symbols are dropped.
KnownBits shl(const KnownBits& a, unsigned k) { return {a.mask << k | ((1u << k) - 1), a.value << k, 0, 0}; }
KnownBits shr(const KnownBits& a, unsigned k) { return {a.mask >> k | ~(0xFFFFFFFFu >> k), a.value >> k, 0, 0}; }

struct State {
    KnownBits fpscr = kEntryMode;
    std::array<KnownBits, 16> r{};
    // FPSCR values saved with sts.l fpscr,@-rN and not yet restored with
    // lds.l @rN+,fpscr: (rN, value), innermost last.
    std::vector<std::pair<unsigned, KnownBits>> saved;
    bool operator==(const State&) const = default;
};

State meet(const State& a, const State& b) {
    State out;
    out.fpscr = meet(a.fpscr, b.fpscr);
    for (size_t k = 0; k < 16; ++k) out.r[k] = meet(a.r[k], b.r[k]);
    if (a.saved.size() == b.saved.size())
        for (size_t k = 0; k < a.saved.size(); ++k)
            out.saved.emplace_back(a.saved[k].first == b.saved[k].first ? a.saved[k].first : 16u, meet(a.saved[k].second, b.saved[k].second));
    return out;
}

// Does the instruction name floating-point registers (so FR matters)?
[[maybe_unused]] bool uses_fr(const Insn& i) {
    switch (i.op) {
    case Op::frchg: case Op::fschg: case Op::lds_fpscr: case Op::lds_fpul: case Op::ldsl_fpscr: case Op::ldsl_fpul:
    case Op::sts_fpscr: case Op::sts_fpul: case Op::stsl_fpscr: case Op::stsl_fpul:
        return false;
    default: return true;
    }
}

// The mode bits an FPU instruction depends on. FR isn't one: the lifter
// keeps the current bank and the back bank as two arrays and frchg swaps
// them, as the hardware does, so code names "the current fr0-fr15" whatever
// FR is. (Compiled code never cares which bank is current; hand-written
// matrix code switches it, and the engine runs in bank 1 for a while after
// some matrix calls.)
uint32_t needed_bits(const Insn& i) {
    const uint32_t flags = sh4::spec(i.op).flags;
    uint32_t need = 0;
    if (flags & sh4::kFpPr) need |= fpscr::kPR | fpscr::kRM | fpscr::kDN;  // arithmetic rounds
    if (flags & sh4::kFpSz) need |= fpscr::kSZ;
    if (i.op == Op::fschg) need |= fpscr::kSZ;
    return need;
}

// The bits whose value at a join or a call matters.
constexpr uint32_t kMatters = fpscr::kPR | fpscr::kSZ | fpscr::kRM | fpscr::kDN;

// What a call site does, kept for the second phase.
struct Site {
    uint32_t pc = 0;
    enum Kind : uint8_t { call, tail, indirect, ret } kind = call;
    uint32_t target = 0;  // call, tail
    KnownBits mode;       // relative to the function's entry
};

// One function relative to its entry: the mode at every FPU instruction,
// the sites where the convention matters, and the exit.
struct Summary {
    std::map<uint32_t, KnownBits> at;
    std::vector<Site> sites;
    std::optional<KnownBits> exit;
    // Joins where paths arrive with a mode bit that differs between them
    // (relative to the entry): address, the bits lost there, and what met.
    std::vector<std::tuple<uint32_t, uint32_t, std::vector<KnownBits>>> joins;
};

// `exit_of(target, mode)`: the mode a direct call to `target` made in `mode`
// returns in.
template <typename ExitOf>
Summary summarise(const BootFile& boot, const Function& f, const KnownBits& entry, ExitOf&& exit_of) {
    Summary out;
    std::map<uint32_t, State> in;
    State start;
    start.fpscr = entry;
    in[f.entry] = start;
    std::vector<uint32_t> work{f.entry};
    const auto step = [&](uint32_t pc, State& s, bool report) {
        const Insn i = sh4::decode(boot.u16(pc));
        const sh4::Spec& spec = sh4::spec(i.op);
        auto& r = s.r;
        if (report && (spec.flags & sh4::kFloat)) out.at[pc] = tracked(s.fpscr);
        bool handled = true;
        switch (i.op) {
        case Op::fschg: s.fpscr = bxor(s.fpscr, constant(fpscr::kSZ)); break;
        case Op::frchg: s.fpscr = bxor(s.fpscr, constant(fpscr::kFR)); break;
        case Op::sts_fpscr: r[i.n()] = tracked(s.fpscr); break;
        case Op::lds_fpscr: s.fpscr = tracked(r[i.n()]); break;
        case Op::stsl_fpscr:
            s.saved.emplace_back(i.n(), tracked(s.fpscr));
            r[i.n()] = {};
            break;
        case Op::ldsl_fpscr:
            // The restore of a save: the value pushed through the same register.
            if (!s.saved.empty() && s.saved.back().first == i.n()) {
                s.fpscr = s.saved.back().second;
                s.saved.pop_back();
            } else {
                s.fpscr = {};
            }
            r[i.n()] = {};
            break;
        case Op::mov_imm: r[i.n()] = constant(static_cast<uint32_t>(i.imm_s())); break;
        case Op::movl_pc: {
            const uint32_t a = i.pc_relative(pc);
            r[i.n()] = boot.mapped(a, 4) ? constant(boot.u32(a)) : KnownBits{};
            break;
        }
        case Op::movw_pc: {
            const uint32_t a = i.pc_relative(pc);
            r[i.n()] = boot.mapped(a, 2) ? constant(static_cast<uint32_t>(static_cast<int16_t>(boot.u16(a)))) : KnownBits{};
            break;
        }
        case Op::mova: r[0] = constant(i.pc_relative(pc)); break;
        case Op::mov: r[i.n()] = r[i.m()]; break;
        case Op::and_: r[i.n()] = band(r[i.n()], r[i.m()]); break;
        case Op::or_: r[i.n()] = bor(r[i.n()], r[i.m()]); break;
        case Op::xor_: r[i.n()] = bxor(r[i.n()], r[i.m()]); break;
        case Op::and_imm: r[0] = band(r[0], constant(i.imm_u())); break;
        case Op::or_imm: r[0] = bor(r[0], constant(i.imm_u())); break;
        case Op::xor_imm: r[0] = bxor(r[0], constant(i.imm_u())); break;
        case Op::not_: r[i.n()] = bnot(r[i.m()]); break;
        case Op::shll: case Op::shal: r[i.n()] = shl(r[i.n()], 1); break;
        case Op::shll2: r[i.n()] = shl(r[i.n()], 2); break;
        case Op::shll8: r[i.n()] = shl(r[i.n()], 8); break;
        case Op::shll16: r[i.n()] = shl(r[i.n()], 16); break;
        case Op::shlr: r[i.n()] = shr(r[i.n()], 1); break;
        case Op::shlr2: r[i.n()] = shr(r[i.n()], 2); break;
        case Op::shlr8: r[i.n()] = shr(r[i.n()], 8); break;
        case Op::shlr16: r[i.n()] = shr(r[i.n()], 16); break;
        case Op::extu_b: r[i.n()] = band(r[i.m()], constant(0xFFu)); break;
        case Op::extu_w: r[i.n()] = band(r[i.m()], constant(0xFFFFu)); break;
        case Op::add_imm:
            r[i.n()] = r[i.n()].mask == 0xFFFFFFFFu ? constant(r[i.n()].value + static_cast<uint32_t>(i.imm_s())) : KnownBits{};
            break;
        case Op::add:
            r[i.n()] = r[i.n()].mask == 0xFFFFFFFFu && r[i.m()].mask == 0xFFFFFFFFu ? constant(r[i.n()].value + r[i.m()].value) : KnownBits{};
            break;
        default: handled = false; break;
        }
        if (!handled) {
            const uint16_t w = gpr_written(i);
            for (unsigned k = 0; k < 16; ++k)
                if (w & (1u << k)) r[k] = {};
        }
        // Calls, returns and tail calls take effect after the delay slot.
        const bool slot = pc > boot.base() && std::ranges::binary_search(f.insns, pc - 2) &&
                          (sh4::spec(sh4::decode(boot.u16(pc - 2)).op).flags & sh4::kDelayed);
        const uint32_t site = slot ? pc - 2 : pc;
        const Insn branch = slot ? sh4::decode(boot.u16(site)) : i;
        const uint32_t bflags = sh4::spec(branch.op).flags;
        if ((bflags & sh4::kDelayed) && !slot) return;  // the slot hasn't run yet
        const KnownBits mode = tracked(s.fpscr);
        const bool jmp_call = f.call_returns.contains(site);
        const bool jmp_return = std::ranges::binary_search(f.jmp_returns, site);
        if ((bflags & sh4::kCall) || jmp_call) {
            if (const auto it = f.calls.find(site); it != f.calls.end()) {
                if (report) out.sites.push_back({site, Site::call, it->second, mode});
                s.fpscr = exit_of(it->second, mode);
            } else {
                // Checked to be the default in phase 2; the callee keeps it.
                if (report) out.sites.push_back({site, Site::indirect, 0, mode});
            }
            for (unsigned k = 0; k < 8; ++k) r[k] = {};
            return;
        }
        if (!report) return;
        const auto leave = [&](const KnownBits& m) { out.exit = out.exit ? meet(*out.exit, m) : m; };
        if ((bflags & sh4::kReturn) || jmp_return) {
            out.sites.push_back({site, Site::ret, 0, mode});
            leave(mode);
        } else if (const auto it = f.tail_jumps.find(site); it != f.tail_jumps.end()) {
            out.sites.push_back({site, Site::tail, it->second, mode});
            leave(exit_of(it->second, mode));
        } else if ((branch.op == Op::jmp || branch.op == Op::braf) && !f.computed_branches.contains(site) &&
                   std::ranges::none_of(f.switches, [&](const Switch& w) { return w.braf == site; })) {
            // An indirect tail call: like an indirect call, the callee keeps the mode.
            out.sites.push_back({site, Site::indirect, 0, mode});
            leave(mode);
        }
    };
    while (!work.empty()) {
        const uint32_t pc = work.back();
        work.pop_back();
        State s = in.at(pc);
        step(pc, s, false);
        for (uint32_t next : successors(boot, f, pc)) {
            auto [it, fresh] = in.try_emplace(next, s);
            if (!fresh) {
                const State merged = meet(it->second, s);
                if (merged == it->second) continue;
                it->second = merged;
            }
            work.push_back(next);
        }
    }
    std::map<uint32_t, std::vector<KnownBits>> arriving;  // join -> the modes its predecessors leave with
    for (const auto& [pc, state] : in) {
        State s = state;
        step(pc, s, true);
        for (uint32_t next : successors(boot, f, pc)) arriving[next].push_back(tracked(s.fpscr));
    }
    // Where a bit is lost at a join although every predecessor leaves with it
    // known or following the entry, the paths disagree there.
    for (const auto& [pc, modes] : arriving) {
        const KnownBits& here = in.at(pc).fpscr;
        const uint32_t lost = kMatters & ~(here.mask | here.smask);
        if (!lost) continue;
        uint32_t clear = kMatters;  // bits every predecessor has known or symbolic
        for (const KnownBits& m : modes) clear &= m.mask | m.smask;
        if (const uint32_t at_join = lost & clear) out.joins.emplace_back(pc, at_join, modes);
    }
    return out;
}

// The default mode in every bit that matters (the bank doesn't: see needed_bits).
bool is_default(const KnownBits& b) { return b.known(kMatters) && (b.value & kMatters) == (fpscr::kDefault & kMatters); }

std::string bits_text(const KnownBits& b) {
    const auto bit = [&](uint32_t m) {
        if (b.known(m)) return std::string(1, (b.value & m) ? '1' : '0');
        if ((b.smask & m) == m) return std::string((b.sinv & m) ? "~entry" : "entry");
        return std::string("?");
    };
    return std::format("PR={} SZ={} FR={} RM={}{} DN={}", bit(fpscr::kPR), bit(fpscr::kSZ), bit(fpscr::kFR), bit(2), bit(1), bit(fpscr::kDN));
}

}  // namespace

std::map<uint32_t, FpModes> fp_modes(const BootFile& boot, const Cfg& cfg, std::span<const Region> regions) {
    // Functions in a region that is replaced are native code at run time,
    // which keeps the default mode: they aren't analysed, and calls to them
    // return in the default mode.
    const auto replaced = [&](uint32_t a) {
        return std::ranges::any_of(regions, [&](const Region& r) { return r.plan == Plan::replace && a >= r.begin && a < r.end; });
    };
    // A literal-pool word is how SHC makes an ordinary call (mov.l pool,rN ;
    // jsr @rN): it takes the address only if its function doesn't call or
    // tail-call that address itself. Any other word holding a function's
    // address does.
    std::set<uint32_t> taken;
    std::map<uint32_t, uint32_t> literal_owner;
    for (const auto& [e, f] : cfg.functions)
        for (const auto& [a, n] : f.literals)
            if (n == 4) literal_owner.try_emplace(a, e);
    for (uint32_t a = boot.base(); a + 4 <= boot.end(); a += 2) {
        if (cfg.at(a) == Kind::code || cfg.at(a + 2) == Kind::code) continue;
        const uint32_t v = boot.u32(a);
        if (!cfg.functions.contains(v)) continue;
        if (const auto it = literal_owner.find(a); it != literal_owner.end()) {
            const Function& owner = cfg.functions.at(it->second);
            const auto targets = [&](const std::map<uint32_t, uint32_t>& m) {
                return std::ranges::any_of(m, [&](const auto& kv) { return kv.second == v; });
            };
            if (targets(owner.calls) || targets(owner.tail_jumps)) continue;
        }
        taken.insert(v);
    }
    taken.insert(boot.entry());

    // Each function is analysed once per mode it is entered in (a context):
    // address-taken functions and those no analysed code calls in the
    // default mode, others in the modes their callers call them in. A call
    // returns in its callee's exit for that context; one not yet known (and
    // a call to a function with no analysis: replaced, or never returning)
    // keeps the mode. Iterated until no context or exit changes.
    using Context = std::pair<uint32_t, KnownBits>;
    struct ContextLess {
        bool operator()(const Context& a, const Context& b) const {
            return std::tie(a.first, a.second.mask, a.second.value) < std::tie(b.first, b.second.mask, b.second.value);
        }
    };
    std::map<Context, Summary, ContextLess> contexts;
    std::map<Context, std::optional<KnownBits>, ContextLess> exits;
    std::set<uint32_t> called;
    for (const auto& [e, f] : cfg.functions)
        for (const auto& [site, target] : f.calls) called.insert(target);
    for (const auto& [e, f] : cfg.functions)
        for (const auto& [site, target] : f.tail_jumps) called.insert(target);
    for (const auto& [e, f] : cfg.functions)
        if (!replaced(e) && (taken.contains(e) || !called.contains(e))) contexts.try_emplace({e, kDefaultMode});
    std::vector<std::string> global_problems;
    for (int round = 0;; ++round) {
        bool changed = false;
        std::vector<Context> todo;
        for (const auto& [c, s] : contexts) todo.push_back(c);
        for (const Context& c : todo) {
            std::vector<Context> wanted;
            const auto exit_of = [&](uint32_t target, const KnownBits& mode) -> KnownBits {
                if (replaced(target) || !cfg.functions.contains(target)) return mode;
                const KnownBits m{mode.mask & kTracked, mode.value & kTracked, 0, 0};
                wanted.emplace_back(target, m);
                const auto it = exits.find({target, m});
                if (it == exits.end()) return m;   // not known yet: keeps the mode
                return it->second ? *it->second : m;  // never returns: anything after is unreachable
            };
            Summary s = summarise(boot, cfg.functions.at(c.first), c.second, exit_of);
            for (const Context& w : wanted)
                if (contexts.try_emplace(w).second) changed = true;
            auto& slot = exits[c];
            if (slot != s.exit || !exits.contains(c)) changed |= slot != s.exit;
            slot = s.exit;
            contexts[c] = std::move(s);
        }
        if (!changed) break;
        if (round == 40) {
            global_problems.push_back("the FPSCR modes don't converge across the call graph");
            break;
        }
    }

    // The results per function, over its contexts.
    std::map<uint32_t, FpModes> out;
    std::map<uint32_t, std::vector<const Context*>> by_function;
    for (const auto& [c, s] : contexts) by_function[c.first].push_back(&c);
    for (const auto& [e, list] : by_function) {
        FpModes m;
        m.address_taken = taken.contains(e);
        m.contexts = static_cast<uint32_t>(list.size());
        std::set<std::string> problems;
        bool first = true;
        for (const Context* c : list) {
            const Summary& s = contexts.at(*c);
            m.entry = first ? c->second : meet(m.entry, c->second);
            m.per_context.emplace_back(c->second, s.exit);
            if (s.exit) m.exit = m.exit ? meet(*m.exit, *s.exit) : *s.exit;
            for (const auto& [pc, mode] : s.at) {
                auto [it, fresh] = m.at.try_emplace(pc, mode);
                if (!fresh) it->second = meet(it->second, mode);
            }
            for (const Site& site : s.sites) {
                const KnownBits& mode = site.mode;
                if (is_default(mode)) continue;
                switch (site.kind) {
                case Site::indirect:
                    problems.insert(std::format("{:08X}: an indirect call or jump in FPSCR mode {}", site.pc, bits_text(mode)));
                    break;
                case Site::call:
                case Site::tail:
                    if (taken.contains(site.target))
                        problems.insert(std::format("{:08X}: {} fn_{:08X}, whose address is taken, in FPSCR mode {}", site.pc,
                                                    site.kind == Site::call ? "calls" : "tail-calls", site.target, bits_text(mode)));
                    else if (auto [it, fresh] = m.calls.try_emplace(site.target, mode); !fresh)
                        it->second = meet(it->second, mode);
                    break;
                case Site::ret:
                    if (m.address_taken)
                        problems.insert(std::format("{:08X}: returns in FPSCR mode {}, and its address is taken", site.pc, bits_text(mode)));
                    break;
                }
            }
            for (const auto& [pc, lost, modes] : s.joins) {
                std::set<std::string> distinct;
                for (const KnownBits& k : modes) distinct.insert(bits_text(k));
                std::string met;
                for (const std::string& d : distinct) met += (met.empty() ? "" : " | ") + d;
                problems.insert(std::format("{:08X}: paths join in different FPSCR modes: {}", pc, met));
                ++m.joins;
            }
            first = false;
        }
        // Each FPU instruction: proven in every context, or in each context
        // separately (entry-dependent), or not.
        for (const auto& [pc, merged] : m.at) {
            ++m.fpu;
            const Insn i = sh4::decode(boot.u16(pc));
            const uint32_t need = needed_bits(i);
            if (merged.known(need)) continue;
            const bool each = std::ranges::all_of(list, [&](const Context* c) {
                const auto& at = contexts.at(*c).at;
                const auto it = at.find(pc);
                return it == at.end() || it->second.known(need);
            });
            if (each) {
                ++m.entry_dependent;
                continue;
            }
            ++m.unproven;
            problems.insert(std::format("{:08X}: {} needs FPSCR bits that aren't known ({})", pc, sh4::disasm(i, pc), bits_text(merged)));
        }
        m.summary_exit = std::nullopt;
        m.problems.assign(problems.begin(), problems.end());
        out.emplace(e, std::move(m));
    }
    if (!global_problems.empty() && !out.empty())
        out.begin()->second.problems.insert(out.begin()->second.problems.end(), global_problems.begin(), global_problems.end());
    return out;
}

}  // namespace qilin
