// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The thread_local witness: each thread reads back what IT wrote, the storage is at a DIFFERENT
// address per thread, and a .tdata initialiser arrives.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/atomic.h>

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

    // `done` PUBLISHES the four fields above it: the worker stores it last, main reads it first.
    struct Report
    {
        unsigned addr;
        unsigned sp;
        unsigned seeded;
        unsigned read_back;
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
// taken while SP is at the top stacks its frame over this block's outermost frames.
extern "C" void tlsprobe_edge(void*);
__asm__(".syntax unified\n"
        ".pushsection .text.tlsprobe_edge, \"ax\", %progbits\n"
        ".balign 2\n"
        ".thumb_func\n"
        ".type tlsprobe_edge, %function\n"
        "tlsprobe_edge:\n"
        "    bl      __aeabi_read_tp\n"
        "    mov     r5, r0\n"
        "    ldr     r4, =" TLSPROBE_XSTR(KICKOS_TLS_STRIDE) "\n"
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
    void worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        unsigned const mine = 0xC0DE0000u + static_cast<unsigned>(k);
        // Read BEFORE the write, so a template that never arrived is visible as a wrong seed
        // rather than as a zero nobody can attribute.
        unsigned const seeded = g_seeded;
        g_seeded = 0;
        g_written = mine;
        // A round trip through the scheduler: on an arch whose thread pointer is a register,
        // this is where a switch that forgot to restore it shows up.
        kos::sleep_ns(50000000ull);
        g_report[k].addr = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_written));
        g_report[k].sp = read_sp();
        g_report[k].seeded = seeded;
        g_report[k].read_back = g_written;
        g_report[k].done = 1;
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    kos::print("[tlsprobe] start\n");

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
                                     "tlsc", 10, KOS_POLICY_FIFO, 0, false, nullptr, 0, stack,
                                     CALLER_STACK)
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

    int bad = 0;
    char b[80];
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
        else if (g_report[k].addr == main_addr)
        {
            verdict = "SHARED WITH MAIN";
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
        ksnprintf(b, sizeof(b), "[tlsprobe] w%d tp %x sp %x seed %x read %x %s\n", k,
                  g_report[k].addr, g_report[k].sp, g_report[k].seeded,
                  g_report[k].read_back, verdict);
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

    if (bad == 0)
    {
        kos::print("[tlsprobe] PASS\n");
    }
    else
    {
        kos::print("[tlsprobe] FAIL\n");
    }
    // Returning from main is a kos_shutdown(0), which is what ends the qemu run.
    return 0;
}
