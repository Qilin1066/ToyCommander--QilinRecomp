#include "cfg.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <set>
#include <stdexcept>

namespace qilin {

using sh4::Insn;
using sh4::Op;
using sh4::Opnd;
using sh4::Spec;

uint16_t gpr_written(const Insn& insn) {
    const Spec& s = sh4::spec(insn.op);
    uint16_t w = 0;
    Opnd last = Opnd::none;
    for (Opnd o : s.operands) {
        if (o == Opnd::none) break;
        last = o;
        if (o == Opnd::at_dec_N || o == Opnd::at_inc_N) w |= uint16_t(1u << insn.n());
        if (o == Opnd::at_inc_M) w |= uint16_t(1u << insn.m());
    }
    switch (insn.op) {
    // The last operand is a register these only read.
    case Op::cmp_eq_imm: case Op::cmp_eq: case Op::cmp_hs: case Op::cmp_ge: case Op::cmp_hi: case Op::cmp_gt:
    case Op::cmp_pl: case Op::cmp_pz: case Op::cmp_str: case Op::tst: case Op::tst_imm: case Op::div0s:
    case Op::mul_l: case Op::muls_w: case Op::mulu_w: case Op::dmuls_l: case Op::dmulu_l:
    case Op::braf: case Op::bsrf:
        return w;
    default: break;
    }
    if (last == Opnd::rN) w |= uint16_t(1u << insn.n());
    if (last == Opnd::rM) w |= uint16_t(1u << insn.m());
    if (last == Opnd::r0) w |= 1;
    return w;
}

std::string_view name(Origin o) {
    switch (o) {
    case Origin::entry: return "entry";
    case Origin::call: return "call";
    case Origin::tail: return "tail";
    case Origin::pointer: return "pointer";
    case Origin::orphan: return "orphan";
    }
    return "?";
}

namespace {

using Regs = RegConstants;

// Where paths join, a register keeps its constant only if both agree.
// Returns whether `into` changed.
bool meet(Regs& into, const Regs& from) {
    bool changed = false;
    for (size_t i = 0; i < 16; ++i)
        if (into[i] && into[i] != from[i]) into[i].reset(), changed = true;
    if (into.pr && into.pr != from.pr) into.pr.reset(), changed = true;
    if (const uint16_t e = into.entry_pr & from.entry_pr; e != into.entry_pr) into.entry_pr = e, changed = true;
    if (into.pr_is_entry && !from.pr_is_entry) into.pr_is_entry = false, changed = true;
    return changed;
}

// Registers with no constants left, PR as it was.
Regs cleared(const Regs& r) {
    Regs out;
    out.pr = r.pr;
    out.pr_is_entry = r.pr_is_entry;
    return out;
}

// The result of a register-to-register or immediate integer operation whose
// inputs are constants, written to rN (r0 for the #imm logic forms).
std::optional<uint32_t> fold(const Insn& i, const Regs& r) {
    const auto& n = r[i.n()];
    const auto& m = r[i.m()];
    const auto both = [&](auto f) -> std::optional<uint32_t> {
        if (n && m) return f(*n, *m);
        return std::nullopt;
    };
    const auto one = [&](const std::optional<uint32_t>& x, auto f) -> std::optional<uint32_t> {
        if (x) return f(*x);
        return std::nullopt;
    };
    switch (i.op) {
    case Op::add_imm: return one(n, [&](uint32_t a) { return a + static_cast<uint32_t>(i.imm_s()); });
    case Op::add: return both([](uint32_t a, uint32_t b) { return a + b; });
    case Op::sub: return both([](uint32_t a, uint32_t b) { return a - b; });
    case Op::and_: return both([](uint32_t a, uint32_t b) { return a & b; });
    case Op::or_: return both([](uint32_t a, uint32_t b) { return a | b; });
    case Op::xor_: return both([](uint32_t a, uint32_t b) { return a ^ b; });
    case Op::neg: return one(m, [](uint32_t b) { return 0u - b; });
    case Op::not_: return one(m, [](uint32_t b) { return ~b; });
    case Op::extu_b: return one(m, [](uint32_t b) { return b & 0xFFu; });
    case Op::extu_w: return one(m, [](uint32_t b) { return b & 0xFFFFu; });
    case Op::exts_b: return one(m, [](uint32_t b) { return static_cast<uint32_t>(static_cast<int8_t>(b)); });
    case Op::exts_w: return one(m, [](uint32_t b) { return static_cast<uint32_t>(static_cast<int16_t>(b)); });
    case Op::swap_b: return one(m, [](uint32_t b) { return (b & 0xFFFF0000u) | (b & 0xFFu) << 8 | (b >> 8 & 0xFFu); });
    case Op::swap_w: return one(m, [](uint32_t b) { return b << 16 | b >> 16; });
    case Op::shll: case Op::shal: return one(n, [](uint32_t a) { return a << 1; });
    case Op::shlr: return one(n, [](uint32_t a) { return a >> 1; });
    case Op::shar: return one(n, [](uint32_t a) { return static_cast<uint32_t>(static_cast<int32_t>(a) >> 1); });
    case Op::shll2: return one(n, [](uint32_t a) { return a << 2; });
    case Op::shlr2: return one(n, [](uint32_t a) { return a >> 2; });
    case Op::shll8: return one(n, [](uint32_t a) { return a << 8; });
    case Op::shlr8: return one(n, [](uint32_t a) { return a >> 8; });
    case Op::shll16: return one(n, [](uint32_t a) { return a << 16; });
    case Op::shlr16: return one(n, [](uint32_t a) { return a >> 16; });
    default: return std::nullopt;
    }
}

// SHC's prologue saves registers first: mov.l rN,@-r15 for r8-r14, or
// sts.l pr,@-r15.
bool is_prologue(uint16_t w) {
    const unsigned reg = (w >> 4) & 15;
    return ((w & 0xFF0F) == 0x2F06 && reg >= 8 && reg <= 14) || w == 0x4F22;
}

struct Reject {
    std::string reason;
    uint32_t ran_into = 0;  // where a guess ran on into known code or another candidate, if that is the reason
};

bool is_branch(const Insn& i) { return i.valid() && (sh4::spec(i.op).flags & sh4::kBranch); }

class Explorer {
public:
    Explorer(const BootFile& boot, const Cfg& cfg, const std::set<uint32_t>& candidates, const std::set<uint32_t>& entries,
             std::map<uint32_t, uint16_t>& scratch_cache)
        : boot_(boot), cfg_(cfg), candidates_(candidates), entries_(entries), scratch_cache_(scratch_cache) {}

