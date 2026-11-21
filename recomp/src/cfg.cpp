#include "cfg.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <set>

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

using Regs = std::array<std::optional<uint32_t>, 16>;

// Where paths join, a register keeps its constant only if both agree.
// Returns whether `into` changed.
bool meet(Regs& into, const Regs& from) {
    bool changed = false;
    for (size_t i = 0; i < 16; ++i)
        if (into[i] && into[i] != from[i]) into[i].reset(), changed = true;
    return changed;
}

struct Reject {
    std::string reason;
};

bool is_branch(const Insn& i) { return i.valid() && (sh4::spec(i.op).flags & sh4::kBranch); }

class Explorer {
public:
    Explorer(const BootFile& boot, const Cfg& cfg, const std::set<uint32_t>& candidates)
        : boot_(boot), cfg_(cfg), candidates_(candidates) {}

    // Everything reachable from `entry` without leaving the function, or the
    // reason it can't be one.
    Function run(uint32_t entry, Origin origin) {
        Function f;
        f.entry = entry;
        f.origin = origin;
        guess_ = origin == Origin::pointer || origin == Origin::orphan;
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
        for (const auto& [pc, st] : state_) f.insns.push_back(pc);
        for (const auto& [a, n] : literals_) f.literals.emplace_back(a, n);
        f.leaders.assign(leaders_.begin(), leaders_.end());
        for (const auto& [braf, sw] : switches_) f.switches.push_back(sw);
        // A target resolved on one path and lost when another joined.
        for (uint32_t pc : unstable_) {
            const bool call = f.calls.erase(pc) != 0;
            f.tail_jumps.erase(pc);
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
        case Op::mov_imm: r[i.n()] = static_cast<uint32_t>(i.imm_s()); return;
        case Op::movw_pc:
        case Op::movl_pc: {
            const uint32_t a = i.pc_relative(pc), n = i.op == Op::movl_pc ? 4 : 2;
            if (!boot_.mapped(a, n)) throw Reject{std::format("{:08X} reads a literal outside the image", pc)};
            literals_[a] = n;
            r[i.n()] = n == 4 ? boot_.u32(a) : static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(a)));
            return;
        }
        case Op::mova: r[0] = i.pc_relative(pc); return;
        case Op::mov: r[i.n()] = r[i.m()]; return;
        default: break;
        }
        const uint16_t w = gpr_written(i);
        for (unsigned k = 0; k < 16; ++k)
            if (w & (1u << k)) r[k].reset();
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
                    if (cfg_.at(pc) == Kind::code) throw Reject{std::format("it runs on into known code at {:08X}", pc)};
                    if (cfg_.functions.contains(pc) || candidates_.contains(pc))
                        throw Reject{std::format("it runs on into the start of another function at {:08X}", pc)};
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
                for (unsigned k = 0; k < 8; ++k) r[k].reset();  // the callee's scratch registers
                pc = next;
                continue;
            }
            if (flags & sh4::kConditional) {
                branch_to(*target, r);
                pc = next;
                continue;
            }
            if (flags & sh4::kReturn) return;
            if (i.op == Op::bra) {
                if (!boot_.mapped(*target, 2)) throw Reject{std::format("{:08X} branches outside the image", pc)};
                branch_to(*target, r);
                return;
            }
            if (i.op == Op::braf) {
                if (target) {
                    branch_to(*target, r);
                    return;
                }
                if (auto sw = find_switch(pc)) {
                    for (uint32_t t : sw->targets) branch_to(t, Regs{});
                    switches_[pc] = std::move(*sw);
                    return;
                }
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
    bool guess_ = false;
    std::map<uint32_t, Regs> state_;  // registers on entry to each instruction reached
    std::vector<std::pair<uint32_t, Regs>> todo_;
    std::map<uint32_t, uint32_t> literals_;
    std::set<uint32_t> leaders_, slots_, unstable_, flow_;
    std::map<uint32_t, Switch> switches_;
};

class Analysis {
public:
    explicit Analysis(const BootFile& boot) : boot_(boot), explorer_(boot, cfg_, candidates_) {
        cfg_.base = boot.base();
        cfg_.kind.assign(boot.size() / 2, Kind::unknown);
        cfg_.owner.assign(boot.size() / 2, 0);
    }

    Cfg run() {
        add(boot_.entry(), Origin::entry);
        drain();
        // Function pointers, until no new function turns up.
        for (;;) {
            const auto candidate = [&](uint32_t c) {
                if (c % 2 == 0 && boot_.mapped(c, 2)) candidates_.insert(c);
            };
            for (const auto& [entry, f] : cfg_.functions)
                for (const auto& [a, n] : f.literals)
                    if (n == 4) candidate(boot_.u32(a));
            for (uint32_t a = boot_.base(); a + 4 <= boot_.end(); a += 4)
                if (kind(a) == Kind::unknown && kind(a + 2) == Kind::unknown) candidate(boot_.u32(a));
            const size_t before = cfg_.functions.size();
            const std::vector<uint32_t> round(candidates_.begin(), candidates_.end());
            for (uint32_t c : round) {
                if (kind(c) != Kind::unknown) continue;
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
        }
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

private:
    static bool is_padding(uint16_t w) { return w == 0x0000 || w == 0x0009; }
    size_t index(uint32_t a) const { return (a - boot_.base()) / 2; }
    Kind kind(uint32_t a) const { return cfg_.kind[index(a)]; }

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
            }
        }
    }

    // The first call or constant jump from `f`, followed through functions not
    // yet known, that leads to something that can't be a function.
    std::optional<std::string> dead_end(const Function& f, std::set<uint32_t>& visiting) {
        const auto check = [&](uint32_t site, uint32_t target) -> std::optional<std::string> {
            if (cfg_.functions.contains(target) || !visiting.insert(target).second) return std::nullopt;
            if (kind(target) == Kind::literal || kind(target) == Kind::jump_table)
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
        }
        for (const auto& [a, n] : f.literals)
            for (uint32_t o = 0; o < n; o += 2)
                if (kind(a + o) == Kind::code) return std::format("its literal at {:08X} is another function's code", a);
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
    Cfg cfg_;
    std::set<uint32_t> candidates_;
    Explorer explorer_;
    std::vector<std::pair<uint32_t, Origin>> work_;
    std::set<uint32_t> guessed_, rejected_;
};

}  // namespace

Cfg analyse(const BootFile& boot) { return Analysis(boot).run(); }

}  // namespace qilin
