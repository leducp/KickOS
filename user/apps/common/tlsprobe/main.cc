// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The thread_local witness: each thread reads back what IT wrote, the storage is at a DIFFERENT
// address per thread, and a .tdata initialiser arrives. Each thread's libc state lies below its
// thread-local storage and keeps the thread's own errno. A thread_local's destructor runs once at
// its own thread's end, and exit() runs only main's. Where the board holds a second task, main
// creates its peer twice, the program tlsprobe_peer in an image of its own whose thread-local
// storage is another span of the link's one thread-local segment (peer.cc).

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/atomic.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>

namespace
{
// By ISA: only M-profile ARM and RX give unprivileged code no per-thread register, so every
// other arch owes a caller stack of any size its own thread-local block.
#if defined(__arm__) or defined(__RX__)
#define TLSPROBE_CALLER_STACK 0
#else
#define TLSPROBE_CALLER_STACK 1
#endif
#if not TLSPROBE_CALLER_STACK
    constexpr int WORKERS = 2;
#else
    // The last worker runs on a stack its caller supplies: half a stride, so neither one stride
    // wide nor, in general, stride-aligned. It must be admitted and still get its own block.
    constexpr int WORKERS = 3;
    constexpr uint32_t CALLER_STACK = KICKOS_TLS_STRIDE / 2u;
    static_assert(CALLER_STACK >= KICKOS_MIN_STACK_SIZE, "below the per-arch stack floor");
#endif

    // .tdata (a non-zero initialiser) and .tbss (no initialiser).
    //
    // VOLATILE ON THE SEEDED ONE OR THE CHECK IS VACUOUS: a thread_local that is only ever read
    // is constant-folded at -Os, .tdata comes out EMPTY, and comparing the read against the
    // initialiser then proves nothing about any template.
    thread_local volatile unsigned g_seeded = 0xA5A5A5A5u;
    thread_local unsigned g_written = 0;

    // A thread's first use of any thread_local below registers the destructors of both, in this
    // order, so at main's exit() main's t_named is destroyed before its t_verdict, which prints
    // the verdict once main arms it. Nothing atexit registered has run yet.
    struct Verdict
    {
        int bad = -1;
        ~Verdict();
    };
    thread_local Verdict t_verdict;

    // Counts its destructor's runs on the thread it names, and returns its name to the heap.
    struct Named
    {
        char* name = nullptr;
        int slot = -1;
        void set(char const* what, int which);
        ~Named();
    };
    thread_local Named t_named;

    // Zero-initialised. Sized with the two above so each task image's block fits a 512-byte idle
    // stack and the images' blocks together overflow it: the link holds idle to the init's block
    // alone.
    constexpr unsigned PAD_WORDS
        = 35u - (sizeof(Named) + sizeof(Verdict) + sizeof(void*)) / sizeof(unsigned);
    thread_local volatile unsigned g_main_pad[PAD_WORDS];

    // `done` PUBLISHES the fields above it: the worker stores it last, main reads it first.
    struct Report
    {
        unsigned addr;
        unsigned sp;
        unsigned seeded;
        unsigned read_back;
        unsigned zeroed;
        unsigned state;
        unsigned seeded_at;
        int err;
        kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE> done;
    };

    unsigned read_sp()
    {
        unsigned sp = 0;
#if defined(__arm__)
        __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif defined(__aarch64__)
        // Through a 64-bit temporary: `mov w0, sp` is not an encoding, and the report field
        // is 32 bits, which every address on the boards that run this fits in.
        unsigned long long sp64 = 0;
        __asm__ volatile("mov %0, sp" : "=r"(sp64));
        sp = static_cast<unsigned>(sp64);
#elif defined(__riscv)
        __asm__ volatile("mv %0, sp" : "=r"(sp));
#elif defined(__XTENSA__)
        __asm__ volatile("mov %0, a1" : "=r"(sp));
#elif defined(__RX__)
        __asm__ volatile("mov.l r0, %0" : "=r"(sp));
#endif
        return sp;
    }

    Report g_report[WORKERS] = {};

    // Set once main has read every report: each worker then ends by returning.
    kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE> g_release{0};
    // Each worker's, then main's.
    kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_dtor_runs[WORKERS + 1] = {};

    void Named::set(char const* what, int which)
    {
        name = static_cast<char*>(malloc(strlen(what) + 1u));
        if (name != nullptr)
        {
            strcpy(name, what);
        }
        slot = which;
    }

    Named::~Named()
    {
        free(name);
        if (slot >= 0)
        {
            g_dtor_runs[slot] = g_dtor_runs[slot] + 1u;
        }
    }