    // The scratch registers (r0-r7, as a mask) a call to `target` can change:
    // every one its code or anything it calls writes. Anything unknown (it
    // doesn't explore, it calls or jumps indirectly) counts as all of them.
    // r8-r15 are assumed kept, the compiler's convention (see cfg.hpp).
    uint16_t scratch_written(uint32_t target, int depth = 0) {
        if (const auto it = scratch_cache_.find(target); it != scratch_cache_.end()) return it->second;
        if (depth > 12) return 0xFF;
        scratch_cache_[target] = 0xFF;  // while it is being worked out (recursion): all
        uint16_t mask = 0;
        Function g;
        try {
            Explorer sub(boot_, cfg_, candidates_, entries_, scratch_cache_);
            g = sub.run(target, Origin::call);
        } catch (const Reject&) {
            return scratch_cache_[target] = 0xFF;
        }
        if (!g.indirect_calls.empty() || !g.indirect_jumps.empty()) return scratch_cache_[target] = 0xFF;
        for (uint32_t pc : g.insns) mask |= gpr_written(sh4::decode(boot_.u16(pc))) & 0xFFu;
        for (const auto& [site, callee] : g.calls) mask |= scratch_written(callee, depth + 1);
        for (const auto& [site, callee] : g.tail_jumps) mask |= scratch_written(callee, depth + 1);
        return scratch_cache_[target] = static_cast<uint16_t>(mask & 0xFFu);
    }

    // Everything reachable from `entry` without leaving the function, or the
    // reason it can't be one.
    const std::map<uint32_t, Regs>& states() const { return state_; }

    Function run(uint32_t entry, Origin origin) {
        Function f;
        f.entry = entry;
        f.origin = origin;
        guess_ = origin == Origin::pointer || origin == Origin::orphan;
        pointer_ = origin == Origin::pointer;
        state_.clear();
        todo_.clear();
        literals_.clear();
        leaders_.clear();
        unstable_.clear();
        switches_.clear();
        slots_.clear();
        flow_.clear();
        todo_.emplace_back(entry, Regs{});
        while (!todo_.empty()) {
            auto [pc, regs] = todo_.back();
            todo_.pop_back();
            walk(f, pc, regs);
        }
        // A jmp to a constant address that the function's other paths reach
        // too is a far branch inside it, not a tail call: a function too long
        // for `bra` to span branches with `mov.l label,rN ; jmp @rN`. (The
        // label's address in the literal pool is a pointer candidate while
        // the function is unknown: Analysis::find_evictions deals with that.)
        for (bool again = true; again;) {
            again = false;
            for (auto it = f.tail_jumps.begin(); it != f.tail_jumps.end();) {
                const auto [site, target] = *it;
                if (sh4::decode(boot_.u16(site)).op != Op::jmp || target == entry || !state_.contains(target) ||
                    unstable_.contains(site)) {
                    ++it;
                    continue;
                }
                it = f.tail_jumps.erase(it);
                f.computed_branches[site] = target;
                branch_to(target, cleared(state_.at(site)));
                again = true;
            }
            while (!todo_.empty()) {
                auto [pc, regs] = todo_.back();
                todo_.pop_back();
                walk(f, pc, regs);
            }
        }
        for (const auto& [pc, st] : state_) f.insns.push_back(pc);
        for (const auto& [a, n] : literals_) f.literals.emplace_back(a, n);
        f.leaders.assign(leaders_.begin(), leaders_.end());
        for (const auto& [braf, sw] : switches_) f.switches.push_back(sw);
        // A target resolved on one path and lost when another joined.
        for (uint32_t pc : unstable_) {
            const bool call = f.calls.erase(pc) != 0;
            f.tail_jumps.erase(pc);
            f.computed_branches.erase(pc);
            auto& list = call ? f.indirect_calls : f.indirect_jumps;
            if (std::ranges::find(list, pc) == list.end()) list.push_back(pc);
            f.problems.push_back(std::format("{:08X}: the target register's constant differs between paths", pc));
        }
        std::ranges::sort(f.indirect_calls);
        std::ranges::sort(f.indirect_jumps);
        // The function's code and its own data must not overlap.
        for (const auto& [a, n] : f.literals)
            for (uint32_t k = 0; k < n; k += 2)
                if (state_.contains(a + k)) throw Reject{std::format("the literal at {:08X} is also executed", a)};
        for (const Switch& sw : f.switches)
            for (uint32_t k = 0; k < sw.targets.size(); ++k)
                if (state_.contains(sw.table + 2 * k)) throw Reject{std::format("the jump table at {:08X} is also executed", sw.table)};
        for (uint32_t t : f.leaders)
            if (slots_.contains(t)) f.problems.push_back(std::format("{:08X}: a branch lands in a delay slot", t));
        return f;
    }

private:
    Insn fetch(uint32_t pc) const {
        if (pc % 2 || !boot_.mapped(pc, 2)) throw Reject{std::format("control reaches {:08X}, outside the image", pc)};
        const Insn i = sh4::decode(boot_.u16(pc));
        if (!i.valid()) throw Reject{std::format("{:08X} ({:04X}) is not an instruction", pc, i.word)};
        return i;
    }

