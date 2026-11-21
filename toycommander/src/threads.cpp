#include "threads.hpp"

#include <condition_variable>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "system.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

// The guest's records (all guest RAM):
//   0x8C124280  the scheduler running now; 0x8C124284 the one before it
//   a scheduler: +0x204 its tick count, +0x208 its thread slots,
//     +0x210 the threads (0x124 bytes each), +0xD78 the thread being run
//   a thread: +12 its state (0 free, 1 ready, 2 to kill, 3 dead), +20 the
//     tick it may run from, +24 its saved registers (0x8C09CDCC's layout:
//     +0 r15, +4 r14, +8 pr, +12 r13 ... +48 r4, +52 r3 ... +64 r0, +68
//     mach, +72 macl, +76 gbr, +80 fr0-fr15, +144 fpul, +148 fpscr)
constexpr uint32_t kCurrent = 0x8C124280u, kPrevious = 0x8C124284u;
constexpr uint32_t kTicks = 0x204u, kSlots = 0x208u, kThreads = 0x210u, kRunning = 0xD78u, kThreadBytes = 0x124u;
constexpr uint32_t kExit = 0x8C0C4972u;   // where a killed thread's saved PR is pointed
constexpr uint32_t kCleanup = 0x8C0C48E6u;  // a dead thread's slot freed (lifted)

// A thread being killed unwinds its host thread with this.
struct Killed {};

// One guest thread on a host thread.
struct Stream {
    uint32_t record = 0;
    rt::Context c;
    bool started = false, killed = false, finished = false;
    std::exception_ptr stop;  // a stop in the guest thread, for its scheduler
    std::thread host;
};

std::mutex g_lock;
std::condition_variable g_turn;
const void* g_running = nullptr;  // the stream (or the scheduler's caller: nullptr's stand-in below) that runs
std::map<uint32_t, std::unique_ptr<Stream>> g_streams;  // by thread record
Stream* g_current = nullptr;                             // the guest thread running, or null (a scheduler's caller)
std::vector<const void*> g_resume;                       // who runs again when the current thread yields
const int g_caller_token = 0;                            // stands for host code outside any guest thread
rt::Context g_stop_context;
bool g_have_stop_context = false;

const void* self() { return g_current ? static_cast<const void*>(g_current) : &g_caller_token; }

// Hands the turn to `to` and waits for it to come back to `me`.
void hand_over(std::unique_lock<std::mutex>& lock, const void* to, const void* me) {
    g_running = to;
    g_turn.notify_all();
    g_turn.wait(lock, [&] { return g_running == me; });
}

// Host threads need room: lifted code nests as deeply as the guest calls.
template <class F>
std::thread big_stack_thread(F f) {
#if defined(_WIN32)
    // std::thread can't set a stack size; the default reserve is set by the
    // linker. MSVC builds link qilin_run with a large one (CMakeLists.txt).
    return std::thread(std::move(f));
#else
    // On macOS secondary threads get 512 KB: run the body on a pthread with
    // 16 MB, and let the std::thread wait for it.
    return std::thread([f = std::move(f)]() mutable {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16u << 20);
        pthread_t t;
        auto* body = new F(std::move(f));
        if (pthread_create(&t, &attr, [](void* p) -> void* {
                std::unique_ptr<F> g(static_cast<F*>(p));
                (*g)();
                return nullptr;
            }, body) != 0)
            rt::fatal("cannot start a host thread for a guest thread");
        pthread_attr_destroy(&attr);
        pthread_join(t, nullptr);
    });
#endif
}

// A guest thread's first run: its registers from its record, r0 = 1 (what
// the restore that starts it returns), from the saved PR, which is its
// entry; its code returns there too (PR stays the entry), so it is entered
// again until it switches away for good.
void begin(Stream& s) {
    const uint32_t b = s.record + 24;
    rt::Context& c = s.c;
    c.r[15] = rt::ld32(b), c.r[14] = rt::ld32(b + 4), c.pr = rt::ld32(b + 8);
    for (unsigned k = 0; k < 10; ++k) c.r[13 - k] = rt::ld32(b + 12 + 4 * k);  // r13 down to r4
    c.r[3] = rt::ld32(b + 52), c.r[2] = rt::ld32(b + 56), c.r[1] = rt::ld32(b + 60);
    c.mach = rt::ld32(b + 68), c.macl = rt::ld32(b + 72), c.gbr = rt::ld32(b + 76);
    for (unsigned k = 0; k < 16; ++k) c.fr[k] = rt::flt(rt::ld32(b + 80 + 4 * k));
    c.fpul = rt::ld32(b + 144), c.fpscr = rt::ld32(b + 148);
    c.r[0] = 1;
    s.started = true;
    Stream* p = &s;
    s.host = big_stack_thread([p] {
        rt::host_fp_setup();
        std::unique_lock<std::mutex> lock(g_lock);
        g_turn.wait(lock, [&] { return g_running == p; });
        try {
            const uint32_t entry = p->c.pr;
            lock.unlock();
            for (;;) rt::call_guest(p->c, entry);
        } catch (const Killed&) {
            if (!lock.owns_lock()) lock.lock();
        } catch (...) {
            if (!lock.owns_lock()) lock.lock();
            p->stop = std::current_exception();
            g_stop_context = p->c, g_have_stop_context = true;
        }
        p->finished = true;
        // Back to whoever ran it.
        g_running = g_resume.empty() ? &g_caller_token : g_resume.back();
        g_turn.notify_all();
    });
}

