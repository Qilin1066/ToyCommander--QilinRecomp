// Runtime code: no std::format.
#include "system.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <vector>

namespace qilin::rt {

namespace {

std::vector<Provided>& registry() {
    static std::vector<Provided> r;  // sorted by entry
    return r;
}

constexpr uint32_t kHostReturn = 0xFFFFFFF0u;  // where call_guest's calls return: no guest code is there

const char* kind_name(Stop::Kind k) {
    switch (k) {
    case Stop::Memory: return "an access outside guest memory";
    case Stop::BadJump: return "a jump or call to an address with no function";
    case Stop::BadReturn: return "a return to an unexpected address";
    case Stop::Trap: return "a trap";
    case Stop::NotLifted: return "a function that isn't lifted";
    case Stop::Unsupported: return "an operation the port doesn't support";
    case Stop::Unregistered: return "a replaced function with no native code yet";
    }
    return "?";
}

}  // namespace

void provide(std::span<const Provided> natives) {
    auto& r = registry();
    r.insert(r.end(), natives.begin(), natives.end());
    std::sort(r.begin(), r.end(), [](const Provided& a, const Provided& b) { return a.entry < b.entry; });
    for (size_t i = 1; i < r.size(); ++i)
        if (r[i - 1].entry == r[i].entry) fatal("%08X has native code twice (%s, %s)", r[i].entry, r[i - 1].name, r[i].name);
}

const char* native_name(uint32_t entry) {
    const auto& r = registry();
    const auto it = std::lower_bound(r.begin(), r.end(), entry, [](const Provided& p, uint32_t a) { return p.entry < a; });
    return it != r.end() && it->entry == entry ? it->name : nullptr;
}

void replaced(Context& c, uint32_t fn) {
    const auto& r = registry();
    const auto it = std::lower_bound(r.begin(), r.end(), fn, [](const Provided& p, uint32_t a) { return p.entry < a; });
    if (it == r.end() || it->entry != fn) unregistered(fn, c.pr);
    it->fn(c);
    c.ret = c.pr;
}

uint32_t function_containing(uint32_t address) {
    const FnEntry* end = g_functions + g_function_count;
    const FnEntry* it = std::upper_bound(g_functions, end, address, [](uint32_t a, const FnEntry& e) { return a < e.addr; });
    return it == g_functions ? 0 : (it - 1)->addr;
}

void call_guest(Context& c, uint32_t entry) {
    const uint32_t saved = c.pr;
    c.pr = kHostReturn;
    call(c, entry, kHostReturn);
    if (c.ret != kHostReturn) bad_return(entry, c.ret);
    c.pr = saved;
}

void report(std::FILE* out, const Stop& s, const Context* c) {
    std::fprintf(out, "stopped: %s\n", kind_name(s.kind));
    switch (s.kind) {
    case Stop::Unregistered:
    case Stop::NotLifted:
        std::fprintf(out, "  function fn_%08X", s.pc);
        if (s.kind == Stop::Unregistered) {
            if (s.value == kHostReturn) std::fprintf(out, ", called by the runtime");
            else std::fprintf(out, ", called with the return address %08X (in fn_%08X)", s.value, function_containing(s.value));
        }
        std::fprintf(out, "\n");
        break;
    default:
        std::fprintf(out, "  at %08X (in fn_%08X), value %08X\n", s.pc, function_containing(s.pc), s.value);
        break;
    }
    if (c) {
        for (int i = 0; i < 16; ++i) std::fprintf(out, "  r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
        if (c->pr == kHostReturn) std::fprintf(out, "  pr  %08X (the runtime)", c->pr);
        else std::fprintf(out, "  pr  %08X (in fn_%08X)", c->pr, function_containing(c->pr));
        std::fprintf(out, "  gbr %08X  t %u  fpscr %08X  fpul %08X\n", c->gbr, c->t, c->fpscr, c->fpul);
    }
}

void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::fputs("qilin_run: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
    std::exit(3);
}

void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

}  // namespace qilin::rt