    void transfer(const Insn& i, uint32_t pc, Regs& r) {
        switch (i.op) {
        case Op::mov_imm:
            r[i.n()] = static_cast<uint32_t>(i.imm_s());
            r.entry_pr &= static_cast<uint16_t>(~(1u << i.n()));
            return;
        case Op::movw_pc:
        case Op::movl_pc: {
            const uint32_t a = i.pc_relative(pc), n = i.op == Op::movl_pc ? 4 : 2;
            if (!boot_.mapped(a, n)) throw Reject{std::format("{:08X} reads a literal outside the image", pc)};
            literals_[a] = n;
            r[i.n()] = n == 4 ? boot_.u32(a) : static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(a)));
            r.entry_pr &= static_cast<uint16_t>(~(1u << i.n()));
            return;
        }
        case Op::mova: r[0] = i.pc_relative(pc); r.entry_pr &= ~1u; return;
        case Op::mov:
            r[i.n()] = r[i.m()];
            r.entry_pr = static_cast<uint16_t>((r.entry_pr & ~(1u << i.n())) | (((r.entry_pr >> i.m()) & 1u) << i.n()));
            return;
        case Op::sts_pr:
            r[i.n()] = r.pr_is_entry ? std::nullopt : r.pr;
            r.entry_pr = static_cast<uint16_t>((r.entry_pr & ~(1u << i.n())) | (r.pr_is_entry ? 1u << i.n() : 0u));
            return;
        case Op::lds_pr:
            r.pr = r[i.n()];
            r.pr_is_entry = (r.entry_pr >> i.n()) & 1u;
            return;
        case Op::ldsl_pr:
            // The epilogue's restore: what was saved isn't tracked.
            r.pr.reset();
            r.pr_is_entry = false;
            r[i.n()].reset();
            r.entry_pr &= static_cast<uint16_t>(~(1u << i.n()));
            return;
        default: break;
        }
        r.entry_pr &= static_cast<uint16_t>(~gpr_written(i));
        // Integer arithmetic on constants stays constant (addresses are often
        // built or adjusted this way). Anything else makes its result unknown.
        if (const auto v = fold(i, r)) {
            r[i.n()] = *v;
            return;
        }
        // A load from a constant address near the code is a PC-relative read
        // of the function's own data by another route (mova then fmov @r0,
        // or mov.l @(disp,r0)): its words are literals too.
        if ((sh4::spec(i.op).flags & sh4::kLoad) && !(sh4::spec(i.op).flags & sh4::kStore))
            if (const auto ea = sh4::register_address(i, r);
                ea && *ea % 2 == 0 && *ea + 2048 > pc && *ea < pc + 2048 && boot_.mapped(*ea, sh4::access_size(i)))
                literals_[*ea] = std::max(literals_[*ea], sh4::access_size(i));
        const uint16_t w = gpr_written(i);
        for (unsigned k = 0; k < 16; ++k)
            if (w & (1u << k)) r[k].reset();
    }

    bool shared_tail(uint32_t pc) const {
        return std::ranges::any_of(cfg_.functions, [&](const auto& kv) { return std::ranges::binary_search(kv.second.leaders, pc); });
    }

    // A branch to another function is a tail call: to an entry an earlier
    // pass found, or to the start of a compiled prologue, which compiled code
    // only ever reaches from outside its function.
    bool is_tail(const Function& f, uint32_t target) const {
        if (target == f.entry || !boot_.mapped(target, 2)) return false;
        if (entries_.contains(target)) return true;
        return target > boot_.base() && is_prologue(boot_.u16(target)) && !is_prologue(boot_.u16(target - 2));
    }

    void branch_to(uint32_t target, const Regs& r) {
        leaders_.insert(target);
        todo_.emplace_back(target, r);
    }

