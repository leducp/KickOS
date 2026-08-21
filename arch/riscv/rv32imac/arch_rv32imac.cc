// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC arch backend: the ISA-generic half of the arch.h seam. The
// context switch + trap entry + syscall trampoline + first-thread entry assembly
// lives in switch.S; the chip layer (arch/riscv/chip/{virt,esp32c6}) supplies the
// hardware edges: arch_init, arch_console_write, arch_shutdown, the clock/timer
// (arch_clock_now / arch_timer_arm / arch_timer_disarm), the CLINT base
// (g_clint_msip, for the deferred-switch software interrupt), and the linker
// script + startup vectors.

#include <kickos/arch/arch.h>
#include <kickos/arch/rv_trap_stack.h> // the trap guard's derived figures + ctx offsets
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend

#include <bit>

#include <stddef.h>
#include <stdint.h>

// The trace-arch id (CMake ladder / this chip's caps.cmake) must equal the ArchId for the
// arch this backend implements, or a SESSION record mislabels the trace.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_RISCV,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_RISCV for rv32imac");

// Fault reporting (see the .Lfault shim in switch.S): the reporter calls kpanic_enter
// first, which masks IRQs, forces the synchronous polled writer and flushes the ring, so
// the dump is safe from the fault path whether or not the chip armed a buffered console.
// kfault_terminate is the shared panic/fault dead-end (kernel.h).
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));
// Kernel MPU-violation reporter (kernel/init/console.cc): names the offending thread and
// shuts down cleanly (the reported-fault path). A U-mode load/store access fault is a PMP
// domain violation, so it routes here and emits the shared "MPU FAULT: thread '<name>'"
// marker.
extern "C" void kickos_isr_fault(uintptr_t addr, int is_write);

// 0 keeps only the one-line fault marker; set it in the board defconfig or with
// cmake -DKICKOS_PANIC_DUMP=0.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// switch.S hard-codes the save-frame layout AND ctx.sp @0 / trace_tid @4. The frame
// (on the thread's own stack, ctx.sp = its base, low->high) is 32 words / 128 bytes:
//   [0 mepc][1 mstatus][2 ra][3 t0][4 t1][5 t2][6 s0][7 s1][8 a0]..[15 a7]
//   [16 s2]..[25 s11][26 t3][27 t4][28 t5][29 t6][30 sp][31 pad]
// gp and tp are out of the frame. tp is set once in _start and no path touches it; gp is
// re-anchored by .Lrestore on every switch (switch.S carries why).
namespace
{
    enum : uint32_t
    {
        F_MEPC = 0, F_MSTATUS = 1, F_RA = 2, F_A0 = 8, // word indices
        F_SP = KICKOS_RV_TRAP_F_SP / 4,
        FRAME_WORDS = 32
    };
    static_assert(KICKOS_RV_TRAP_F_SP % 4 == 0, "F_SP is not a word offset");
    static_assert(F_SP < FRAME_WORDS, "F_SP lies outside the frame");

    // mstatus bits (RISC-V Privileged ISA v1.10).
    constexpr uint32_t MSTATUS_MIE  = 1u << 3;
    constexpr uint32_t MSTATUS_MPIE = 1u << 7;
    constexpr uint32_t MSTATUS_MPP_M = 3u << 11; // MPP = machine (U = 0)

}

// switch.S hard-codes each offset below as a literal displacement, and nothing in the
// language ties them to the struct: a field inserted ahead of the bounds leaves the trap
// guard comparing sp against trace_tid. rv_trap_stack.h holds the single definition; these
// assert the struct agrees with it.
static_assert(offsetof(struct arch_context, sp) == KICKOS_RV_CTX_OFF_SP,
              "switch.S expects ctx.sp @0");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_RV_CTX_OFF_TRACE_TID,
              "switch.S reads trace_tid at 4(ctx)");
#endif
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_RV_CTX_OFF_STACK_LO,
              "switch.S reads stack_lo at F_CTX_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_RV_CTX_OFF_STACK_HI,
              "switch.S reads stack_hi at F_CTX_STACK_HI");
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_RV_CTX_OFF_KERNEL_SP,
              "a trusted entry loads ctx.kernel_sp at F_CTX_KERNEL_SP");
static_assert(sizeof(struct arch_context) >= KICKOS_RV_CTX_OFF_KERNEL_SP + sizeof(uint32_t),
              "the guard reads a word past the end of struct arch_context");