    Verdict::~Verdict()
    {
        if (bad < 0)
        {
            return;
        }
        int b = bad;
        char d[96];
        for (int k = 0; k < WORKERS; k++)
        {
            if (g_dtor_runs[k] != 1u)
            {
                ksnprintf(d, sizeof(d),
                          "[tlsprobe] at exit() w%d's thread_local destructor ran %u times\n", k,
                          static_cast<unsigned>(g_dtor_runs[k]));
                kos::print(d);
                b++;
            }
        }
        if (g_dtor_runs[WORKERS] != 1u)
        {
            kos::print("[tlsprobe] exit() did not run main's thread_local destructor\n");
            b++;
        }
        if (b == 0)
        {
            kos::print("[tlsprobe] PASS\n");
        }
        else
        {
            kos::print("[tlsprobe] FAIL\n");
        }
    }

#if defined(__arm__) and defined(KICKOS_TLS_STRIDE)
#define TLSPROBE_EDGE 1
#define TLSPROBE_STR(x) #x
#define TLSPROBE_XSTR(x) TLSPROBE_STR(x)
#define TLSPROBE_KEEP_R1 0x11111111
#define TLSPROBE_KEEP_R2 0x22222222
#define TLSPROBE_KEEP_R3 0x33333333
#else
#define TLSPROBE_EDGE 0
#endif
}

#if TLSPROBE_EDGE
// What tlsprobe_edge stores, in this order: the block __aeabi_read_tp answers from where the
// thread stands, the one it answers with SP at that block's exclusive top, and r1-r3 after it.
extern "C" unsigned g_edge[5];
unsigned g_edge[5] = {};
kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE> g_edge_done{0};

extern "C" [[noreturn]] void tlsprobe_edge_park(void)
{
    g_edge_done = 1;
    while (true)
    {
        kos::sleep_ns(1000000000ull);
    }
}

// An empty stack's SP sits at its block's exclusive top, which no C code reaches, so this
// thread's entry is assembly. It keeps nothing on its own stack and never returns: an interrupt
// taken while SP is at the top stacks its frame over this block's outermost frames. The thread
// pointer is KICKOS_REENT_CARVE above the block's base, past the libc state.
extern "C" void tlsprobe_edge(void*);
__asm__(".syntax unified\n"
        ".pushsection .text.tlsprobe_edge, \"ax\", %progbits\n"
        ".balign 2\n"
        ".thumb_func\n"
        ".type tlsprobe_edge, %function\n"
        "tlsprobe_edge:\n"
        "    bl      __aeabi_read_tp\n"
        "    mov     r5, r0\n"
        "    ldr     r4, =" TLSPROBE_XSTR(KICKOS_TLS_STRIDE) " - "
        TLSPROBE_XSTR(KICKOS_REENT_CARVE) "\n"
        "    adds    r0, r0, r4\n"
        "    ldr     r1, =" TLSPROBE_XSTR(TLSPROBE_KEEP_R1) "\n"
        "    ldr     r2, =" TLSPROBE_XSTR(TLSPROBE_KEEP_R2) "\n"
        "    ldr     r3, =" TLSPROBE_XSTR(TLSPROBE_KEEP_R3) "\n"
        "    mov     r4, sp\n"
        "    mov     sp, r0\n"
        "    bl      __aeabi_read_tp\n"
        "    mov     sp, r4\n"
        "    ldr     r4, =g_edge\n"
        "    str     r5, [r4, #0]\n"
        "    str     r0, [r4, #4]\n"
        "    str     r1, [r4, #8]\n"
        "    str     r2, [r4, #12]\n"
        "    str     r3, [r4, #16]\n"
        "    bl      tlsprobe_edge_park\n"
        "    .ltorg\n"
        ".size tlsprobe_edge, . - tlsprobe_edge\n"
        ".popsection\n");
#endif

namespace
{
    int want_err(int k)
    {
        if ((k & 1) == 0)
        {
            return EINVAL;
        }
        return ERANGE;
    }

    // The lowest of the worker's thread_locals as it saw them.
    unsigned tls_lo(Report const& r)
    {
        unsigned lo = r.addr;
        if (r.seeded_at < lo)
        {
            lo = r.seeded_at;
        }
        return lo;
    }