    // SHC's switch: a bounds check, then
    //   shll r0 ; mov r0,rX ; mova table,r0 ; mov.w @(r0,rX),r0 ; braf r0
    // The bound is the constant `cmp/hs` compares with, a few words back.
    std::optional<Switch> find_switch(uint32_t braf) const {
        const auto at = [&](uint32_t a) { return boot_.mapped(a, 2) && state_.contains(a) ? sh4::decode(boot_.u16(a)) : Insn{}; };
        const Insn load = at(braf - 2), mova = at(braf - 4);
        if (load.op != Op::movw_ld_r0 || load.n() != 0 || mova.op != Op::mova) return std::nullopt;
        Switch sw;
        sw.braf = braf;
        sw.table = mova.pc_relative(braf - 4);
        for (uint32_t a = braf - 6; a >= braf - 40 && a > boot_.base(); a -= 2) {
            const Insn cmp = at(a);
            if (cmp.op != Op::cmp_hs) continue;
            for (uint32_t b = a - 2; b >= a - 10; b -= 2) {
                const Insn def = at(b);
                int64_t count = -1;
                if (def.op == Op::mov_imm && def.n() == cmp.m()) count = def.imm_s();
                if (def.op == Op::movw_pc && def.n() == cmp.m() && boot_.mapped(def.pc_relative(b), 2))
                    count = static_cast<int16_t>(boot_.u16(def.pc_relative(b)));
                if (count < 0) continue;
                if (count == 0 || count > 4096 || !boot_.mapped(sw.table, static_cast<uint32_t>(2 * count)))
                    throw Reject{std::format("the switch at {:08X} has an impossible table", braf)};
                for (uint32_t k = 0; k < count; ++k)
                    sw.targets.push_back(braf + 4 + static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(sw.table + 2 * k))));
                return sw;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    void walk(Function& f, uint32_t pc, Regs r) {
        bool sequential = false;  // reached by falling through, not by a branch
        for (;;) {
            // An address seen only as a delay slot is still new as a branch target.
            const bool first = flow_.insert(pc).second;
            if (auto it = state_.find(pc); it != state_.end()) {
                if (!meet(it->second, r) && !first) return;  // nothing new on this path
                r = it->second;
            } else {
                (void)fetch(pc);
                // Compiled code never runs off its end into another function;
                // data that happens to decode does. (Hand-written library code
                // can, which is why this only tests guesses.)
                // (Following a branch into known code and on through it is fine.)
                if (guess_ && sequential && cfg_.at(pc - 2) != Kind::code) {
                    // Except, for an address the image holds, into a shared
                    // tail: code functions branch to, which compiled code falls
                    // into from a short entry of its own (bu's `mov #7,r4` at
                    // 0x8C0B6778, before the tail the entries beside it reach
                    // with `bra`). Bytes nothing points at that a branch skips
                    // are still data.
                    if (cfg_.at(pc) == Kind::code && !(pointer_ && shared_tail(pc)))
                        throw Reject{std::format("it runs on into known code at {:08X}", pc), pc};
                    if (cfg_.functions.contains(pc) || candidates_.contains(pc))
                        throw Reject{std::format("it runs on into the start of another function at {:08X}", pc), pc};
                }
                state_.emplace(pc, r);
            }
            sequential = true;
            const Insn i = fetch(pc);
            const uint32_t flags = sh4::spec(i.op).flags;
            if (!(flags & sh4::kBranch)) {
                transfer(i, pc, r);
                pc += 2;
                continue;
            }
            if (i.op == Op::trapa) throw Reject{std::format("{:08X} is a trapa", pc)};
            // The target, from the registers as they are before the delay slot.
            std::optional<uint32_t> target;
            if (flags & sh4::kPcRelative) {
                if (i.op == Op::braf || i.op == Op::bsrf) {
                    if (r[i.n()]) target = pc + 4 + *r[i.n()];
                } else {
                    target = i.branch_target(pc);
                }
            } else if (i.op == Op::jmp || i.op == Op::jsr) {
                target = r[i.n()];
            }
            const bool through_return_address = i.op == Op::jmp && ((r.entry_pr >> i.n()) & 1u);
            if (flags & sh4::kDelayed) {
                const Insn slot = fetch(pc + 2);
                if (is_branch(slot)) throw Reject{std::format("{:08X}: a branch in a delay slot", pc + 2)};
                if (sh4::spec(slot.op).flags & sh4::kPcRelative)
                    f.problems.push_back(std::format("{:08X}: a PC-relative instruction in a delay slot", pc + 2));
                if (auto it = state_.find(pc + 2); it != state_.end()) meet(it->second, r);
                else state_.emplace(pc + 2, r);
                slots_.insert(pc + 2);
                transfer(slot, pc + 2, r);
            }
            const uint32_t next = pc + ((flags & sh4::kDelayed) ? 4 : 2);
            const auto resolved = [&](std::map<uint32_t, uint32_t>& into) {
                if (target && boot_.mapped(*target, 2) && *target % 2 == 0) {
                    if (auto it = into.find(pc); it != into.end() && it->second != *target) unstable_.insert(pc);
                    into[pc] = *target;
                    return true;
                }
                if (into.contains(pc)) unstable_.insert(pc);
                return false;
            };
            if (flags & sh4::kCall) {
                if (!resolved(f.calls)) {
                    if (target && i.op != Op::jsr) throw Reject{std::format("{:08X} calls {:08X}, outside the image", pc, *target)};
                    // A constant outside the image is a call into the boot ROM or to another address space.
                    if (std::ranges::find(f.indirect_calls, pc) == f.indirect_calls.end()) f.indirect_calls.push_back(pc);
                }
                // The scratch registers the callee can change.
                const auto callee = f.calls.find(pc);
                const uint16_t clobbered = callee != f.calls.end() ? scratch_written(callee->second) : 0xFF;
                for (unsigned k = 0; k < 8; ++k)
                    if (clobbered & (1u << k)) r[k].reset();
                r.entry_pr &= static_cast<uint16_t>(~clobbered);
                r.pr.reset();
                r.pr_is_entry = false;  // PR now holds this call's return address
                pc = next;
                continue;
            }
            if (flags & sh4::kConditional) {
                if (is_tail(f, *target)) f.tail_jumps[pc] = *target;
                else branch_to(*target, r);
                pc = next;
                continue;
            }
            if (flags & sh4::kReturn) return;
            if (i.op == Op::bra) {
                if (!boot_.mapped(*target, 2)) throw Reject{std::format("{:08X} branches outside the image", pc)};
                if (is_tail(f, *target)) f.tail_jumps[pc] = *target;
                else branch_to(*target, r);
                return;
            }
            if (i.op == Op::braf) {
                if (target) {
                    if (auto it = f.computed_branches.find(pc); it != f.computed_branches.end() && it->second != *target)
                        unstable_.insert(pc);
                    f.computed_branches[pc] = *target;
                    branch_to(*target, r);
                    return;
                }
                if (auto sw = find_switch(pc)) {
                    for (uint32_t t : sw->targets) branch_to(t, cleared(r));
                    switches_[pc] = std::move(*sw);
                    return;
                }
            }
            if (i.op == Op::jmp) {
                // Through the return address the function was entered with: a return.
                if (through_return_address) {
                    f.jmp_returns.insert(std::ranges::upper_bound(f.jmp_returns, pc), pc);
                    return;
                }
                // With PR loaded by hand: a call that returns where PR points.
                if (r.pr && !r.pr_is_entry && boot_.mapped(*r.pr, 2) && *r.pr % 2 == 0) {
                    const uint32_t back = *r.pr;
                    if (!resolved(f.calls)) {
                        if (std::ranges::find(f.indirect_calls, pc) == f.indirect_calls.end()) f.indirect_calls.push_back(pc);
                    }
                    f.call_returns[pc] = back;
                    Regs after = cleared(r);
                    const auto callee = f.calls.find(pc);
                    const uint16_t clobbered = callee != f.calls.end() ? scratch_written(callee->second) : 0xFF;
                    for (unsigned k = 0; k < 16; ++k)
                        if (k >= 8 || !(clobbered & (1u << k))) after[k] = r[k];
                    after.entry_pr = static_cast<uint16_t>(r.entry_pr & ~clobbered);
                    after.pr.reset();
                    after.pr_is_entry = false;
                    branch_to(back, after);
                    return;
                }
            }
            // A jmp already found to be a far branch inside the function.
            if (i.op == Op::jmp && f.computed_branches.contains(pc)) {
                if (!target || *target != f.computed_branches.at(pc)) unstable_.insert(pc);
                else branch_to(*target, r);
                return;
            }
            // jmp, or a braf with no table: a tail jump if the target is a constant.
            if (i.op == Op::jmp && resolved(f.tail_jumps)) return;
            if (std::ranges::find(f.indirect_jumps, pc) == f.indirect_jumps.end()) f.indirect_jumps.push_back(pc);
            return;
        }
    }

    const BootFile& boot_;
    const Cfg& cfg_;
    const std::set<uint32_t>& candidates_;  // addresses that may be function entries
    const std::set<uint32_t>& entries_;     // function entries an earlier pass found
    std::map<uint32_t, uint16_t>& scratch_cache_;  // function -> scratch registers a call to it can change
    bool guess_ = false;
    bool pointer_ = false;  // a guess from an address the image holds
    std::map<uint32_t, Regs> state_;  // registers on entry to each instruction reached
    std::vector<std::pair<uint32_t, Regs>> todo_;
    std::map<uint32_t, uint32_t> literals_;
    std::set<uint32_t> leaders_, slots_, unstable_, flow_;
    std::map<uint32_t, Switch> switches_;
};

class Analysis {
public:
    Analysis(const BootFile& boot, std::span<const Region> regions, std::set<uint32_t> entries,
             const std::map<uint32_t, uint32_t>& banned)
        : boot_(boot), banned_(banned), entries_(std::move(entries)), explorer_(boot, cfg_, candidates_, entries_, scratch_) {
        cfg_.base = boot.base();
        cfg_.kind.assign(boot.size() / 2, Kind::unknown);
        cfg_.owner.assign(boot.size() / 2, 0);
        for (const Region& r : regions)
            if (r.plan == Plan::data)
                for (uint32_t a = std::max(r.begin, boot.base()); a < r.end && a < boot.end(); a += 2) cfg_.kind[index(a)] = Kind::data;
    }