// The frame the prologue builds and the frame arch_context_init fabricates are one object,
// and the guard prices the prologue's own descent at KICKOS_RV_TRAP_FRAME.
static_assert(FRAME_WORDS * 4 == KICKOS_RV_TRAP_FRAME,
              "the fabricated frame and the trap guard disagree on the frame size");
// The syscall zone's structural half is the ecall frame plus the msip frame the deferred
// switcher builds at whatever depth the dispatch reached.
static_assert(KICKOS_RV_TRAP_FRAME_SYS == 2 * KICKOS_RV_TRAP_FRAME,
              "the syscall zone must hold exactly two frames");
// The floor must DOMINATE the worst-case red zone, or a thread spawned at the floor passes
// the spawn check and is then refused by the guard on every syscall it makes. This assert
// covers every rv32imac board.
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RV_TRAP_REDZONE_SYS,
              "KICKOS_MIN_STACK_SIZE is below the rv32imac syscall red zone: raise the "
              "per-arch default in Kconfig, never the red zone, which is a measurement");

extern "C"
{
    // Shared with switch.S: written by C and by asm. No leading underscore (RISC-V ELF
    // symbol convention).
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_current = nullptr;
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_next = nullptr;

    // switch.S loads each as a plain word at offset 0. Nothing else enforces the layout.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // In-ISR depth: bumped by the timer/soft/external trap paths alone (switch.S), so
    // arch_in_isr() reads false throughout syscall_dispatch and the msip switch.
    uint32_t g_isr_depth = 0;

    // switch.S bumps it with lw/sw at offset 0. Nothing else enforces the width.
    static_assert(sizeof(g_isr_depth) == 4, "asm reads one word");

    // Trusted per-hart trap stack. mscratch holds its top while a thread runs, so
    // trap_entry (switch.S) swaps onto it before it touches the interrupted sp, and a
    // U-mode thread's sp never selects where the prologue's own scratch lands. It also
    // CARRIES the frame of every M-mode trap whose frame is not a thread's saved context,
    // and the kernel C that runs below it, so a tick taken mid-dispatch stays off the
    // calling thread's stack. rv_trap_stack.h derives the size and check_trap_redzone.sh
    // re-measures the depth half of it.
    alignas(16) uint8_t g_rv_trap_stack[KICKOS_RV_TRAP_STACK_SIZE];
    static_assert(KICKOS_RV_TRAP_STACK_SIZE % KICKOS_RV_TRAP_SP_ALIGN == 0,
                  "the trap-stack top must land on the alignment the prologue requires");
    static_assert(KICKOS_RV_TRAP_STACK_SIZE > KICKOS_RV_TRAP_FRAME,
                  "a nested frame would fill the whole trap stack, leaving nowhere for the "
                  "kernel C below it; how much is enough is what the gate measures");

    // CLINT machine-software-interrupt-pending register for this hart, set by the
    // chip's arch_init. arch_switch writes 1 to pend the deferred switch; the msip
    // switcher (switch.S) writes 0 to clear it. Chip-provided because the CLINT base
    // differs per chip (qemu-virt 0x0200_0000; the C6 exposes its own).
    volatile uint32_t* g_clint_msip = nullptr;

#if KICKOS_BENCH
    // Bench cycle source. Default null -> switch.S/bench use `rdcycle`. A core whose
    // `rdcycle` traps (the ESP32-C6 HP core has no Zicntr counters) points this at a
    // free-running MMIO counter instead (its core-clocked CLINT MTIME low word), so
    // the switch bracket reads that. Set by the chip before the first switch.
    volatile uint32_t* g_bench_cycle_src = nullptr;
#endif

    // switch.S entry points + the kernel/user thread-return trampolines.
    void trap_entry(void);
    void kickos_rv_mtvec(void); // the vectored mtvec table (switch.S)
    void kickos_thread_return(void);
    void kickos_user_thread_return(void);
}

// ===========================================================================
extern "C"
{

// Anything file-local below is `static`, never an anonymous namespace: C language
// linkage overrides the namespace, so an anonymous namespace nested in this block
// emits an unmangled GLOBAL symbol.

// CMSIS core clock, defined + maintained by the chip at PLL bring-up.
extern uint32_t SystemCoreClock;
uint32_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}