    void worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        unsigned const mine = 0xC0DE0000u + static_cast<unsigned>(k);
        // Read BEFORE the write, so a template that never arrived is visible as a wrong seed
        // rather than as a zero nobody can attribute.
        unsigned const seeded = g_seeded;
        unsigned const zeroed = g_main_pad[0] | g_main_pad[PAD_WORDS - 1u];
        t_named.set("tlsprobe worker thread", k);
        g_seeded = 0;
        g_written = mine;
        g_main_pad[0] = mine;
        char* end = nullptr;
        if ((k & 1) == 0)
        {
            (void)strtol("10", &end, 99);
        }
        else
        {
            (void)strtol("99999999999999999999999999", &end, 10);
        }
        // A round trip through the scheduler: on an arch whose thread pointer is a register,
        // this is where a switch that forgot to restore it shows up.
        kos::sleep_ns(50000000ull);
        g_report[k].err = errno;
        g_report[k].state = static_cast<unsigned>(reinterpret_cast<uintptr_t>(_REENT));
        g_report[k].seeded_at = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_seeded));
        g_report[k].addr = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_written));
        g_report[k].sp = read_sp();
        g_report[k].seeded = seeded;
        g_report[k].read_back = g_written;
        g_report[k].zeroed = zeroed;
        g_report[k].done = 1;
        while (g_release == 0)
        {
            kos::sleep_ns(10000000ull);
        }
    }
}

namespace
{
    // The peer's exit status, started once its last incarnation's members are dead, or a value no
    // status takes. Main writes its own thread-local storage while the peer runs.
    int run_peer(kos_program_t prog)
    {
        kos_task_params p{};
        p.instance = "peer";
        kos_task_t t = KOS_TASK_NONE;
        int const rc = kos_task_create(prog, &p, 100000u, &t);
        if (rc != 0)
        {
            return -1000 + rc;
        }
        for (unsigned i = 0; i < 1000u; i++)
        {
            g_written = 0x3A3A0000u + i;
            int status = 0;
            int const state = kos_task_state(t, &status);
            if (state >= 0 and (state & KOS_TASK_DEAD) != 0)
            {
                return status;
            }
            kos::sleep_ns(1000000ull);
        }
        return -2000;
    }
}

int main(int, char**)
{
    kos::print("[tlsprobe] start\n");
    int bad = 0;
    char b[96];
    {
        int status[2] = {-3000, -3000};
        kos_program_t prog = KOS_CAP_NONE;
        if (kos_program_find("tlsprobe_peer", &prog) == 0)
        {
            status[0] = run_peer(prog);
            status[1] = run_peer(prog);
            (void)kos_handle_close(prog);
        }
        if (status[0] == 0 and status[1] == 0)
        {
            kos::print("[tlsprobe] peer PASS\n");
        }
        else
        {
            ksnprintf(b, sizeof(b), "[tlsprobe] peer FAIL %d then %d\n", status[0], status[1]);
            kos::print(b);
            bad++;
        }
    }

    unsigned const main_addr = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_written));
    {
        char d[72];
        ksnprintf(d, sizeof(d), "[tlsprobe] main tp %x sp %x\n", main_addr, read_sp());
        kos::print(d);
    }
    g_written = 0x4F4F5400u;

    for (int k = 0; k < 2; k++)
    {
        kos::thread::create(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(k)),
                            "tlsw", 10);
    }
#if TLSPROBE_EDGE
    kos::thread::create(tlsprobe_edge, nullptr, "tlse", 10);
#endif
#if TLSPROBE_CALLER_STACK
    {
        // Allocation grants nothing, and where a backend translates the block is not mapped.
        void* const stack = kos_ram_alloc(CALLER_STACK);
        int rc = -1;
        if (stack != nullptr and kos_mem_self_grant(stack, CALLER_STACK, 0) == 0)
        {
            rc = kos::thread::create(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(2)),
                                     "tlsc", 10, KOS_POLICY_FIFO, 0, stack, CALLER_STACK)
                     .error();
        }
        char d[72];
        ksnprintf(d, sizeof(d), "[tlsprobe] w2 caller stack %x size %x spawn %d\n",
                  static_cast<unsigned>(reinterpret_cast<uintptr_t>(stack)),
                  static_cast<unsigned>(CALLER_STACK), rc);
        kos::print(d);
    }
