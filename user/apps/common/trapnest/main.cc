// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test RV32 nested traps during syscall dispatch. Syscalls must run on the thread's kernel stack
// so an M-mode interrupt cannot use the user stack. Every thread of the app makes its syscalls
// from a parked sp with a poisoned band of its own stack below it, and reads the band back: the
// worker raises its own claimed line from inside dispatch and makes the deepest syscall, main
// and the ticker sleep, so timer ticks and switches land in their dispatches.

#include <kickos/arch/rv_trap_stack.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/abi.h>

#if !defined(__riscv)
#error "trapnest moves sp with RISC-V asm and reads rv32imac's own figures; not for this ISA"
#endif

namespace
{
    // No hardware source, so unmasking it cannot deliver a real device interrupt here.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int TN_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 1;
#else
    constexpr int TN_LINE = 7;
#endif

    // Each call is one nested trap, so this is a count of witnesses and not a retry budget.
    constexpr uint32_t TN_RAISES = 64;

    // Enough real switches for the timer path, short enough that the whole arm stays well inside
    // the runner's timeout.
    constexpr uint32_t TN_SLEEPS = 32;
    constexpr uint64_t TN_SLEEP_NS = 200u * 1000u;

    // Each attempt is a spawn REFUSED after the chain has descended, so no thread slot is
    // consumed and the loop can repeat. A tick has to land in the window, so this is a budget
    // and not a count.
    constexpr uint32_t TN_SPAWNS = 8192;

    // The clock is TICKLESS, so with nothing sleeping no timer is armed and the deep loop below
    // would run to completion without one interrupt landing in it. The ticker must outrank the
    // worker, so its wake preempts and it gets to re-arm; a lower-priority one fires once,
    // becomes ready, and never sleeps again.
    constexpr uint64_t TN_TICK_NS = 100u * 1000u;

    // Exactly what a trap entry that adopted the interrupted sp would spend on this thread's own
    // stack for a deep syscall: the ecall frame, the dispatch, and the msip frame the switcher
    // takes at that depth. Frames deeper than the band go unseen here.
    constexpr uintptr_t TN_PARK_ROOM =
        KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYS;
    constexpr uint32_t TN_BAND_WORDS = TN_PARK_ROOM / 4u;
    constexpr uint32_t TN_BAND_POISON = 0x5AFEBA5Eu;
    static_assert(TN_PARK_ROOM % 16u == 0u, "a parked sp must keep the ABI's 16-byte alignment");

    // Power of two and clear of the floor: under enforcement the worker's stack is one PMP
    // region, and PMP NAPOT wants a naturally aligned power of two.
    constexpr uint32_t TN_STACK_SIZE = 2048;
    static_assert(TN_STACK_SIZE > TN_PARK_ROOM,
                  "the worker cannot park that low and still have a stack above it, so it "
                  "would fault before reaching the arm");

    enum TnThread
    {
        TN_MAIN = 0,
        TN_TICKER = 1,
        TN_WORKER = 2,
        TN_THREADS = 3
    };
    char const* const TN_NAMES[TN_THREADS] = {"main", "ticker", "worker"};

    struct BandResult
    {
        uint32_t hits;
        uintptr_t lowest;
        bool read;
    };
    BandResult g_tn_band[TN_THREADS] = {};

    uintptr_t g_tn_stack_lo = 0;
    volatile uint32_t g_tn_stop = 0;

    constexpr kos_cap_t TN_CAP = KOS_SPAWN_DELEGATED_CAP0;      // the line: raise and ack
    constexpr kos_cap_t TN_NOTE = KOS_SPAWN_DELEGATED_CAP0 + 1; // the object it raises, bit 0

    void band_poison(volatile uint32_t* band)
    {
        for (uint32_t i = 0; i < TN_BAND_WORDS; i++)
        {
            band[i] = TN_BAND_POISON;
        }
    }

    void band_read(volatile uint32_t const* band, TnThread who)
    {
        uint32_t hits = 0;
        uintptr_t lowest = 0;
        for (uint32_t i = TN_BAND_WORDS; i > 0; i--)
        {
            if (band[i - 1u] != TN_BAND_POISON)
            {
                hits++;
                lowest = (i - 1u) * 4u;
            }
        }
        g_tn_band[who].hits = hits;
        g_tn_band[who].lowest = lowest;
        g_tn_band[who].read = true;
    }