// --- Context init: fabricate a first-resume frame (see the layout above) -----
// The frame is identical to what the msip switcher saves, so the first switch-in
// (arch_start) restores it and mret's into entry(arg): mepc=entry, a0=arg, ra=the
// thread-return trampoline (kernel or user), mstatus=MPIE|MPP so mret runs the
// thread at the right privilege with interrupts enabled.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(15); // 16-byte stack alignment (RISC-V psABI)
    uint32_t* f = reinterpret_cast<uint32_t*>(top - FRAME_WORDS * 4);
    for (uint32_t i = 0; i < FRAME_WORDS; i++)
    {
        f[i] = 0;
    }

    uint32_t mstatus = MSTATUS_MPIE; // MIE=0 now; mret sets MIE<-MPIE (=1)
    uint32_t ret = reinterpret_cast<uint32_t>(kickos_thread_return);
    if (privileged)
    {
        mstatus |= MSTATUS_MPP_M; // return to M-mode (privileged thread)
    }
    else
    {
        ret = reinterpret_cast<uint32_t>(kickos_user_thread_return); // MPP=U (0)
    }

    f[F_MEPC] = reinterpret_cast<uint32_t>(entry);
    f[F_MSTATUS] = mstatus;
    f[F_RA] = ret;                              // entry() returns here
    f[F_A0] = reinterpret_cast<uint32_t>(arg);  // first C argument
    // The sp .Lrestore leaves on. Nothing else seats it in a fabricated frame.
    f[F_SP] = static_cast<uint32_t>(top);
    ctx->sp = reinterpret_cast<uint32_t>(f);

    // trap_entry validates the interrupted U-mode sp against these before it stores a
    // frame through it. `top` is the 16-byte-aligned high edge the first frame base
    // sits below, so a running thread's sp stays in [stack_lo, stack_hi].
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);

    // No kernel stack is allocated yet, so it is seated at 0 rather than left as whatever
    // the TCB slab last held. A trusted entry that finds 0 here has nothing to transfer to,
    // which is the state every thread is in until the allocator lands.
    ctx->kernel_sp = 0;
}

// The fastpath parks a caller on its own syscall frame with no kernel continuation, so
// the result has to be seated where .Lrestore reloads a0 from. ctx->sp is the frame base
// and the thread is not running, so this is a plain store to memory nothing else holds.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp)[F_A0] = result;
}

// The whole seam on this backend: the fabricated frame's F_MSTATUS carries MPP=M, so the
// mret that resumes it lands in M-mode. This works entirely through the saved frame, which
// is why it applies to a context that is not running; arch_fault_redirect_to_exit is the
// live-CSR half.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
}

// --- Switch: record the target + pend the msip switcher ---------------------
// Always deferred, in ISR and thread context alike: the physical swap happens in the msip
// trap. Called under the kernel IrqLock (mstatus.MIE=0), so the pended msip fires once the
// lock releases (thread context) or the current trap returns (ISR context).
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // the switcher saves g_arch_current
    g_arch_next = to;
    *g_clint_msip = 1; // pend machine software interrupt
}

// --- Critical section: clear/restore mstatus.MIE ----------------------------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t old;
    // Atomically clear MIE and return the prior mstatus; keep only the MIE bit.
    __asm volatile("csrrci %0, mstatus, 0x8" : "=r"(old)::"memory");
    return old & MSTATUS_MIE;
}

void arch_irq_restore(arch_irq_state_t state)
{
    // csrs only SETS bits: state is 0 or MSTATUS_MIE, so this re-enables MIE iff it was
    // enabled at the paired save and is a no-op otherwise, which keeps it nesting-safe.
    __asm volatile("csrs mstatus, %0" ::"r"(state) : "memory");
}

int arch_in_isr(void)
{
    return g_isr_depth != 0;
}

// --- Trace clock: the cycle CSR (rdcycle), 32-bit raw ------------------------
// Reads its own low 32 bits (wraps; the host reconstructs absolute time from the SESSION
// clock_hz anchors). Raw and lock-free, so it is safe on the switch path. kickos_rv32_init
// enables mcounteren.CY where the core has the CSR (arch_rv_has_mcounteren), so a U-mode
// thread can read it there.
//
// THE COUNTER IS PER-CORE, NOT PER-ARCH. Without Zicntr this instruction is illegal in
// M-mode too, so the trap lands in the kernel: the ESP32-C6 HP core is one such part, and
// its caps.cmake declares KICKOS_HAVE_TRACE_CLOCK 0, which refuses telemetry at configure.
// A chip that has a counter states the capability in its own caps.cmake and, if the counter
// is not rdcycle, overrides this function.
uint32_t arch_trace_now(void)
{
    uint32_t v;
    __asm volatile("rdcycle %0" : "=r"(v));
    return v;
}