    Cfg run() {
        add(boot_.entry(), Origin::entry);
        drain();
        // Function pointers, until no new function turns up.
        for (;;) {
            const auto candidate = [&](uint32_t c) {
                if (c % 2 != 0 || !boot_.mapped(c, 2)) return;
                if (const auto it = banned_.find(c); it != banned_.end()) {
                    if (rejected_.insert(c).second)
                        cfg_.rejected.push_back(
                            {c, Origin::pointer,
                             std::format("only a data word holds this address, and it is inside the function at {:08X}", it->second)});
                    return;
                }
                candidates_.insert(c);
            };
            for (const auto& [entry, f] : cfg_.functions)
                for (const auto& [a, n] : f.literals)
                    if (n == 4) candidate(boot_.u32(a));
            for (uint32_t a = boot_.base(); a + 4 <= boot_.end(); a += 4)
                if ((kind(a) == Kind::unknown || kind(a) == Kind::data) && (kind(a + 2) == Kind::unknown || kind(a + 2) == Kind::data))
                    candidate(boot_.u32(a));
            // A table of function pointers can also point into the middle of a
            // function found already, where another function's code was
            // reached by a branch that is really a tail call (gdFs's driver
            // table has one, 0x8C0B1FD6, which 0x8C0B1704 branches to). Such a
            // pointer is taken when the words beside it in the table point at
            // function entries and it isn't a delay slot.
            std::vector<uint32_t> inside;
            for (uint32_t a = boot_.base() + 4; a + 8 <= boot_.end(); a += 4) {
                if (kind(a) != Kind::data && kind(a) != Kind::unknown) continue;
                const uint32_t c = boot_.u32(a);
                if (c % 2 || !boot_.mapped(c, 2) || kind(c) != Kind::code || cfg_.functions.contains(c)) continue;
                if (!cfg_.functions.contains(boot_.u32(a - 4)) || !cfg_.functions.contains(boot_.u32(a + 4))) continue;
                const Insn prev = sh4::decode(boot_.u16(c - 2));
                if (kind(c - 2) == Kind::code && prev.valid() && (sh4::spec(prev.op).flags & sh4::kDelayed)) continue;
                inside.push_back(c);
            }
            const size_t before = cfg_.functions.size();
            const std::vector<uint32_t> round(candidates_.begin(), candidates_.end());
            for (uint32_t c : round) {
                if (kind(c) != Kind::unknown) continue;
                add(c, Origin::pointer);
                drain();
            }
            for (uint32_t c : inside) {
                add(c, Origin::pointer);
                drain();
            }
            if (cfg_.functions.size() == before) break;
        }
        // Orphans: the first thing in each gap that isn't padding.
        for (bool added = true; added;) {
            added = false;
            for (uint32_t a = boot_.base() + 2; a < boot_.end(); a += 2) {
                if (kind(a) != Kind::unknown || kind(a - 2) == Kind::unknown) continue;
                uint32_t b = a;
                while (b < boot_.end() && kind(b) == Kind::unknown && is_padding(boot_.u16(b))) b += 2;
                if (b >= boot_.end() || kind(b) != Kind::unknown) continue;
                const size_t before = cfg_.functions.size();
                add(b, Origin::orphan);
                drain();
                added |= cfg_.functions.size() != before;
            }
            // Where the gap's first word wasn't a function, the gap may still
            // hold one after some data: try where a compiled prologue starts
            // (the first of a run of register pushes).
            for (uint32_t a = boot_.base() + 2; a < boot_.end(); a += 2) {
                if (kind(a) != Kind::unknown || !is_prologue(boot_.u16(a)) || is_prologue(boot_.u16(a - 2))) continue;
                const size_t before = cfg_.functions.size();
                add(a, Origin::orphan);
                drain();
                added |= cfg_.functions.size() != before;
            }
        }
        find_evictions();
        // Padding: zero or nop words between classified things, up to 16 bytes
        // of them (alignment), or any run of them that ends the image.
        for (uint32_t a = boot_.base(); a < boot_.end();) {
            if (kind(a) != Kind::unknown) {
                a += 2;
                continue;
            }
            uint32_t b = a;
            bool pad = true;
            while (b < boot_.end() && kind(b) == Kind::unknown) pad &= is_padding(boot_.u16(b)), b += 2;
            if (pad && (b - a <= 16 || b == boot_.end()))
                for (uint32_t k = a; k < b; k += 2) cfg_.kind[index(k)] = Kind::padding;
            a = b;
        }
        std::ranges::sort(cfg_.rejected, {}, &Rejected::address);
        return std::move(cfg_);
    }