    // Nothing may touch memory between the sp move and the trap. a0 is the syscall number and
    // a1..a4 the arguments (sys/abi.h); the unused ones are ZEROED, as the arch_syscall stub
    // does, in case a dispatch arm ever reads an argument it does not read today.
    uint32_t call_from(uintptr_t sp_target, uint32_t nr, uint32_t x1, uint32_t x2)
    {
        uint32_t saved = 0;
        uint32_t rc = 0;
        __asm volatile("mv   %[sv], sp   \n\t"
                       "mv   sp, %[low]  \n\t"
                       "mv   a0, %[nr]   \n\t"
                       "mv   a1, %[x1]   \n\t"
                       "mv   a2, %[x2]   \n\t"
                       "mv   a3, zero    \n\t"
                       "mv   a4, zero    \n\t"
                       "ecall            \n\t"
                       "mv   sp, %[sv]   \n\t"
                       "mv   %[rc], a0   \n\t"
                       : [sv] "=&r"(saved), [rc] "=r"(rc)
                       : [low] "r"(sp_target), [nr] "r"(nr), [x1] "r"(x1), [x2] "r"(x2)
                       : "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
                         "t0", "t1", "t2", "t3", "t4", "t5", "t6", "memory");
        return rc;
    }

    uint32_t sleep_from(uintptr_t sp_target, uint64_t ns)
    {
        return call_from(sp_target, KOS_SYS_SLEEP_NS, static_cast<uint32_t>(ns),
                         static_cast<uint32_t>(ns >> 32));
    }

    // File-scope and not a local: the kernel reads this struct out of the caller's memory, and
    // the worker's sp is parked far from its own frames when the trap is taken.
    kos_thread_params g_tn_params = {};
    kos_thread_t g_tn_out = 0;

    void ticker(void*)
    {
        alignas(16) volatile uint32_t band[TN_BAND_WORDS];
        band_poison(band);
        uintptr_t const low = reinterpret_cast<uintptr_t>(&band[TN_BAND_WORDS]);
        while (g_tn_stop == 0u)
        {
            (void)sleep_from(low, TN_TICK_NS);
        }
        band_read(band, TN_TICKER);
    }

    void worker(void*)
    {
        uintptr_t const low = g_tn_stack_lo + TN_PARK_ROOM;
        volatile uint32_t* const band = reinterpret_cast<volatile uint32_t*>(g_tn_stack_lo);
        band_poison(band);
        char msg[128];
        ksnprintf(msg, sizeof(msg), "[trapnest] worker parks sp at 0x%x (stack_lo 0x%x + %u)\n",
                  static_cast<unsigned>(low), static_cast<unsigned>(g_tn_stack_lo),
                  static_cast<unsigned>(TN_PARK_ROOM));
        kos::print(msg);
        // Bind delivery to this worker before waiting.
        if (kos_notify_bind(TN_NOTE) != 0)
        {
            kos::print("[trapnest] ERROR: notify_bind refused the delegated object\n");
            return;
        }
        for (uint32_t i = 0; i < TN_RAISES; i++)
        {
            if (call_from(low, KOS_SYS_IRQ_RAISE, static_cast<uint32_t>(TN_CAP), 0) != 0u)
            {
                kos::print("[trapnest] ERROR: the raise was refused\n");
                return;
            }
            // Consume and rearm after each raise; the ISR leaves the line masked.
            (void)kos_notify_wait(TN_NOTE, 1u, KOS_TIMEOUT_NONE, nullptr);
            kos_irq_ack(TN_CAP);
        }
        // The DEEPEST syscall an unprivileged thread can reach, and the chain rv_trap_stack.h
        // measures the syscall requirement against. Depth decides how far under the frame the
        // nested trap lands; the shallower calls above cannot get it low enough.
        for (uint32_t i = 0; i < TN_SPAWNS; i++)
        {
            (void)call_from(low, KOS_SYS_THREAD_CREATE,
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_tn_params)),
                            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_tn_out)));
        }
        band_read(band, TN_WORKER);
        kos::print("[trapnest] worker done\n");
    }
}