// --- MPU: RISC-V PMP backend (NAPOT per region) ------------------------------
#if KICKOS_HAVE_MPU
// NAPOT encoding: for a region of size 2^k (k>=3) aligned to its size,
// pmpaddr = (base>>2) | ((size>>3)-1); the trailing 1s encode the size.
static uint32_t pmp_napot_addr(uintptr_t base, size_t size)
{
    return (static_cast<uint32_t>(base) >> 2)
         | ((static_cast<uint32_t>(size) >> 3) - 1u);
}

// cfg byte: A=NAPOT (0b11<<3) | R | W? | X?  (attr = the U-mode rights; M-mode bypasses
// these unlocked entries).
static uint8_t pmp_cfg(uint32_t attr)
{
    uint32_t c = 0x18u | 0x1u; // NAPOT | R
    if (attr & ARCH_MPU_W)
    {
        c |= 0x2u;
    }
    if (attr & ARCH_MPU_X)
    {
        c |= 0x4u;
    }
    return static_cast<uint8_t>(c);
}

// Pack the region set into the ten CSR words a commit writes. The cfg BYTES ride four to a
// pmpcfg word, so the packing is a property of the whole set. A region PMP cannot name
// (arch_mpu_region_encodable: a power-of-two size >= 8, naturally aligned) is left cfg 0,
// which grants no access at all: the encoding fails closed.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    uint8_t cfg[ARCH_MPU_ENCODED_SLOTS];
    size_t i = 0;
    for (; i < n; i++)
    {
        out->addr[i] = 0;
        cfg[i] = 0;
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            out->addr[i] = pmp_napot_addr(regions[i].base, regions[i].size);
            cfg[i] = pmp_cfg(regions[i].attr);
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->addr[i] = 0;
        cfg[i] = 0;
    }
    out->cfg[0] = static_cast<uint32_t>(cfg[0]) | (static_cast<uint32_t>(cfg[1]) << 8)
                | (static_cast<uint32_t>(cfg[2]) << 16) | (static_cast<uint32_t>(cfg[3]) << 24);
    out->cfg[1] = static_cast<uint32_t>(cfg[4]) | (static_cast<uint32_t>(cfg[5]) << 8)
                | (static_cast<uint32_t>(cfg[6]) << 16) | (static_cast<uint32_t>(cfg[7]) << 24);
    return seated;
}

// The image the next commit programs. A POINTER into the caller's TCB, not a copy: every
// commit on this arch is preceded by an apply inside the SAME MIE=0 window (the msip
// trap, the .Lecall fastpath tail, arch_start, and the self-grant syscall, which commits
// before it returns), so nothing can rewrite the image in between. Thread slots come from
// a static pool and are never returned to an allocator, so a pointer left over from an
// earlier switch still addresses valid storage.
static struct arch_mpu_encoded const* g_pend_image = nullptr;

#if KICKOS_BENCH
// The kernel's phase accumulator, reached the way switch.S reaches
// kickos_bench_switch_done: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_mpu_commit(uint32_t delta);

static uint32_t mpu_bench_cyc(void)
{
    if (::g_bench_cycle_src != nullptr)
    {
        return *::g_bench_cycle_src;
    }
    uint32_t v;
    __asm volatile("rdcycle %0" : "=r"(v));
    return v;
}
#endif

// STASH-ONLY apply (deferred-commit seam, docs/design-mpu-commit-deferred.md): record
// the incoming set; kickos_arch_mpu_commit writes the PMP CSRs from the .Lswitch switch
// epilogue (switch.S) AFTER the physical msip-driven swap. Eager apply on the deferred
// switch would run the OUTGOING user thread under the incoming PMP set until msip fires,
// so it would fault on its own stack.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}