    // Pointer guesses this pass showed to be wrong: entry -> the function it is inside.
    const std::map<uint32_t, uint32_t>& evicted() const { return evicted_; }

private:
    static bool is_padding(uint16_t w) { return w == 0x0000 || w == 0x0009; }
    size_t index(uint32_t a) const { return (a - boot_.base()) / 2; }
    Kind kind(uint32_t a) const { return cfg_.kind[index(a)]; }

    // A data word can hold a code address by coincidence, and the code there
    // can decode up to a return: the middle of a function nothing refers to,
    // taken for a function. It shows when the real function is tried as a
    // guess itself: it starts with a compiled prologue and runs straight on
    // into the first guess, which compiled code never does into another function. Then
    // the guess is the mistake, provided nothing else supports it: it has no
    // prologue of its own, no literal pool holds its address, and nothing
    // calls or jumps to it. The analysis is run again without it. (The
    // commonest such word is a long function's own literal: the label of a
    // far branch inside it.)
    void find_evictions() {
        if (ran_into_.empty()) return;
        std::set<uint32_t> referenced;
        for (const auto& [entry, f] : cfg_.functions) {
            for (const auto& [a, n] : f.literals)
                if (n == 4) referenced.insert(boot_.u32(a));
            for (const auto& [site, target] : f.calls) referenced.insert(target);
            for (const auto& [site, target] : f.tail_jumps) referenced.insert(target);
        }
        for (const auto& [guess, into] : ran_into_) {
            const auto it = cfg_.functions.find(into);
            if (it == cfg_.functions.end() || it->second.origin != Origin::pointer) continue;
            if (!is_prologue(boot_.u16(guess)) || is_prologue(boot_.u16(into)) || referenced.contains(into)) continue;
            evicted_.emplace(into, guess);
        }
    }