int main(int, char**)
{
    alignas(16) volatile uint32_t band[TN_BAND_WORDS];
    band_poison(band);

    // The worker's stack is the upper half of one block, so stack_lo is a power-of-two boundary
    // the PMP can name: kos_ram_alloc rounds to a describable region size and aligns to it.
    void* const raw = kos_ram_alloc(2u * TN_STACK_SIZE);
    if (raw == nullptr)
    {
        kos::print("[trapnest] ERROR: the arena cannot spare a caller-owned stack\n");
        return 1;
    }
    uintptr_t const lo = reinterpret_cast<uintptr_t>(raw) + TN_STACK_SIZE;
    if ((lo & (TN_STACK_SIZE - 1u)) != 0)
    {
        kos::print("[trapnest] ERROR: the arena block is not aligned to the stack size\n");
        return 1;
    }
    g_tn_stack_lo = lo;

    // WHERE the refusal happens decides the depth reached. A stack under the floor is refused
    // near the TOP of thread_create_call; an INADMISSIBLE memory region is refused in
    // spawn_grant_admit -> ram_region_admit -> grant_region_admissible, the deep end of the
    // chain rv_trap_stack.h measures the syscall red zone against. So: a kernel-default stack,
    // and a region outside the arena.
    g_tn_params.entry = worker;
    g_tn_params.name = "tndeep";
    g_tn_params.prio = 10;
    g_tn_params.mem_base = reinterpret_cast<void*>(0x1000u);
    g_tn_params.mem_size = 64;

    // Stop on claim failure or the raises will not exercise the trap path.
    kos_cap_t line = KOS_CAP_NONE;
    if (kos_irq_claim(TN_LINE, KOS_IRQ_EDGE, &line) != 0)
    {
        kos::print("[trapnest] ERROR: irq_claim refused, so the line stays masked\n");
        return 1;
    }
    kos_cap_t note = KOS_CAP_NONE;
    if (kos_notify_create(&note) != 0 or kos_irq_bind_notify(line, note) != 0)
    {
        kos::print("[trapnest] ERROR: the line could not be attached to a notification\n");
        return 1;
    }
    kos_irq_ack(line); // a claim leaves the line masked, and the worker raises it
    kos_cap_grant const wcaps[] = {{line, KOS_CAP_WAIT | KOS_CAP_SIGNAL}, {note, KOS_CAP_WAIT}};
    uint8_t const nwcaps = static_cast<uint8_t>(sizeof(wcaps) / sizeof(wcaps[0]));

    kos::thread::Handle const tk = kos::thread::create(ticker, nullptr, "tntick", 20);
    if (not tk.valid())
    {
        kos::print("[trapnest] ERROR: ticker spawn refused\n");
        return 1;
    }
    kos::thread::Handle const w = kos::thread::create_caps(
        worker, nullptr, "tnwork", 10, wcaps, nwcaps, KOS_POLICY_FIFO, 0,
        /*privileged=*/false, nullptr, 0, 0, nullptr, KOS_TASK_NONE,
        reinterpret_cast<void*>(lo), TN_STACK_SIZE);
    if (not w.valid())
    {
        kos::print("[trapnest] ERROR: worker spawn refused\n");
        return 1;
    }
    if (w.join(KOS_TIMEOUT_NONE) != 0)
    {
        kos::print("[trapnest] ERROR: join did not report the worker gone\n");
        return 1;
    }

    uintptr_t const low = reinterpret_cast<uintptr_t>(&band[TN_BAND_WORDS]);
    for (uint32_t i = 0; i < TN_SLEEPS; i++)
    {
        (void)sleep_from(low, TN_SLEEP_NS);
    }
    band_read(band, TN_MAIN);
    g_tn_stop = 1u;
    if (tk.join(KOS_TIMEOUT_NONE) != 0)
    {
        kos::print("[trapnest] ERROR: join did not report the ticker gone\n");
        return 1;
    }

    // Main prints, the threads only count: their prints would deepen the stacks they measure.
    char msg[160];
    for (uint32_t t = 0; t < TN_THREADS; t++)
    {
        BandResult const& r = g_tn_band[t];
        if (not r.read)
        {
            ksnprintf(msg, sizeof(msg), "[trapnest] ERROR: the %s never read its band\n",
                      TN_NAMES[t]);
        }
        else if (r.hits == 0u)
        {
            ksnprintf(msg, sizeof(msg),
                      "[trapnest] %s band intact: 0 of %u words written below the parked sp\n",
                      TN_NAMES[t], static_cast<unsigned>(TN_BAND_WORDS));
        }
        else
        {
            ksnprintf(msg, sizeof(msg),
                      "[trapnest] %s band CORRUPTED: %u of %u words written below the parked "
                      "sp, the lowest %u bytes above the band's base\n",
                      TN_NAMES[t], static_cast<unsigned>(r.hits),
                      static_cast<unsigned>(TN_BAND_WORDS), static_cast<unsigned>(r.lowest));
        }
        kos::print(msg);
    }
    kos::print("[trapnest] main ran after the worker\n");
    return 0;
}