// Program the 8 PMP entries from the stash. Called from .Lswitch / arch_start after the
// physical swap; that path runs in the M-mode trap with MIE=0, so the CSR writes are
// already atomic vs interrupts and must NOT toggle MIE here.
void kickos_arch_mpu_commit(void)
{
#if KICKOS_BENCH
    uint32_t const bench_start = mpu_bench_cyc();
#endif
    struct arch_mpu_encoded const* const img = g_pend_image;
    if (img == nullptr)
    {
        return;
    }
    uint32_t const* const addr = img->addr;
    // Write the addresses, then the two cfg words, which activate the entries. This
    // overwrites the permissive bootstrap TOR entry (kickos_rv32_init); the kernel is in
    // M-mode here and bypasses PMP, so the transient is safe. csrw takes an IMMEDIATE CSR
    // number, so the entries are spelled out rather than indexed.
    __asm volatile("csrw pmpaddr0, %0" ::"r"(addr[0]) : "memory");
    __asm volatile("csrw pmpaddr1, %0" ::"r"(addr[1]) : "memory");
    __asm volatile("csrw pmpaddr2, %0" ::"r"(addr[2]) : "memory");
    __asm volatile("csrw pmpaddr3, %0" ::"r"(addr[3]) : "memory");
    __asm volatile("csrw pmpaddr4, %0" ::"r"(addr[4]) : "memory");
    __asm volatile("csrw pmpaddr5, %0" ::"r"(addr[5]) : "memory");
    __asm volatile("csrw pmpaddr6, %0" ::"r"(addr[6]) : "memory");
    __asm volatile("csrw pmpaddr7, %0" ::"r"(addr[7]) : "memory");
    __asm volatile("csrw pmpcfg0, %0" ::"r"(img->cfg[0]) : "memory");
    __asm volatile("csrw pmpcfg1, %0" ::"r"(img->cfg[1]) : "memory");
    // Order the PMP update before the mret (arch_switch) that drops to U-mode, so the
    // incoming thread's fetches/loads see the new entries. The priv spec says the writing
    // hart sees PMP changes on its next access; the fence is the conservative guarantee
    // across the M->U transition.
    __asm volatile("fence" ::: "memory");
#if KICKOS_BENCH
    kickos_bench_mpu_commit(mpu_bench_cyc() - bench_start);
#endif
}
#else
// KICKOS_HAVE_MPU=0: isolation is privilege + syscall only, and the permissive bootstrap
// PMP stays in place for the life of the image.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}
// .Lswitch and arch_start call this unconditionally.
void kickos_arch_mpu_commit(void) {}
#endif

size_t arch_mpu_min_region(void)
{
    return 8u; // RISC-V PMP NAPOT minimum region size
}

// PMP NAPOT needs a power-of-two size >= 8 with the base naturally aligned to it
// (the encoding folds the size into the trailing address bits).
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 8u or (size & (size - 1)) != 0)
    {
        return false;
    }
    return (base & (size - 1)) == 0;
}

int arch_mpu_region_pow2(void)
{
    return 1;
}

// The RISC-V parts in tree reach the arena uncached, and a PMP entry carries permissions
// with no memory type, so the arena is already in the state a nocache grant asks for.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

// Rule 7 (arch.h): 0 for RISC-V, which has no bit-band alias, so the grant path skips the
// alias-image test.
int arch_bitband_present(void)
{
    return 0;
}


// --- Interrupt controller (software-injected test scaffolding) ---------------
// arch_irq_inject fakes a device firing (test/bench scaffolding, arch.h). It masks with a
// software bitmask (a raise on a masked line latches one-deep, redelivered at unmask),
// records the logical line in g_inject_line, then hands the actual raise to a
// chip-overridable delivery hook (arch_rv_inject_deliver). ONE physical doorbell carries
// every logical line and g_inject_line tells the trap which line it was, so
// arch_irq_mask/unmask stay pure-software and decoupled from the physical interrupt.
//
// virt default: the SUPERVISOR SOFTWARE interrupt (mip.SSIP, a software-writable bit,
// mcause=1) as a private channel. SSIP needs S-mode, present on the QEMU virt CPU.
//
// ESP32-C6 override (chip_esp32c6.cc): the C6 HP core is M/U-only and has no SSIP, so its
// override raises a real machine interrupt via the interrupt matrix + INTPRI local
// controller, from a FROM_CPU source routed to a dedicated CPU interrupt ID. That ID
// vectors here as mcause=<ID> (the C6 reports mcause = interrupt ID, not the standard
// mcause=11), demuxed to .Lext in switch.S -> kickos_rv_ext_dispatch below.
static constexpr uint32_t MIP_SSIP = 1u << 1;

// bit set = line masked. All lines start MASKED at reset (the arch.h reset contract); a
// driver unmasks its line (arch_irq_unmask, or irq_claim) before use.
static uint32_t g_irq_masked = 0xFFFFFFFFu;
// the pending software-injected line
static kickos::Atomic<int, kickos::Order::RELAXED> g_inject_line = -1;

// bit set = a raise landed on this software line while masked (latched one-
// deep, coalesced). Redelivered through the doorbell at unmask. Scoped to the
// software-inject lines; a real PLIC line holds its own pending in hardware.
static uint32_t g_irq_pending = 0;

