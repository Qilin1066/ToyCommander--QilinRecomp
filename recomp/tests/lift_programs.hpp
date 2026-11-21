// Synthetic SH-4 programs for the end-to-end lifter tests: a small assembler
// and the programs, built into one boot file image. Shared by the generator
// (which lifts the image with the real lifter at build time) and the driver
// (which runs the lifted code against references). No game code.
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace qilin::test {

inline constexpr uint32_t kBase = 0x8C010000u;

// A tiny assembler: words, labels, branches fixed up once labels are known,
// and literal pools.
class Asm {
public:
    uint32_t here() const { return kBase + static_cast<uint32_t>(w_.size() * 2); }
    void word(uint16_t v) { w_.push_back(v); }
    void label(const std::string& name) {
        if (!labels_.emplace(name, here()).second) throw std::logic_error("label defined twice: " + name);
    }
    uint32_t at(const std::string& name) const {
        const auto it = labels_.find(name);
        if (it == labels_.end()) throw std::logic_error("no label " + name);
        return it->second;
    }
    // Branches: 8-bit (bt, bf, bt/s, bf/s) and 12-bit (bra, bsr) displacements.
    void b8(uint16_t op, const std::string& to) { fix_.push_back({w_.size(), to, 8}), word(op); }
    void b12(uint16_t op, const std::string& to) { fix_.push_back({w_.size(), to, 12}), word(op); }
    // mov.l @(disp,PC),Rn of a 32-bit value; mova @(disp,PC),R0 of a label.
    void movl(unsigned n, uint32_t value) { pool_.push_back({w_.size(), value, {}}), word(static_cast<uint16_t>(0xD000 | n << 8)); }
    void movl_label(unsigned n, const std::string& to) { pool_.push_back({w_.size(), 0, to}), word(static_cast<uint16_t>(0xD000 | n << 8)); }
    void mova(const std::string& to) { mova_.push_back({w_.size(), to}), word(0xC700); }
    // mov.l @(disp,PC),Rn of the word at a label (a literal placed by hand).
    void movl_at(unsigned n, const std::string& to) { mova_.push_back({w_.size(), to}), word(static_cast<uint16_t>(0xD000 | n << 8)); }
    // The pending literals, 4-byte aligned, after the code before them.
    void pool() {
        if (w_.size() % 2) word(0x0009);
        for (const Lit& l : pool_) {
            const uint32_t pc = kBase + static_cast<uint32_t>(l.at * 2);
            const uint32_t a = here();
            const uint32_t disp = (a - ((pc & ~3u) + 4)) / 4;
            if (disp > 255) throw std::logic_error("literal too far");
            w_[l.at] = static_cast<uint16_t>(w_[l.at] | disp);
            lits_.push_back({w_.size(), l.label, l.value});
            word(0), word(0);
        }
        pool_.clear();
    }
    // A 32-bit word holding a label's address (a table entry).
    void long_of(const std::string& to) {
        if (w_.size() % 2) throw std::logic_error("unaligned long");
        lits_.push_back({w_.size(), to, 0});
        word(0), word(0);
    }
    void align4() {
        if (w_.size() % 2) word(0x0009);
    }
    std::vector<uint16_t> finish() {
        if (!pool_.empty()) throw std::logic_error("a pool was not emitted");
        for (const Fix& f : fix_) {
            const uint32_t pc = kBase + static_cast<uint32_t>(f.at * 2);
            const int32_t d = (static_cast<int32_t>(at(f.to)) - static_cast<int32_t>(pc + 4)) / 2;
            const int32_t limit = f.bits == 8 ? 128 : 2048;
            if (d < -limit || d >= limit) throw std::logic_error("branch too far: " + f.to);
            w_[f.at] = static_cast<uint16_t>(w_[f.at] | (d & (f.bits == 8 ? 0xFF : 0xFFF)));
        }
        for (const auto& [at, to] : mova_) {
            const uint32_t pc = kBase + static_cast<uint32_t>(at * 2);
            const uint32_t d = (this->at(to) - ((pc & ~3u) + 4)) / 4;
            if (d > 255 || this->at(to) % 4) throw std::logic_error("mova target: " + to);
            w_[at] = static_cast<uint16_t>(w_[at] | d);
        }
        for (const Placed& p : lits_) {
            const uint32_t v = p.label.empty() ? p.value : at(p.label);
            w_[p.at] = static_cast<uint16_t>(v), w_[p.at + 1] = static_cast<uint16_t>(v >> 16);
        }
        return w_;
    }

private:
    struct Fix {
        size_t at;
        std::string to;
        int bits;
    };
    struct Lit {
        size_t at;
        uint32_t value;
        std::string label;
    };
    struct Placed {
        size_t at;
        std::string label;
        uint32_t value;
    };
    std::vector<uint16_t> w_;
    std::map<std::string, uint32_t> labels_;
    std::vector<Fix> fix_;
    std::vector<Lit> pool_;
    std::vector<Placed> lits_;
    std::vector<std::pair<size_t, std::string>> mova_;
};

// Encodings (n, m: register numbers).
constexpr uint16_t nm(uint16_t op, unsigned n, unsigned m) { return static_cast<uint16_t>(op | n << 8 | m << 4); }
constexpr uint16_t n_(uint16_t op, unsigned n) { return static_cast<uint16_t>(op | n << 8); }
constexpr uint16_t imm(uint16_t op, unsigned n, int v) { return static_cast<uint16_t>(op | n << 8 | (v & 0xFF)); }
constexpr uint16_t kNop = 0x0009, kRts = 0x000B, kClrt = 0x0008, kSett = 0x0018, kDiv0u = 0x0019, kClrmac = 0x0028;

// The programs, each a function at its own address; entries by name.
struct Programs {
    std::vector<uint16_t> image;
    std::map<std::string, uint32_t> entry;
    uint32_t data = 0;  // a 256-byte data block: the indirect-call table, then scratch
};

inline Programs build() {
    Asm a;
    Programs p;
    // Entry: the first function, so analysis starts here; it calls every
    // other one directly, so all are found as call targets.
    a.label("entry");
    a.b12(0xB000, "add64"), a.word(kNop);
    a.b12(0xB000, "sub64"), a.word(kNop);
    a.b12(0xB000, "neg64"), a.word(kNop);
    a.b12(0xB000, "udiv"), a.word(kNop);
    a.b12(0xB000, "sdiv"), a.word(kNop);
    a.b12(0xB000, "bits"), a.word(kNop);
    a.b12(0xB000, "mul"), a.word(kNop);
    a.b12(0xB000, "loop"), a.word(kNop);
    a.b12(0xB000, "sw"), a.word(kNop);
    a.b12(0xB000, "calls"), a.word(kNop);
    a.b12(0xB000, "indirect"), a.word(kNop);
    a.b12(0xB000, "prcall"), a.word(kNop);
    a.b12(0xB000, "prbra"), a.word(kNop);
    a.b12(0xB000, "farjmp"), a.word(kNop);
    a.b12(0xB000, "hang"), a.word(kNop);
    a.b12(0xB000, "patch"), a.word(kNop);
    a.b12(0xB000, "badjump"), a.word(kNop);
    a.b12(0xB000, "fpu"), a.word(kNop);
    a.b12(0xB000, "memory"), a.word(kNop);
    a.b12(0xB000, "farith"), a.word(kNop);
    a.b12(0xB000, "ftrc"), a.word(kNop);
    a.b12(0xB000, "fpair"), a.word(kNop);
    a.b12(0xB000, "fdouble"), a.word(kNop);
    a.b12(0xB000, "fmac"), a.word(kNop);
    a.b12(0xB000, "fsrra"), a.word(kNop);
    a.b12(0xB000, "fipr"), a.word(kNop);
    a.b12(0xB000, "ftrv"), a.word(kNop);
    a.b12(0xB000, "fsca"), a.word(kNop);
    a.b12(0xB000, "call_native"), a.word(kNop);
    a.word(kRts), a.word(kNop);

    // add64: (r5:r4) + (r7:r6) -> (r1:r0)
    a.label("add64");
    a.word(kClrt), a.word(nm(0x300E, 4, 6)), a.word(nm(0x300E, 5, 7)), a.word(nm(0x6003, 0, 4)), a.word(kRts), a.word(nm(0x6003, 1, 5));
    // sub64: (r5:r4) - (r7:r6) -> (r1:r0)
    a.label("sub64");
    a.word(kClrt), a.word(nm(0x300A, 4, 6)), a.word(nm(0x300A, 5, 7)), a.word(nm(0x6003, 0, 4)), a.word(kRts), a.word(nm(0x6003, 1, 5));
    // neg64: -(r5:r4) -> (r1:r0)
    a.label("neg64");
    a.word(kClrt), a.word(nm(0x600A, 0, 4)), a.word(nm(0x600A, 1, 5)), a.word(kRts), a.word(kNop);
    // udiv: r4 / r5 (unsigned) -> r0, by the 64/32 sequence: r1:r4 with r1 = 0.
    a.label("udiv");
    a.word(imm(0xE000, 1, 0)), a.word(kDiv0u);
    for (int k = 0; k < 32; ++k) a.word(n_(0x4024, 4)), a.word(nm(0x3004, 1, 5));
    a.word(n_(0x4024, 4)), a.word(kRts), a.word(nm(0x6003, 0, 4));
    // sdiv: r1 / r0 (signed, r0 != 0) -> r1.
    a.label("sdiv");
    a.word(nm(0x6003, 2, 1));  // mov r1,r2
    a.word(n_(0x4024, 2));     // rotcl r2
    a.word(nm(0x300A, 2, 2));  // subc r2,r2: the dividend's sign, extended
    a.word(nm(0x200A, 3, 3));  // xor r3,r3
    a.word(nm(0x300A, 1, 3));  // subc r3,r1: one less if negative
    a.word(nm(0x2007, 2, 0));  // div0s r0,r2
    for (int k = 0; k < 32; ++k) a.word(n_(0x4024, 1)), a.word(nm(0x3004, 2, 0));
    a.word(n_(0x4024, 1));     // rotcl r1
    a.word(kRts);
    a.word(nm(0x300E, 1, 3));  // addc r3,r1
    // bits: r4, r5 -> r0..r3, r6, r7: shad, shld, rotcl/rotcr chain, cmp/str, xtrct, swap.b
    a.label("bits");
    a.word(nm(0x6003, 0, 4)), a.word(nm(0x400C, 0, 5));  // r0 = shad(r4, r5)
    a.word(nm(0x6003, 1, 4)), a.word(nm(0x400D, 1, 5));  // r1 = shld(r4, r5)
    a.word(nm(0x6003, 2, 4)), a.word(kSett), a.word(n_(0x4024, 2)), a.word(n_(0x4025, 2)), a.word(n_(0x4025, 2));  // r2: rotcl, rotcr, rotcr with T
    a.word(nm(0x200C, 4, 5)), a.word(n_(0x0029, 3));     // r3 = cmp/str r5,r4
    a.word(nm(0x6003, 6, 4)), a.word(nm(0x200D, 6, 5));  // r6 = xtrct r5,r6
    a.word(kRts), a.word(nm(0x6008, 7, 4));               // r7 = swap.b r4
    // mul: dmuls.l, dmulu.l, mul.l, muls.w, mulu.w
    a.label("mul");
    a.word(nm(0x300D, 4, 5)), a.word(n_(0x000A, 0)), a.word(n_(0x001A, 1));
    a.word(nm(0x3005, 4, 5)), a.word(n_(0x000A, 2)), a.word(n_(0x001A, 3));
    a.word(nm(0x0007, 4, 5)), a.word(n_(0x001A, 6));
    a.word(nm(0x200F, 4, 5)), a.word(n_(0x001A, 7));
    a.word(nm(0x200E, 4, 5)), a.word(kRts), a.word(n_(0x001A, 8));
    // loop: r0 = r4 + (r4-1) + ... + 1, with dt and bf/s
    a.label("loop");
    a.word(imm(0xE000, 0, 0));
    a.label("loop_top");
    a.word(nm(0x300C, 0, 4)), a.word(n_(0x4010, 4));
    a.b8(0x8F00, "loop_top"), a.word(kNop);
    a.word(kRts), a.word(kNop);
    // sw: SHC's switch: r4 in 0..2 -> 10, 20, 30; anything else -> 99
    a.label("sw");
    a.word(imm(0xE000, 1, 3)), a.word(nm(0x3002, 4, 1));  // mov #3,r1 ; cmp/hs r1,r4
    a.b8(0x8900, "sw_default");
    a.word(nm(0x6003, 0, 4)), a.word(n_(0x4000, 0)), a.word(nm(0x6003, 1, 0));  // mov r4,r0 ; shll r0 ; mov r0,r1
    a.mova("sw_table");
    a.word(nm(0x000D, 0, 1));  // mov.w @(r0,r1),r0
    const uint32_t braf_next = a.here() + 4;  // the braf's address + 4
    a.word(n_(0x0023, 0)), a.word(kNop);      // braf r0 ; nop
    a.align4();
    a.label("sw_table");
    a.word(0), a.word(0), a.word(0), a.word(kNop);
    a.label("sw_10"), a.word(kRts), a.word(imm(0xE000, 0, 10));
    a.label("sw_20"), a.word(kRts), a.word(imm(0xE000, 0, 20));
    a.label("sw_30"), a.word(kRts), a.word(imm(0xE000, 0, 30));
    a.label("sw_default"), a.word(kRts), a.word(imm(0xE000, 0, 99));
    // calls: saves PR, bsr inc (r4+1), jsr through the pool to dbl (x2), then tail-calls via bra to tail (+100)
    a.label("calls");
    a.word(0x4F22);  // sts.l pr,@-r15
    a.b12(0xB000, "inc"), a.word(kNop);
    a.movl_label(1, "dbl");
    a.word(n_(0x400B, 1)), a.word(kNop);
    a.word(0x4F26);  // lds.l @r15+,pr
    a.b12(0xA000, "tail"), a.word(kNop);
    a.pool();
    a.label("inc"), a.word(kRts), a.word(imm(0x7000, 4, 1));
    a.label("dbl"), a.word(kRts), a.word(nm(0x300C, 4, 4));
    a.label("tail"), a.word(0x2FE6), a.word(imm(0x7000, 4, 100)), a.word(kRts), a.word(0x6EF6);  // mov.l r14,@-r15 ; add ; rts ; mov.l @r15+,r14
    // indirect: r5 = the table's address; calls the r4-th function in it (inc or dbl)
    a.label("indirect");
    a.word(0x4F22);
    a.word(n_(0x4008, 4)), a.word(nm(0x6003, 0, 4)), a.word(nm(0x000E, 1, 5));  // shll2 r4 ; mov r4,r0 ; mov.l @(r0,r5),r1
    a.word(imm(0xE000, 4, 7));                                                   // r4 = 7
    a.word(n_(0x400B, 1)), a.word(kNop);
    a.word(0x4F26), a.word(kRts), a.word(kNop);
    // prcall: a call made by loading PR by hand, returning to "back", which returns through the saved PR
    a.label("prcall");
    a.word(n_(0x002A, 2));  // sts pr,r2
    a.movl_label(0, "inc");
    a.movl_label(1, "prcall_back");
    a.word(0x412A);  // lds r1,pr
    a.word(n_(0x402B, 0)), a.word(kNop);      // jmp @r0
    a.align4();
    a.label("prcall_back");
    a.word(imm(0x7000, 4, 1)), a.word(n_(0x402B, 2)), a.word(kNop);  // add #1,r4 ; jmp @r2 ; nop
    a.pool();
    // prbra: calls made by loading PR by hand and branching with bra (as the
    // engine's hand-written matrix code enters a shared tail), once with the
    // lds before the bra and once in its delay slot; each call adds 1 and so
    // does each place it returns to: r4 += 4
    a.label("prbra");
    a.word(0x4F22);                         // sts.l pr,@-r15
    a.movl_label(0, "prbra_back1");
    a.word(0x402A);                         // lds r0,pr
    a.b12(0xA000, "inc"), a.word(kNop);     // bra inc
    a.label("prbra_back1");
    a.word(imm(0x7000, 4, 1));              // add #1,r4
    a.movl_label(0, "prbra_back2");
    a.b12(0xA000, "inc");                   // bra inc
    a.word(0x402A);                         //  lds r0,pr
    a.label("prbra_back2");
    a.word(imm(0x7000, 4, 1));              // add #1,r4
    a.word(0x4F26), a.word(kRts), a.word(kNop);
    a.pool();
    // farjmp: a branch inside a function made through a register, as SHC does
    // where bra can't reach: r4 == 0 falls into the label with r0 = 1, anything
    // else jumps to it with r0 = 2 (set in the jmp's delay slot); then r0 += 40
    a.label("farjmp");
    a.word(0x4F22);                        // sts.l pr,@-r15
    a.word(nm(0x2008, 4, 4));              // tst r4,r4
    a.b8(0x8900, "farjmp_seq");            // bt
    a.movl_label(1, "farjmp_label");
    a.word(n_(0x402B, 1));                 // jmp @r1
    a.word(imm(0xE000, 0, 2));             //  mov #2,r0
    a.label("farjmp_seq");
    a.word(imm(0xE000, 0, 1));             // mov #1,r0
    a.label("farjmp_label");
    a.word(imm(0x7000, 0, 40));            // add #40,r0
    a.word(0x4F26), a.word(kRts), a.word(kNop);
    a.pool();
    // hang: a branch to itself, the game's stop on a fatal error
    a.label("hang");
    a.b12(0xA000, "hang"), a.word(kNop);
    // patch: rewrites its own literal (as the movie decoder patches its
    // dispatcher's offsets) and then loads it: r0 = 7, not the image's value.
    a.label("patch");
    a.movl_label(1, "patch_word");
    a.word(imm(0xE000, 2, 7)), a.word(nm(0x2002, 1, 2));  // mov #7,r2 ; mov.l r2,@r1
    a.movl_at(0, "patch_word");
    a.word(kRts), a.word(kNop);
    a.pool();
    a.label("patch_word");
    a.word(0x1111), a.word(0x1111);
    // badjump: a call to an address with no function
    a.label("badjump");
    a.word(0x4F22);
    a.movl(1, kBase + 0x4000);
    a.word(n_(0x400B, 1)), a.word(kNop), a.word(0x4F26), a.word(kRts), a.word(kNop);
    a.pool();
    // fpu: fmac in double precision, which the SH-4 doesn't define: not lifted
    a.label("fpu");
    a.movl(1, 0x000C0000u), a.word(n_(0x406A, 1)), a.word(nm(0xF00E, 2, 4));
    a.movl(1, 0x00040000u), a.word(n_(0x406A, 1)), a.word(kRts), a.word(kNop);
    a.pool();
    // memory: r4 = a scratch address, r5 a value: stores and loads of every
    // size, sign extension, pre-decrement and post-increment, GBR forms,
    // mac.w, tas.b
    a.label("memory");
    a.word(nm(0x6003, 6, 4)), a.word(imm(0x7000, 6, 16));                          // r6 = r4 + 16
    a.word(nm(0x2002, 4, 5));                                                       // mov.l r5,@r4
    a.word(nm(0x6000, 0, 4)), a.word(nm(0x6001, 1, 4)), a.word(nm(0x6002, 2, 4));  // r0, r1, r2: mov.b, mov.w, mov.l @r4
    a.word(nm(0x2004, 6, 5)), a.word(nm(0x2004, 6, 5)), a.word(nm(0x2005, 6, 5));  // mov.b, mov.b, mov.w r5,@-r6
    a.word(nm(0x6006, 3, 6));                                                       // mov.l @r6+,r3
    a.word(nm(0x6003, 14, 4)), a.word(imm(0x7000, 14, 64)), a.word(nm(0x2006, 14, 14));  // r14 = r4 + 64 ; mov.l r14,@-r14
    a.word(nm(0x6003, 12, 6));                                                      // r12 = r6
    a.word(n_(0x401E, 4));                                                          // ldc r4,gbr
    a.word(imm(0xE000, 0, 0)), a.word(0xCF80);                                      // mov #0,r0 ; or.b #0x80,@(r0,gbr)
    a.word(nm(0x6003, 13, 0)), a.word(0xC400), a.word(nm(0x6003, 7, 0));           // r13 = 0 ; mov.b @(0,gbr),r0 ; r7 = r0
    a.word(nm(0x6003, 8, 4)), a.word(nm(0x6003, 9, 4)), a.word(kClrmac);           // r8 = r9 = r4 ; clrmac
    a.word(nm(0x400F, 8, 9)), a.word(n_(0x001A, 10));                               // mac.w @r9+,@r8+ ; r10 = macl
    a.word(imm(0x7000, 4, 32)), a.word(n_(0x401B, 4)), a.word(n_(0x0029, 11));     // r4 += 32 ; tas.b @r4 ; r11 = T
    a.word(kRts), a.word(kNop);
    // farith: r4 -> three floats a, b, c; r5 -> eight results:
    // a+b, a-b, a*b, a/b, sqrt(c), -a, |b|, 1+a; r1 = a > b, r2 = a == b
    a.label("farith");
    a.word(nm(0xF009, 0, 4)), a.word(nm(0xF009, 1, 4)), a.word(nm(0xF009, 2, 4));  // fmov.s @r4+,fr0..fr2
    a.word(nm(0xF00C, 4, 0)), a.word(nm(0xF000, 4, 1));                            // fr4 = a ; fadd fr1,fr4
    a.word(nm(0xF00C, 5, 0)), a.word(nm(0xF001, 5, 1));                            // fsub fr1,fr5
    a.word(nm(0xF00C, 6, 0)), a.word(nm(0xF002, 6, 1));                            // fmul fr1,fr6
    a.word(nm(0xF00C, 7, 0)), a.word(nm(0xF003, 7, 1));                            // fdiv fr1,fr7
    a.word(nm(0xF00C, 8, 2)), a.word(n_(0xF06D, 8));                               // fsqrt fr8
    a.word(nm(0xF00C, 9, 0)), a.word(n_(0xF04D, 9));                               // fneg fr9
    a.word(nm(0xF00C, 10, 1)), a.word(n_(0xF05D, 10));                             // fabs fr10
    a.word(n_(0xF09D, 11)), a.word(nm(0xF000, 11, 0));                             // fldi1 fr11 ; fadd fr0,fr11
    a.word(nm(0xF005, 0, 1)), a.word(n_(0x0029, 1));                               // fcmp/gt fr1,fr0 ; movt r1
    a.word(nm(0xF004, 0, 1)), a.word(n_(0x0029, 2));                               // fcmp/eq fr1,fr0 ; movt r2
    a.word(imm(0xE000, 0, 0));
    for (unsigned k = 4; k < 12; ++k) a.word(nm(0xF007, 5, k)), a.word(imm(0x7000, 0, 4));  // fmov.s frk,@(r0,r5) ; add #4,r0
    a.word(kRts), a.word(kNop);
    // ftrc: r4 = a float's bits -> r0 = ftrc, r1 = the bits of float(r0)
    a.label("ftrc");
    a.word(n_(0x405A, 4)), a.word(n_(0xF00D, 0));  // lds r4,fpul ; fsts fpul,fr0
    a.word(n_(0xF03D, 0)), a.word(n_(0x005A, 0));  // ftrc fr0,fpul ; sts fpul,r0
    a.word(n_(0xF02D, 1)), a.word(n_(0xF01D, 1));  // float fpul,fr1 ; flds fr1,fpul
    a.word(n_(0x005A, 1)), a.word(kRts), a.word(kNop);
    // fpair: r4 -> four words in; r5 -> seven words out; 64-bit moves, XD, frchg.
    // r1 = FPSCR in the other bank, r2 = FPSCR at the end.
    a.label("fpair");
    a.word(0xF3FD);                                // fschg: 64-bit moves
    a.word(nm(0xF009, 0, 4)), a.word(nm(0xF009, 3, 4));  // fmov @r4+,dr0 ; fmov @r4+,xd2
    a.word(nm(0xF00C, 5, 0)), a.word(nm(0xF00C, 6, 3));  // fmov dr0,xd4 ; fmov xd2,dr6
    a.word(nm(0xF00A, 5, 6));                      // fmov dr6,@r5      -> out[0..1] = in[2..3]
    a.word(nm(0x6003, 6, 5)), a.word(imm(0x7000, 6, 16));
    a.word(nm(0xF00B, 6, 5));                      // fmov xd4,@-r6     -> out[2..3] = in[0..1]
    a.word(0xF3FD), a.word(0xFBFD);                // fschg ; frchg: the back bank is current
    a.word(imm(0xE000, 0, 16)), a.word(nm(0xF007, 5, 2));  // fmov.s fr2,@(r0,r5) -> out[4] = in[2]
    a.word(imm(0xE000, 0, 20)), a.word(nm(0xF007, 5, 5));  // fmov.s fr5,@(r0,r5) -> out[5] = in[1]
    a.word(n_(0x006A, 1)), a.word(0xFBFD);         // sts fpscr,r1 ; frchg
    a.word(imm(0xE000, 0, 24)), a.word(nm(0xF007, 5, 0));  // fmov.s fr0,@(r0,r5) -> out[6] = in[0]
    a.word(n_(0x006A, 2)), a.word(kRts), a.word(kNop);
    // fdouble: r4, r5 = floats a, b; in double precision: r0 = (float)(a+b),
    // r1 = (float)(a*b), r2 = (float)(a/b), r3 = a > b, r6:r7 = a/b's bits
    a.label("fdouble");
    a.movl(8, 0x000C0000u), a.word(n_(0x406A, 8));                  // lds r8,fpscr: PR = 1
    a.word(n_(0x405A, 4)), a.word(n_(0xF0AD, 0));                   // lds r4,fpul ; fcnvsd fpul,dr0
    a.word(n_(0x405A, 5)), a.word(n_(0xF0AD, 2));                   // fcnvsd fpul,dr2
    const auto copy0to4 = [&] { a.word(nm(0xF00C, 4, 0)), a.word(nm(0xF00C, 5, 1)); };
    copy0to4(), a.word(nm(0xF000, 4, 2)), a.word(n_(0xF0BD, 4)), a.word(n_(0x005A, 0));  // fadd dr2,dr4 ; fcnvds dr4,fpul ; sts fpul,r0
    copy0to4(), a.word(nm(0xF002, 4, 2)), a.word(n_(0xF0BD, 4)), a.word(n_(0x005A, 1));  // fmul
    copy0to4(), a.word(nm(0xF003, 4, 2)), a.word(n_(0xF0BD, 4)), a.word(n_(0x005A, 2));  // fdiv
    a.word(nm(0xF005, 0, 2)), a.word(n_(0x0029, 3));                // fcmp/gt dr2,dr0 ; movt r3
    a.word(n_(0xF01D, 4)), a.word(n_(0x005A, 6));                   // flds fr4,fpul ; sts fpul,r6
    a.word(n_(0xF01D, 5)), a.word(n_(0x005A, 7));
    a.movl(8, 0x00040000u), a.word(n_(0x406A, 8));                  // back to the default mode
    a.word(kRts), a.word(kNop);
    a.pool();
    // The approximate instructions (r4 -> inputs, r5 -> outputs).
    // fmac: out[0] = in[0] * in[1] + in[2] (fmac fr0,fr1,fr2)
    a.label("fmac");
    a.word(nm(0xF009, 0, 4)), a.word(nm(0xF009, 1, 4)), a.word(nm(0xF009, 2, 4));
    a.word(nm(0xF00E, 2, 1)), a.word(nm(0xF00A, 5, 2)), a.word(kRts), a.word(kNop);
    // fsrra: out[0] = 1 / sqrt(in[0]) (fsrra fr5)
    a.label("fsrra");
    a.word(nm(0xF009, 5, 4)), a.word(n_(0xF07D, 5)), a.word(nm(0xF00A, 5, 5)), a.word(kRts), a.word(kNop);
    // fipr: out[0] = in[0..3] . in[4..7] (fipr fv0,fv4: the result in fr7)
    a.label("fipr");
    for (unsigned k = 0; k < 8; ++k) a.word(nm(0xF009, k, 4));
    a.word(0xF4ED), a.word(nm(0xF00A, 5, 7)), a.word(kRts), a.word(kNop);
    // ftrv: the matrix in[0..15] (into the back bank) times in[16..19] (into fv8): out[0..3]
    a.label("ftrv");
    a.word(0xFBFD);
    for (unsigned k = 0; k < 16; ++k) a.word(nm(0xF009, k, 4));
    a.word(0xFBFD);
    for (unsigned k = 8; k < 12; ++k) a.word(nm(0xF009, k, 4));
    a.word(0xF9FD);  // ftrv xmtrx,fv8
    for (unsigned k = 8; k < 12; ++k) a.word(nm(0xF00A, 5, k)), a.word(imm(0x7000, 5, 4));
    a.word(kRts), a.word(kNop);
    // fsca: r4 = the angle; out[0] = sin, out[1] = cos (fsca fpul,dr6)
    a.label("fsca");
    a.word(n_(0x405A, 4)), a.word(n_(0xF0FD, 6)), a.word(nm(0xF00A, 5, 6)), a.word(imm(0x7000, 5, 4)), a.word(nm(0xF00A, 5, 7));
    a.word(kRts), a.word(kNop);
    // call_native: calls native_me, which the generator marks as replaced
    // (its stub goes through the runtime's registry), and adds 1 to its r0.
    a.label("call_native");
    a.word(0x4F22), a.b12(0xB000, "native_me"), a.word(kNop), a.word(0x4F26), a.word(kRts), a.word(imm(0x7000, 0, 1));
    a.label("native_me"), a.word(kRts), a.word(kNop);
    // data: the indirect-call table (inc, dbl), then scratch
    a.align4();
    a.label("data");
    a.long_of("inc"), a.long_of("dbl");
    for (int k = 0; k < 252; ++k) a.word(0);
    p.image = a.finish();
    p.data = a.at("data");
    for (const char* name : {"add64", "sub64", "neg64", "udiv", "sdiv", "bits", "mul", "loop", "sw", "calls", "indirect", "prcall", "prbra", "farjmp", "hang", "patch", "badjump",
                             "fpu", "memory", "farith", "ftrc", "fpair", "fdouble", "fmac", "fsrra", "fipr", "ftrv", "fsca", "call_native", "native_me", "inc", "dbl", "tail"})
        p.entry[name] = a.at(name);
    // Fill the switch table: offsets from the braf's address + 4.
    const size_t t = (a.at("sw_table") - kBase) / 2;
    p.image[t] = static_cast<uint16_t>(a.at("sw_10") - braf_next);
    p.image[t + 1] = static_cast<uint16_t>(a.at("sw_20") - braf_next);
    p.image[t + 2] = static_cast<uint16_t>(a.at("sw_30") - braf_next);
    return p;
}

}  // namespace qilin::test