    void add(uint32_t entry, Origin origin) {
        // A guess that failed once fails again: try each only once.
        if ((origin == Origin::pointer || origin == Origin::orphan) && !guessed_.insert(entry).second) return;
        work_.emplace_back(entry, origin);
    }

    void drain() {
        while (!work_.empty()) {
            const auto [entry, origin] = work_.back();
            work_.pop_back();
            if (cfg_.functions.contains(entry)) continue;
            try {
                Function f = explorer_.run(entry, origin);
                if (const auto clash = collision(f, origin)) {
                    // A direct call or jump is a fact about the code: keep it and say so.
                    if (origin == Origin::pointer || origin == Origin::orphan) throw Reject{*clash};
                    f.problems.push_back(*clash);
                }
                // A guess is only as good as what it calls: real code doesn't
                // call or jump to something that isn't code.
                if (origin == Origin::pointer || origin == Origin::orphan) {
                    std::set<uint32_t> visiting{entry};
                    if (const auto bad = dead_end(f, visiting)) throw Reject{*bad};
                }
                commit(std::move(f));
            } catch (const Reject& r) {
                if (rejected_.insert(entry).second) cfg_.rejected.push_back({entry, origin, r.reason});
                if ((origin == Origin::orphan || origin == Origin::pointer) && r.ran_into) ran_into_.emplace_back(entry, r.ran_into);
            }
        }
    }

    // The first call or constant jump from `f`, followed through functions not
    // yet known, that leads to something that can't be a function.
    std::optional<std::string> dead_end(const Function& f, std::set<uint32_t>& visiting) {
        const auto check = [&](uint32_t site, uint32_t target) -> std::optional<std::string> {
            if (cfg_.functions.contains(target) || !visiting.insert(target).second) return std::nullopt;
            if (kind(target) == Kind::literal || kind(target) == Kind::jump_table || kind(target) == Kind::data)
                return std::format("{:08X} calls {:08X}, which is data", site, target);
            try {
                const Function callee = explorer_.run(target, Origin::call);
                if (const auto clash = collision(callee, Origin::pointer))
                    return std::format("{:08X} calls {:08X}: {}", site, target, *clash);
                return dead_end(callee, visiting);
            } catch (const Reject& r) {
                return std::format("{:08X} calls {:08X}: {}", site, target, r.reason);
            }
        };
        for (const auto& [site, target] : f.calls)
            if (auto bad = check(site, target)) return bad;
        for (const auto& [site, target] : f.tail_jumps)
            if (auto bad = check(site, target)) return bad;
        return std::nullopt;
    }

    // Code on another function's data, or data on code.
    std::optional<std::string> collision(const Function& f, Origin origin) const {
        const bool guess = origin == Origin::pointer || origin == Origin::orphan;
        if (guess && f.insns.size() < 2) return "too short to be a function";
        for (uint32_t pc : f.insns) {
            const Kind k = kind(pc);
            if (k == Kind::literal || k == Kind::jump_table) return std::format("its code at {:08X} is another function's data", pc);
            if (k == Kind::data) return std::format("its code at {:08X} is in a data region", pc);
        }
        for (const auto& [a, n] : f.literals)
            for (uint32_t o = 0; o < n; o += 2)
                if (kind(a + o) == Kind::code) return std::format("its literal at {:08X} is another function's code", a);
                else if (kind(a + o) == Kind::data && guess) return std::format("its literal at {:08X} is in a data region", a);
        for (const Switch& sw : f.switches)
            for (uint32_t k = 0; k < sw.targets.size(); ++k)
                if (kind(sw.table + 2 * k) == Kind::code) return std::format("its jump table at {:08X} is another function's code", sw.table);
        return std::nullopt;
    }