// The rv32imac chip hooks; every fallback body lives in its own TU (<symbol>_default.cc).
// arch_rv_hw_{un,}mask reach a REAL controller line (interrupt matrix + PLIC) from INSIDE
// the arch critical section, and arch_rv_ext_eoi de-asserts a level source at the head of
// the external-doorbell trap.
void arch_rv_inject_deliver(int line);
void arch_rv_hw_unmask(int line);
void arch_rv_hw_mask(int line);
void arch_rv_ext_eoi(void);
int arch_rv_has_mcounteren(void);

void arch_irq_mask(int line)
{
    if (line < 0 or line >= 32)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked |= (1u << line);
    // Reach the controller to mask a REAL line inside the critical section (mstatus.MIE=0).
    // Injected lines are carried by the software bitmask above.
    arch_rv_hw_mask(line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= 32)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked &= ~(1u << line);
    // Chip HW routing runs INSIDE the critical section (mstatus.MIE=0) so an INTMTX/PLIC
    // reconfigure can't glitch in the controller's transient state (C6 TRM section 1.6.3.2:
    // configure with MIE cleared + a FENCE).
    arch_rv_hw_unmask(line);
    // Latch-and-coalesce: a raise taken on this software line while it was masked
    // redelivers now through the doorbell. The raise sets mip.SSIP with MIE=0, so it fires
    // at arch_irq_restore, on the normal ISR path rather than as a direct post.
    if ((g_irq_pending & (1u << line)) != 0)
    {
        g_irq_pending &= ~(1u << line);
        g_inject_line = line;
        arch_rv_inject_deliver(line);
    }
    arch_irq_restore(s);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= 32)
    {
        return;
    }
    // Software-inject lines: drop the latched raise. A real PLIC line holds its pending in
    // hardware.
    arch_irq_state_t s = arch_irq_save();
    g_irq_pending &= ~(1u << line);
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= 32)
    {
        return;
    }
    // Bracketed like arch_irq_mask/unmask: an ISR reaching those read-modify-writes the
    // same words.
    arch_irq_state_t s = arch_irq_save();
    if ((g_irq_masked & (1u << irq)) != 0)
    {
        // Latch-and-coalesce: a masked line latches the raise one-deep and redelivers it
        // through the doorbell at unmask.
        g_irq_pending |= (1u << irq);
    }
    else
    {
        g_inject_line = irq; // set BEFORE the raise, so the trap sees it
        arch_rv_inject_deliver(irq);
    }
    arch_irq_restore(s);
}