#endif

    for (int spin = 0; spin < 200; spin++)
    {
        int ready = 0;
        for (int k = 0; k < WORKERS; k++)
        {
            ready += static_cast<int>(g_report[k].done);
        }
#if TLSPROBE_EDGE
        ready += static_cast<int>(g_edge_done);
        if (ready == WORKERS + 1)
#else
        if (ready == WORKERS)
#endif
        {
            break;
        }
        kos::sleep_ns(20000000ull);
    }

    for (int k = 0; k < WORKERS; k++)
    {
        unsigned const want = 0xC0DE0000u + static_cast<unsigned>(k);
        char const* verdict = "ok";
        if (g_report[k].done == 0)
        {
            verdict = "NEVER RAN";
            bad++;
        }
        else if (g_report[k].read_back != want)
        {
            verdict = "READ BACK WRONG";
            bad++;
        }
        else if (g_report[k].seeded != 0xA5A5A5A5u)
        {
            verdict = "TEMPLATE MISSING";
            bad++;
        }
        else if (g_report[k].zeroed != 0u)
        {
            verdict = "ZERO-INITIALISED NOT ZERO";
            bad++;
        }
        else if (g_report[k].addr == main_addr)
        {
            verdict = "SHARED WITH MAIN";
            bad++;
        }
        else if (g_report[k].err != want_err(k))
        {
            verdict = "ERRNO NOT ITS OWN";
            bad++;
        }
        // The state sits below the thread pointer, so no part of it is a thread_local.
        else if (g_report[k].state + KICKOS_REENT_CARVE > tls_lo(g_report[k]))
        {
            verdict = "STATE OVERLAPS THREAD_LOCAL";
            bad++;
        }
        else if (tls_lo(g_report[k]) - g_report[k].state >= KICKOS_TLS_STRIDE)
        {
            verdict = "STATE NOT IN THE THREAD'S BLOCK";
            bad++;
        }
        // THE BLOCK MUST BE THE ONE THIS THREAD IS STANDING ON: a thread pointer derived from SP
        // names the NEIGHBOUR's block when SP sits exactly at an exclusive stack top, which
        // checking the two against each other makes visible here rather than as a data abort in
        // whichever thread happens to be adjacent.
        else if (g_report[k].addr > g_report[k].sp)
        {
            verdict = "BLOCK ABOVE SP";
            bad++;
        }
#if defined(KICKOS_TLS_STRIDE)
        else if (g_report[k].sp - g_report[k].addr >= KICKOS_TLS_STRIDE)
        {
            verdict = "BLOCK MORE THAN ONE STRIDE BELOW SP";
            bad++;
        }
#endif
        ksnprintf(b, sizeof(b), "[tlsprobe] w%d tp %x sp %x seed %x read %x state %x %s\n",
                  k, g_report[k].addr, g_report[k].sp, g_report[k].seeded,
                  g_report[k].read_back, g_report[k].state, verdict);
        kos::print(b);
    }
    for (int i = 0; i < WORKERS; i++)
    {
        for (int j = i + 1; j < WORKERS; j++)
        {
            if (g_report[i].done != 0 and g_report[j].done != 0
                and g_report[i].addr == g_report[j].addr)
            {
                ksnprintf(b, sizeof(b), "[tlsprobe] w%d and w%d SHARE ONE BLOCK\n", i, j);
                kos::print(b);
                bad++;
            }
        }
    }
#if TLSPROBE_EDGE
    {
        char const* verdict = "ok";
        if (g_edge_done == 0)
        {
            verdict = "NEVER RAN";
            bad++;
        }
        else if (g_edge[1] != g_edge[0])
        {
            verdict = "EDGE NAMES ANOTHER BLOCK";
            bad++;
        }
        else if (g_edge[2] != TLSPROBE_KEEP_R1 or g_edge[3] != TLSPROBE_KEEP_R2
                 or g_edge[4] != TLSPROBE_KEEP_R3)
        {
            verdict = "R1-R3 CLOBBERED";
            bad++;
        }
        char e[112];
        ksnprintf(e, sizeof(e), "[tlsprobe] edge tp %x at top %x r1-r3 %x %x %x %s\n",
                  g_edge[0], g_edge[1], g_edge[2], g_edge[3], g_edge[4], verdict);
        kos::print(e);
    }
#endif
    if (g_written != 0x4F4F5400u)
    {
        kos::print("[tlsprobe] MAIN COPY CLOBBERED\n");
        bad++;
    }
    ksnprintf(b, sizeof(b), "[tlsprobe] main addr %x read %x\n", main_addr, g_written);
    kos::print(b);

    g_release = 1;
    for (int spin = 0; spin < 200; spin++)
    {
        int ended = 0;
        for (int k = 0; k < WORKERS; k++)
        {
            ended += static_cast<int>(g_dtor_runs[k] != 0u);
        }
        if (ended == WORKERS)
        {
            break;
        }
        kos::sleep_ns(20000000ull);
    }
    for (int k = 0; k < WORKERS; k++)
    {
        char const* verdict = "ok";
        if (g_dtor_runs[k] != 1u)
        {
            verdict = "NOT ONCE AT ITS END";
            bad++;
        }
        ksnprintf(b, sizeof(b), "[tlsprobe] w%d thread_local destructor runs %u %s\n", k,
                  static_cast<unsigned>(g_dtor_runs[k]), verdict);
        kos::print(b);
    }

    t_named.set("tlsprobe main thread", WORKERS);
    t_verdict.bad = bad;
    // Returning from main is a kos_shutdown(0), which is what ends the qemu run.
    return 0;
}