    void commit(Function f) {
        const auto mark = [&](uint32_t a, Kind k) {
            if (cfg_.kind[index(a)] == Kind::unknown) cfg_.kind[index(a)] = k;
            if (!cfg_.owner[index(a)]) cfg_.owner[index(a)] = f.entry;
        };
        for (uint32_t pc : f.insns) mark(pc, Kind::code);
        for (const auto& [a, n] : f.literals)
            for (uint32_t o = 0; o < n; o += 2) mark(a + o, Kind::literal);
        for (const Switch& sw : f.switches)
            for (uint32_t k = 0; k < sw.targets.size(); ++k) mark(sw.table + 2 * k, Kind::jump_table);
        for (const auto& [site, target] : f.calls) add(target, Origin::call);
        for (const auto& [site, target] : f.tail_jumps) add(target, Origin::tail);
        cfg_.functions.emplace(f.entry, std::move(f));
    }

    const BootFile& boot_;
    const std::map<uint32_t, uint32_t>& banned_;
    std::map<uint32_t, uint32_t> evicted_;
    std::vector<std::pair<uint32_t, uint32_t>> ran_into_;  // a rejected guess, the address it ran on into
    Cfg cfg_;
    std::set<uint32_t> candidates_;
    std::set<uint32_t> entries_;
    std::map<uint32_t, uint16_t> scratch_;
    Explorer explorer_;
    std::vector<std::pair<uint32_t, Origin>> work_;
    std::set<uint32_t> guessed_, rejected_;
};

}  // namespace

// Tail calls found in one pass (a branch to a function found later) change
// what the functions before them contain, so the analysis runs again with
// every entry found so far until the set of entries no longer changes.
// A pass that shows a pointer guess to be wrong (find_evictions) is run again
// without it.
Cfg analyse(const BootFile& boot, std::span<const Region> regions) {
    std::set<uint32_t> entries;
    std::map<uint32_t, uint32_t> banned;
    for (int pass = 0;; ++pass) {
        Analysis analysis(boot, regions, entries, banned);
        Cfg cfg = analysis.run();
        if (!analysis.evicted().empty() && pass < 8) {
            for (const auto& [guess, inside] : analysis.evicted()) banned.emplace(guess, inside), entries.erase(guess);
            continue;
        }
        // Keep every entry seen: one dropped in a later pass would make it oscillate.
        const size_t before = entries.size();
        for (const auto& [entry, f] : cfg.functions) entries.insert(entry);
        if (entries.size() == before) return cfg;
        if (pass == 8) throw std::logic_error("function discovery doesn't converge");
    }
}

std::vector<uint32_t> successors(const BootFile& boot, const Function& f, uint32_t pc) {
    std::vector<uint32_t> out;
    const auto in = [&](uint32_t a) { return std::ranges::binary_search(f.insns, a); };
    const auto add = [&](uint32_t a) {
        if (in(a) && std::ranges::find(out, a) == out.end()) out.push_back(a);
    };
    // A delay slot goes where its branch goes.
    uint32_t at = pc;
    bool slot = false;
    if (pc > boot.base() && in(pc - 2)) {
        const Insn b = sh4::decode(boot.u16(pc - 2));
        if (b.valid() && (sh4::spec(b.op).flags & sh4::kDelayed)) at = pc - 2, slot = true;
    }
    const Insn i = sh4::decode(boot.u16(at));
    const uint32_t flags = sh4::spec(i.op).flags;
    if (!(flags & sh4::kBranch)) {
        add(pc + 2);
        return out;
    }
    if ((flags & sh4::kDelayed) && !slot) {
        add(pc + 2);  // the slot first
        return out;
    }
    const uint32_t next = at + ((flags & sh4::kDelayed) ? 4 : 2);
    if (flags & sh4::kCall) {
        add(next);
        return out;
    }
    if (flags & sh4::kReturn) return out;
    const bool tail = f.tail_jumps.contains(at);
    switch (i.op) {
    case Op::bt: case Op::bf: case Op::bt_s: case Op::bf_s:
        if (!tail) add(i.branch_target(at));
        add(next);
        break;
    case Op::bra:
        if (!tail) add(i.branch_target(at));
        break;
    case Op::braf:
        if (auto it = f.computed_branches.find(at); it != f.computed_branches.end()) add(it->second);
        for (const Switch& sw : f.switches)
            if (sw.braf == at)
                for (uint32_t t : sw.targets) add(t);
        break;
    case Op::jmp:
        // A call made by loading PR by hand continues where PR points; any
        // other jmp (a tail call, an indirect jump, a return) leaves.
        if (auto it = f.call_returns.find(at); it != f.call_returns.end()) add(it->second);
        // A far branch inside the function.
        if (auto it = f.computed_branches.find(at); it != f.computed_branches.end()) add(it->second);
        break;
    default: break;  // trapa never reaches here
    }
    return out;
}

std::map<uint32_t, RegConstants> register_constants(const BootFile& boot, const Cfg& cfg, const Function& f) {
    const std::set<uint32_t> none;
    std::set<uint32_t> entries;
    for (const auto& [entry, fn] : cfg.functions) entries.insert(entry);
    std::map<uint32_t, uint16_t> scratch;
    Explorer e(boot, cfg, none, entries, scratch);
    try {
        // Not as a guess: the function is already known to be good.
        (void)e.run(f.entry, Origin::call);
    } catch (const Reject& r) {
        throw std::logic_error(std::format("register_constants: fn_{:08X} no longer explores: {}", f.entry, r.reason));
    }
    return e.states();
}

}  // namespace qilin