// SSIP dispatch (switch.S .Lssoft, virt), ISR context. Clear the software interrupt,
// then run the injected line's first-level ISR (kickos_isr_irq masks the line and wakes
// its driver, kernel/irq/irq.cc); the driver re-unmasks via irq_ack.
void kickos_rv_dispatch_soft(void)
{
    __asm volatile("csrc mip, %0" ::"r"(MIP_SSIP) : "memory");
    int line = g_inject_line;
    g_inject_line = -1;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// External-doorbell dispatch (switch.S .Lext), ISR context. Reached on a chip whose
// arch_rv_inject_deliver raises a real machine external interrupt (the C6). EOI the chip's
// controller source first, so a level source cannot re-fire, then run the injected line's
// ISR.
void kickos_rv_ext_dispatch(void)
{
    arch_rv_ext_eoi();
    int line = g_inject_line;
    g_inject_line = -1;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// The call site is inside .Lintr, which lies within the window
// kernel/bench/bench.cc's injected-IRQ arm measures, so the witness is out of a bench build.
#if defined(KICKOS_ENABLE_SELFTEST) && !KICKOS_BENCH
// Nested-trap witness (arch.h), called from switch.S's .Lintr demux once msip is out and
// the whole frame is saved at `frame`. An interrupt taken with mstatus.MPP=M interrupted
// the kernel, and if what it interrupted was a syscall dispatch then the sp it found is
// the CALLING THREAD'S, at whatever depth the dispatch had reached, with no bound applied
// to it. mstatus comes out of the frame rather than the live CSR because .Lrestore is what
// will consume it.
void kickos_rv_nested_witness(void* frame)
{
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    if ((f[F_MSTATUS] & MSTATUS_MPP_M) == 0)
    {
        return; // interrupted U-mode: the bounds-checked thread stack is where it belongs
    }
    struct arch_context* const c = g_arch_current;
    uintptr_t lo = 0;
    uintptr_t hi = 0;
    if (c != nullptr)
    {
        lo = c->stack_lo;
        hi = c->stack_hi;
    }
    kickos_nestwitness_note(reinterpret_cast<uintptr_t>(frame), lo, hi);
}
#endif

// --- Fault isolation ----------------------------------------------------------
// mstatus.MPP is the privilege BEFORE the trap and lives in a CSR, so it is valid
// whatever the stack did; nothing between the vector and here writes it. NOT the
// thread's identity: .Lecall runs syscall dispatch in M-mode on the thread's own
// stack, so a fault there is a kernel bug and MPP says so.
//
// The frame is the one trap_entry pushed at sp. The prologue is software and runs M-mode,
// which bypasses the unlocked PMP entries, so an overflowed thread's frame is written
// SUCCESSFULLY below its own stack and the stack-bounds test is what catches it.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t mstatus;
    __asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    if ((mstatus & MSTATUS_MPP_M) != 0)
    {
        return false;
    }
    return kickos_fault_frame_trusted(frame, FRAME_WORDS * 4);
}

// Mirrors .Lecall: point mepc at the stub and set MPP=M so the mret lands M-mode on
// this thread's own stack, sp still on the trap frame. MIE is 0 for the whole trap,
// so MPIE is what turns interrupts back on for the stub, which blocks and reschedules.
void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    __asm volatile("csrr %0, mcause" : "=r"(mcause));
    __asm volatile("csrr %0, mepc" : "=r"(mepc));
    __asm volatile("csrr %0, mtval" : "=r"(mtval));
    // mtval is the faulting address only for an access/misaligned cause; for an illegal
    // instruction it holds the instruction bits, which is not an address.
    int addr_valid = 0;
    if (mcause == 1 or mcause == 4 or mcause == 5 or mcause == 6 or mcause == 7)
    {
        addr_valid = 1;
    }
    kickos_fault_record("mcause", mcause, mepc, mtval, addr_valid);
    (void)frame;

    uint32_t const stub = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit);
    __asm volatile("csrw mepc, %0" ::"r"(stub) : "memory");
    uint32_t next;
    __asm volatile("csrr %0, mstatus" : "=r"(next));
    next |= MSTATUS_MPP_M | MSTATUS_MPIE;
    __asm volatile("csrw mstatus, %0" ::"r"(next) : "memory");
}

// --- Unhandled trap (switch.S .Lfault) ----------------------------------------
// The .Lfault shim reads the trap CSRs and passes them here. A true return means
// .Lfault must mret instead of dumping: the redirect above already re-pointed
// mepc/mstatus at the stub. Otherwise dump the context, then hand off to the shared
// dead-end (blink on real HW, exit with a fault status on QEMU/virt so a CTest run
// reports it rather than hanging). ecall-from-M (mcause 11) is demuxed before .Lfault and
// never reaches here; ecall-from-U (8) does, but only from .Ltrap_wild, when the trap
// guard refused the caller's sp before any frame was built.
bool kickos_rv_fault_report(uint32_t mcause, uint32_t mepc, uint32_t mtval,
                            uint32_t mstatus, void* frame)
{
    // Nothing may print above: kpanic_enter's console reclaim is permanent and this
    // fault is survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return true;
    }
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
#if defined(KICKOS_ENABLE_SELFTEST)
    kickos_trapstack_witness_report(); // names a U-mode sp that reached kernel memory
#endif
    // An access fault taken FROM U-mode (mstatus.MPP==0) is a PMP domain violation by an
    // unprivileged thread, on instruction fetch (mcause 1) as well as load (5) / store
    // (7); a fetch from an ungranted region must report the same as a data access. Route it
    // to the kernel reporter that names the thread and exits via the reported-fault path;
    // mtval holds the faulting address. An access fault from M-mode (MPP!=0) is a genuine
    // kernel bug, M-mode bypassing the unlocked PMP entries, so it falls through to the
    // generic dump + kfault_terminate.
    bool const from_user = (mstatus & MSTATUS_MPP_M) == 0;
    if (from_user and (mcause == 1 or mcause == 5 or mcause == 7))
    {
        kickos_isr_fault(mtval, mcause == 7); // never returns (arch_shutdown)
    }
    char const* what = "trap";
    if (mcause == 0)
    {
        what = "instruction address misaligned";
    }
    else if (mcause == 1)
    {
        what = "instruction access fault";
    }
    else if (mcause == 2)
    {
        what = "illegal instruction";
    }
    else if (mcause == 3)
    {
        what = "breakpoint";
    }
    else if (mcause == 4)
    {
        what = "load address misaligned";
    }
    else if (mcause == 5)
    {
        what = "load access fault";
    }
    else if (mcause == 6)
    {
        what = "store address misaligned";
    }
    else if (mcause == 7)
    {
        what = "store access fault";
    }
    else if (mcause == 8)
    {
        // Only .Ltrap_wild sends an ecall here: the demux routes every accepted ecall
        // to .Lecall, so the cause is the refused sp and not the syscall.
        what = "ecall on a refused stack";
    }
    else if (mcause == 12)
    {
        what = "instruction page fault";
    }
    else if (mcause == 13)
    {
        what = "load page fault";
    }
    else if (mcause == 15)
    {
        what = "store page fault";
    }
    ::kickos::kprintf("\n=== RISC-V TRAP (%s) ===\n", what);
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf(KDIAG_F_RV_CAUSE, mcause, mepc);
    ::kickos::kprintf(KDIAG_F_RV_STATUS, mtval, mstatus);