// Runs guest thread `record` until it yields (or ends), as restoring its
// saved registers does. The saved registers don't include SR: a thread runs
// at the interrupt mask of whoever runs the pass, as on the console. (The
// Maple driver's DMA-end handler runs a pass at level 4: a thread must not
// take another interrupt there and run the pass inside itself.)
void run(uint32_t record, uint32_t caller_sr) {
    auto& slot = g_streams[record];
    if (!slot) slot = std::make_unique<Stream>(), slot->record = record;
    Stream& s = *slot;
    if (s.finished) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C4A7Cu, record};  // ended without dying first
    Stream* const outer = g_current;
    const void* me = self();
    std::unique_lock<std::mutex> lock(g_lock);
    g_resume.push_back(me);
    g_current = &s;
    s.c.sr = (s.c.sr & ~0xF0u) | (caller_sr & 0xF0u);
    if (!s.started) begin(s);
    hand_over(lock, &s, me);
    g_resume.pop_back();
    g_current = outer;
    if (s.stop) {
        const std::exception_ptr e = s.stop;
        s.stop = nullptr;
        lock.unlock();
        std::rethrow_exception(e);
    }
}

// A dead thread's host thread is unwound and its stream forgotten.
void bury(uint32_t record) {
    const auto it = g_streams.find(record);
    if (it == g_streams.end()) return;
    Stream& s = *it->second;
    if (s.started && !s.finished) {
        std::unique_lock<std::mutex> lock(g_lock);
        s.killed = true;
        g_resume.push_back(self());
        hand_over(lock, &s, self());
        g_resume.pop_back();
    }
    if (s.host.joinable()) s.host.join();
    g_streams.erase(it);
}

// 0x8C0C4A7C: one pass of scheduler r4. Each ready thread whose tick has come
// runs until it yields; after every yield the pass starts again from the
// first thread, as the guest code does (it resumes where it saved its own
// registers, before the loop). Threads to kill are marked dead (their saved
// PR pointed at the exit, as the guest code does, though a dead thread is
// never run again); dead ones are freed (0x8C0C48E6, lifted). Then the tick
// count goes up and the previous scheduler is current again.
void scheduler_pass(rt::Context& c) {
    const uint32_t sched = c.r[4];
    rt::st32(kPrevious, rt::ld32(kCurrent));
    rt::st32(kCurrent, sched);
    for (uint32_t i = 0; i < rt::ld32(sched + kSlots);) {
        const uint32_t rec = sched + kThreads + i * kThreadBytes;
        rt::st32(sched + kRunning, rec);
        switch (rt::ld32(rec + 12)) {
        case 3: {
            bury(rec);
            rt::Context k = c;
            k.r[4] = sched, k.r[5] = rec;
            rt::call_guest(k, kCleanup);
            ++i;
            break;
        }
        case 1:
            if (rt::ld32(rec + 20) <= rt::ld32(sched + kTicks)) {
                run(rec, c.sr);
                i = 0;
            } else {
                ++i;
            }
            break;
        case 2:
            rt::st32(rec + 12, 3), rt::st32(rec + 24 + 8, kExit);
            ++i;
            break;
        default: ++i; break;
        }
    }
    rt::st32(sched + kTicks, rt::ld32(sched + kTicks) + 1);
    rt::st32(kCurrent, rt::ld32(kPrevious));
}

// 0x8C0C4A3A: the running thread yields for r4 ticks (r4 < 0: until woken),
// back to its scheduler; returns 1 when it runs again, as the restore that
// resumes it makes its save return. (The guest code also saves its registers
// in its record; nothing reads them but the restore, which this replaces.)
void thread_yield(rt::Context& c) {
    Stream* const s = g_current;
    if (!s) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C4A3Au, 0};  // not in a guest thread
    const uint32_t sched = rt::ld32(kCurrent), rec = rt::ld32(sched + kRunning);
    if (rec != s->record) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C4A3Au, rec};
    const int32_t n = static_cast<int32_t>(c.r[4]);
    rt::st32(rec + 20, n >= 0 ? rt::ld32(sched + kTicks) + static_cast<uint32_t>(n) : 0x7FFFFFFFu);
    {
        std::unique_lock<std::mutex> lock(g_lock);
        hand_over(lock, g_resume.back(), s);
        if (s->killed) throw Killed{};
    }
    c.r[0] = 1;
}

constexpr rt::Provided kNatives[] = {
    {0x8C0C4A3Au, "thread_yield", thread_yield},
    {0x8C0C4A7Cu, "thread_scheduler_pass", scheduler_pass},
};

}  // namespace

void provide_threads() {
    g_running = &g_caller_token;
    rt::provide(kNatives);
}

const rt::Context* thread_stop_context() { return g_have_stop_context ? &g_stop_context : nullptr; }

}  // namespace toycommander