#else
    (void)mepc;
    (void)mtval;
    (void)mstatus;
#endif
    kfault_terminate();
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
// The chip has already set g_clint_msip (+ its timer base). Install the single
// trap vector, enable the software (switch) + timer local interrupts, allow U-mode
// to read the cycle/time counters, and reset the software ISR-depth state.
void kickos_rv32_init(void)
{
    g_isr_depth = 0;

    // mtvec: VECTORED mode (low 2 bits = 01), which is the one mode the ESP32-C6 core
    // supports. Point at the 256B-aligned vector table (switch.S); every slot jumps to
    // trap_entry, which demuxes on mcause. (void(*)() decays to the table base address.)
    uintptr_t tv = reinterpret_cast<uintptr_t>(kickos_rv_mtvec) | 1u;
    __asm volatile("csrw mtvec, %0" ::"r"(tv) : "memory");
    (void)trap_entry; // referenced by the asm vector table, not directly here

    // The trusted trap stack top. trap_entry swaps sp with mscratch on entry, so it
    // must hold this before the first trap (and thus before the first mret to U-mode).
    uintptr_t const trap_sp = reinterpret_cast<uintptr_t>(&g_rv_trap_stack[sizeof(g_rv_trap_stack)]);
    __asm volatile("csrw mscratch, %0" ::"r"(trap_sp) : "memory");

    // Enable the machine software (msip, bit 3 = the deferred switch), machine timer
    // (mtip, bit 7 = the tickless clock), and supervisor software (ssip, bit 1 = the
    // injected-IRQ test channel) local interrupts. ssip only fires via arch_irq_inject
    // (and only where S-mode exists, e.g. qemu-virt); on an M/U-only core the bit is
    // read-only-zero, so enabling it is harmless.
    uint32_t mie = (1u << 3) | (1u << 7) | (1u << 1);
    __asm volatile("csrw mie, %0" ::"r"(mie) : "memory");

    // Let U-mode threads read cycle/time/instret (rdcycle in arch_trace_now, and a
    // userspace clock read) instead of trapping. mcounteren bits CY|TM|IR. The write itself
    // traps on a core without the CSR (arch_rv_has_mcounteren; the ESP32-C6 HP core is one,
    // and that fault hangs bring-up), so it is gated.
    if (arch_rv_has_mcounteren() != 0)
    {
        __asm volatile("csrw mcounteren, %0" ::"r"(0x7u) : "memory");
    }

    // Permissive bootstrap PMP: ONE entry covering the whole address space, R+W+X,
    // U-accessible. RISC-V is fail-CLOSED: once PMP is implemented (it is on this core), a
    // U-mode access with NO matching entry FAULTS, so an unprivileged thread cannot even
    // fetch its first instruction without this entry. It grants U-mode full access, with
    // no isolation until arch_mpu_apply refines per-thread PMP. Use TOR (A=01, top =
    // pmpaddr0<<2) rather than the all-ones NAPOT idiom: the ESP32-C6 PMP does not honor
    // the all-ones-NAPOT match-everything special case (U-mode still takes an
    // instruction-access fault), whereas TOR with pmpaddr0 = 0xFFFFFFFF covers every
    // 32-bit address on both it and QEMU virt. pmpcfg0 byte0 = A=TOR(0x08) | X(0x4) |
    // W(0x2) | R(0x1) = 0x0F.
    __asm volatile("csrw pmpaddr0, %0" ::"r"(0xFFFFFFFFu) : "memory");
    __asm volatile("csrw pmpcfg0, %0" ::"r"(0x0Fu) : "memory");
}

}
