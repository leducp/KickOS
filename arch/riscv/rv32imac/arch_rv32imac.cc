// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC arch backend, ISA-generic half; switch.S and arch/riscv/chip/ hold the rest.

#include <kickos/arch/arch.h>
#include <kickos/arch/idle_floor.h>
#include <kickos/arch/rv_trap_stack.h>
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>
#include <kickos/trace/record.h>

#include <bit>

#include <stddef.h>
#include <stdint.h>

static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_RISCV,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_RISCV for rv32imac");

namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kickos_isr_fault(uintptr_t addr, int is_write);

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// switch.S hard-codes the save-frame layout. The frame (on the thread's own stack,
// ctx.sp = its base, low to high) is 32 words / 128 bytes:
//   [0 mepc][1 mstatus][2 ra][3 t0][4 t1][5 t2][6 s0][7 s1][8 a0]..[15 a7]
//   [16 s2]..[25 s11][26 t3][27 t4][28 t5][29 t6][30 sp][31 pad]
// gp and tp are not in the frame: .Lrestore writes both on every resume.
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

    // MIE is KICKOS_RV32_MSTATUS_MIE (kickos/arch/irq_inline.h).
    constexpr uint32_t MSTATUS_MPIE = 1u << 7;
    constexpr uint32_t MSTATUS_MPP_M = 3u << 11; // MPP = machine (U = 0)

}

// switch.S hard-codes each offset below as a literal displacement; rv_trap_stack.h holds
// the single definition.
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
#if defined(KICKOS_TLS) && KICKOS_TLS
static_assert(offsetof(struct arch_context, tls_base) == KICKOS_RV_CTX_OFF_KERNEL_SP + 4,
              ".Lrestore loads ctx.tls_base at F_CTX_TLS_BASE");

namespace kickos
{
    size_t tls_block_size();
}
#endif
// The panic reporter's stack (kernel/init/console.cc) is sized by Kconfig; a board may only
// RAISE it.
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_RV_PANIC_FRAME + KICKOS_RV_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");
// The link checks the same floor above idle's thread-local carve.
static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_ARCH_IDLE_FLOOR,
              "the switch frame does not fit this board's idle stack");

static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_RV_TRAP_SP_ALIGN == 0,
              "KICKOS_KERNEL_STACK_SIZE must be a multiple of KICKOS_RV_TRAP_SP_ALIGN, "
              "or a kernel stack's top does not land on the alignment the prologue "
              "requires");
static_assert(sizeof(struct arch_context) >= KICKOS_RV_CTX_OFF_KERNEL_SP + sizeof(uint32_t),
              "the guard reads a word past the end of struct arch_context");

// The frame the prologue builds and the one arch_context_init fabricates are one object.
static_assert(FRAME_WORDS * 4 == KICKOS_RV_TRAP_FRAME,
              "the fabricated frame and the trap guard disagree on the frame size");
// The ecall frame plus the msip frame the deferred switcher builds below the dispatch, both
// on the kernel stack.
static_assert(KICKOS_RV_TRAP_FRAME_SYS == 2 * KICKOS_RV_TRAP_FRAME,
              "the syscall requirement must hold exactly two frames");
// SYSPRIV is the SYS chain with a subtree removed. Both are scraped as plain immediates, so
// only this catches a swap between them.
static_assert(KICKOS_RV_TRAP_KERNEL_DEPTH_SYSPRIV <= KICKOS_RV_TRAP_KERNEL_DEPTH_SYS,
              "the tail-excluded syscall depth exceeds the tail-included one");
// A privileged thread's ecall arrives with MPP=M, so .Ltrap_from_m_ctx keeps frame and
// dispatch on its OWN sp. No bound refuses an M-mode sp, so a floor under this requirement
// overflows rather than refuses.
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RV_TRAP_NEED_SYSPRIV,
              "the spawn floor cannot hold a privileged thread's own syscall dispatch");
// The fault and slay stubs relocate to the thread's kernel block (kickos_fault_stack_top
// answers ctx.kernel_sp), so the block above its canary holds them; kickos_thread_return does
// not relocate, so the spawn floor holds it.
static_assert(KICKOS_RV_TRAP_NEST_EXIT == KICKOS_RV_TRAP_FRAME,
              "the death path's frame term is the msip frame a reschedule puts below it");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV_TRAP_NEST_EXIT + KICKOS_RV_TRAP_KERNEL_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_RV_TRAP_NEST_EXIT + KICKOS_RV_TRAP_KERNEL_DEPTH_RET,
              "the spawn floor cannot hold a privileged thread's entry return");
// With no block seated every U-mode trap takes the refusal path. ARCH_KERNEL_STACKS_MANDATORY
// forces the knob to 1; this fires if that select is dropped.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "rv32imac's trap entry builds every U-mode frame on ctx.kernel_sp");
// A syscall's deepest descent is the ecall frame, the dispatch, the msip frame a blocking
// dispatch takes at that depth, and the switcher. The block's lowest word is the overflow
// canary (kernel/thread/thread.cc), so the requirement must fit ABOVE it.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV_TRAP_FRAME_SYS + KICKOS_RV_TRAP_KERNEL_DEPTH_SYS
                      + KICKOS_RV_TRAP_SWITCH_DEPTH,
              "KICKOS_KERNEL_STACK_SIZE is below the rv32imac syscall kernel-stack "
              "requirement plus its canary word: raise the per-arch default in Kconfig, "
              "never the depth, which is a measurement");
// Neither the .Lintr nor the .Lfault figure is ordered against the syscall depth, so the
// clause above does not imply these.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_KERNEL_DEPTH,
              "the kernel block cannot hold an interrupt's dispatch plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_NESTED_DEPTH,
              "the kernel block cannot hold an accepted U-mode fault's reporter plus its "
              "canary word");

extern "C"
{
    // Shared with switch.S.
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_current = nullptr;
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_next = nullptr;

    // switch.S loads each as a plain word at offset 0.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // Bumped only by the timer, soft and external trap paths (switch.S), so arch_in_isr() is
    // false throughout syscall_dispatch and the msip switch.
    uint32_t g_isr_depth = 0;

    // Per-hart trap stack. mscratch holds its top while a thread runs, so trap_entry swaps
    // onto it before touching the interrupted sp and a U-mode sp never selects where the
    // prologue's scratch lands. It also carries every M-mode trap frame that is not a
    // thread's saved context, and the kernel C below it.
    alignas(16) uint8_t g_rv_trap_stack[KICKOS_NUM_CORES][KICKOS_RV_TRAP_STACK_SIZE];
    static_assert(KICKOS_RV_TRAP_STACK_SIZE % KICKOS_RV_TRAP_SP_ALIGN == 0,
                  "the trap-stack top must land on the alignment the prologue requires");
    static_assert(KICKOS_RV_TRAP_STACK_SIZE > KICKOS_RV_TRAP_FRAME,
                  "a nested frame would fill the whole trap stack, leaving nowhere for the "
                  "kernel C below it; how much is enough is what the gate measures");

    // This hart's CLINT msip register; the chip's arch_init sets it.
    volatile uint32_t* g_clint_msip = nullptr;

#if KICKOS_BENCH
    // Null means `rdcycle`. A core whose `rdcycle` traps (the ESP32-C6 HP core has no Zicntr)
    // points this at a free-running MMIO counter before the first switch.
    volatile uint32_t* g_bench_cycle_src = nullptr;
#endif

    void trap_entry(void);
    void kickos_rv_mtvec(void);
    void kickos_thread_return(void);
    void kickos_user_thread_return(void);
}

extern "C"
{

// File-local symbols below are `static`, never an anonymous namespace: under C language
// linkage an anonymous namespace still emits an unmangled GLOBAL symbol.

extern uint32_t SystemCoreClock;
uint64_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}

// --- Context init --------------------------------------------------------------
// The fabricated frame must match what the msip switcher saves: arch_start restores it and
// mret's into entry(arg).
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
        mstatus |= MSTATUS_MPP_M;
    }
    else
    {
        ret = reinterpret_cast<uint32_t>(kickos_user_thread_return); // MPP stays U (0)
    }

    f[F_MEPC] = reinterpret_cast<uint32_t>(entry);
    f[F_MSTATUS] = mstatus;
    f[F_RA] = ret;
    f[F_A0] = reinterpret_cast<uint32_t>(arg);
    // The sp .Lrestore resumes on; nothing else seats it in a fabricated frame.
    f[F_SP] = static_cast<uint32_t>(top);
    ctx->sp = reinterpret_cast<uint32_t>(f);

    // trap_entry refuses an interrupted U-mode sp outside [stack_lo, stack_hi] before it
    // stores a frame through it.
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);
#if defined(KICKOS_TLS) && KICKOS_TLS
    // The carve sits directly below the stack this is handed.
    ctx->tls_base = ctx->stack_lo - static_cast<uint32_t>(::kickos::tls_block_size());
#endif

    // ctx->kernel_sp IS DELIBERATELY UNTOUCHED: thread_create seats it before this call, and
    // its zero (no block seated) is what the trusted entry's refusal path keys on.
}

// ctx->sp is the frame base, where .Lrestore reloads a0 from.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp)[F_A0] = result;
}

// Works entirely through the saved frame, so it applies to a context that is not running;
// arch_fault_redirect_to_exit is the live-CSR half.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp is put back explicitly: lost, the thread carries 0 through its own teardown
    // and every trap on the way takes the entry's refusal path.
    uintptr_t const kernel_sp = ctx->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    uint32_t const tls_base = ctx->tls_base;
#endif
#if KICKOS_KERNEL_STACKS
    // The stub is rebuilt at the TOP of the thread's own kernel block, so no privileged frame
    // lands on memory the thread or a domain sibling can write, and the block requirement is
    // the MAX of the dispatch and exit classes rather than their sum.
    //
    // stack_lo and stack_hi are put back, or the context would describe the kernel block as
    // this thread's stack. tests/static/check_death_stack_seating.sh holds this shape.
    if (kernel_sp != 0)
    {
        uint32_t const lo = ctx->stack_lo;
        uint32_t const hi = ctx->stack_hi;
        void* const block = reinterpret_cast<void*>(
            kernel_sp - KICKOS_KERNEL_STACK_SIZE);
        arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1);
        ctx->stack_lo = lo;
        ctx->stack_hi = hi;
        ctx->kernel_sp = kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
        ctx->tls_base = tls_base;
#endif
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    ctx->tls_base = tls_base;
#endif
}

// --- Switch ------------------------------------------------------------------
// Always deferred, in ISR and thread context alike: the physical swap happens in the msip
// trap. Entered with mstatus.MIE=0, so the pended msip fires once the lock releases or the
// current trap returns.
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // the switcher saves g_arch_current
    g_arch_next = to;
    *g_clint_msip = 1; // pend machine software interrupt
}

int arch_in_isr(void)
{
    return g_isr_depth != 0;
}

// --- Trace clock --------------------------------------------------------------
// 32-bit and wraps; the host reconstructs absolute time from the SESSION clock_hz anchors.
// U-mode can read it only where kickos_rv32_init enabled mcounteren.CY.
//
// Without Zicntr this instruction is illegal in M-mode too; the ESP32-C6 HP core is such a
// part and declares KICKOS_HAVE_TRACE_CLOCK 0. A chip whose counter is not rdcycle overrides
// this function.
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

// cfg byte: A=NAPOT (0b11<<3) | R | W? | X?. attr is the U-mode rights; M-mode bypasses these
// unlocked entries.
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

// A region PMP cannot name is left cfg 0, which grants no access: the encoding fails closed.
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

// A POINTER into the caller's TCB, not a copy: every commit is preceded by an apply inside
// the SAME MIE=0 window (the msip trap, the .Lecall fastpath tail, arch_start, and
// arch_mpu_apply_now), so nothing can rewrite the image in between. Thread slots come from a
// static pool and are never freed, so a stale pointer still addresses valid storage.
static struct arch_mpu_encoded const* g_pend_image = nullptr;

#if KICKOS_BENCH
// Declared rather than included: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_mpu_commit(uint32_t delta);

// always_inline: an out-of-line copy charges two call/ret pairs to a delta PH_NULL prices as
// two bare counter reads.
static __attribute__((always_inline)) inline uint32_t mpu_bench_cyc(void)
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

// STASH-ONLY: kickos_arch_mpu_commit writes the PMP CSRs from the .Lswitch epilogue AFTER the
// physical swap. An eager apply would run the OUTGOING user thread under the incoming PMP set
// until msip fires, faulting on its own stack.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}

// Runs in the M-mode trap with MIE=0, so it must NOT toggle MIE.
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
    // Addresses first, then the two cfg words, which activate the entries. Overwriting the
    // bootstrap TOR entry is safe: the kernel is in M-mode here and bypasses PMP. csrw takes
    // an IMMEDIATE CSR number, so the entries are spelled out rather than indexed.
    //
    // EVERY ENTRY, EVERY COMMIT, by measurement: skipping an entry costs a load and a taken
    // branch where writing it costs a load and a csrw (docs/reference/invariants.md,
    // mpu-commit-writes-what-changed).
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
    // Orders the PMP update before the mret to U-mode; the priv spec only promises the
    // writing hart sees it on its next access.
    __asm volatile("fence" ::: "memory");
#if KICKOS_BENCH
    kickos_bench_mpu_commit(mpu_bench_cyc() - bench_start);
#endif
}

// THE STASH IS RESTORED: a switch may already be pended behind the caller and the stash is
// ONE cell, so leaving this set in it would program the CALLER's regions onto the incoming
// thread. Self-bracketed: an interrupt between the two writes could decide a switch, and the
// restore would then drop the image that switch stashed.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    arch_irq_state_t const irq = arch_irq_save();
    struct arch_mpu_encoded const* const pend = g_pend_image;
    g_pend_image = image;
    kickos_arch_mpu_commit();
    g_pend_image = pend;
    arch_irq_restore(irq);
}
#else
// KICKOS_HAVE_MPU=0: the permissive bootstrap PMP stays in place for the life of the image.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}
void kickos_arch_mpu_commit(void) {}

void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    arch_mpu_apply(regions, n, image);
}
#endif

size_t arch_mpu_min_region(void)
{
    return 8u; // RISC-V PMP NAPOT minimum region size
}

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

// A PMP entry carries no memory type, and the parts in tree reach the arena uncached, so it
// is already in the state a nocache grant asks for.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

int arch_bitband_present(void)
{
    return 0;
}


// --- Interrupt controller (software-injected test scaffolding) ---------------
// ONE physical doorbell carries every logical line and g_inject_line tells the trap which
// line it was, so arch_irq_mask/unmask stay pure-software and decoupled from the physical
// interrupt.
//
// virt: the SUPERVISOR SOFTWARE interrupt (mip.SSIP, software-writable, mcause=1), which
// needs S-mode, present on the QEMU virt CPU.
//
// ESP32-C6 (chip_esp32c6.cc): the HP core is M/U-only with no SSIP, so its override raises a
// real machine interrupt from a FROM_CPU source on a dedicated CPU interrupt ID. The C6
// reports mcause = that ID rather than the standard 11, and switch.S demuxes it to .Lext.
static constexpr uint32_t MIP_SSIP = 1u << 1;

// bit set = line masked. All lines start MASKED at reset (the arch.h reset contract).
static uint32_t g_irq_masked = 0xFFFFFFFFu;
static kickos::Atomic<int, kickos::Order::RELAXED> g_inject_line = -1;

// bit set = a raise landed on this software line while masked, latched one-deep and
// redelivered at unmask. A real PLIC line holds its own pending in hardware.
static uint32_t g_irq_pending = 0;

// Chip hooks, each fallback in its own TU (<symbol>_default.cc). arch_rv_hw_{un,}mask run
// INSIDE the arch critical section; arch_rv_ext_eoi de-asserts a level source.
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
    // Only a REAL line reaches the controller; injected lines live in the bitmask above.
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
    // Inside the critical section (mstatus.MIE=0) so an INTMTX/PLIC reconfigure cannot
    // glitch in the controller's transient state (C6 TRM 1.6.3.2: configure with MIE
    // cleared and a FENCE).
    arch_rv_hw_unmask(line);
    // A raise taken while masked redelivers through the doorbell with MIE=0, so it fires at
    // arch_irq_restore on the normal ISR path rather than as a direct post.
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
    // Software-inject lines only; a real PLIC line holds its pending in hardware.
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
    // An ISR reaching arch_irq_mask/unmask touches the same words.
    arch_irq_state_t s = arch_irq_save();
    if ((g_irq_masked & (1u << irq)) != 0)
    {
        g_irq_pending |= (1u << irq);
    }
    else
    {
        g_inject_line = irq; // BEFORE the raise, so the trap sees it
        arch_rv_inject_deliver(irq);
    }
    arch_irq_restore(s);
}

// SSIP dispatch (switch.S .Lssoft), ISR context.
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

// External-doorbell dispatch (switch.S .Lext), ISR context. The EOI comes first so a level
// source cannot re-fire.
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

// Out of a bench build: it sits inside the window kernel/bench/bench.cc's injected-IRQ arm
// measures.
#if defined(KICKOS_ENABLE_SELFTEST) && !KICKOS_BENCH
// Nested-trap witness (arch.h), from .Lintr once the whole frame is saved at `frame`. An
// interrupt taken with MPP=M interrupted the kernel; in a syscall dispatch its sp is the
// CALLING THREAD'S, at any depth, with no bound applied. mstatus comes from the frame rather
// than the live CSR because .Lrestore is what will consume it.
void kickos_rv_nested_witness(void* frame)
{
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    if ((f[F_MSTATUS] & MSTATUS_MPP_M) == 0)
    {
        return; // interrupted U-mode, on the bounds-checked thread stack
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
// mstatus.MPP is valid whatever the stack did, but it is NOT the thread's identity: .Lecall
// runs syscall dispatch in M-mode, so a fault there is a kernel bug and MPP says so.
//
// A refused sp (wild, misaligned, or no block seated) leaves the frame on the per-hart trap
// stack, and the M-mode prologue bypasses the unlocked PMP entries, so the kernel-stack
// bounds test is what catches such a frame.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t mstatus;
    __asm volatile("csrr %0, mstatus" : "=r"(mstatus));
    if ((mstatus & MSTATUS_MPP_M) != 0)
    {
        return false;
    }
    return kickos_fault_frame_on_kernel_stack(frame, FRAME_WORDS * 4);
}

// Mirrors .Lecall: mepc at the stub and MPP=M so the mret lands M-mode, sp still on the trap
// frame. MIE is 0 for the whole trap, so MPIE is what re-enables interrupts for the stub,
// which blocks and reschedules.
void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    __asm volatile("csrr %0, mcause" : "=r"(mcause));
    __asm volatile("csrr %0, mepc" : "=r"(mepc));
    __asm volatile("csrr %0, mtval" : "=r"(mtval));
    // mtval is an address only for an access or misaligned cause; for an illegal
    // instruction it holds the instruction bits.
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

// --- Wild sp refused by the trap entry (switch.S .Ltrap_wild) -----------------
// Returns the context to resume, or null to fall through to the generic dump.
//
// NOTHING MAY PANIC HERE. kpanic_enter masks IRQs irreversibly and belongs to .Lfault's
// terminating path alone.
//
// g_arch_current and NOT the scheduler's current: it tracks the PHYSICAL switch, so it names
// the thread whose sp was refused even when a booked switch has already published another.
struct arch_context* kickos_rv_contain_wild_sp(uint32_t sp)
{
    // The name is read here, on the trap stack, and never from the exit stub: a stack
    // overflow reaches this refusal before the PMP-denial report, so it would die anonymously.
    char const* who = "?";
    struct arch_context* const next = kickos_thread_contain_wild_stack(g_arch_current, &who);
    if (next == nullptr)
    {
        return nullptr;
    }
#if defined(KICKOS_ENABLE_SELFTEST)
    // Read here as well as on .Lfault's path: it witnesses what the prologue wrote through
    // the refused sp, which containment does not change.
    kickos_trapstack_witness_report();
#endif
    // tests/lib/panic.ere matches "=== RISC-V TRAP", so spelling this one that way would
    // make assert_no_panic blind to the difference.
    ::kickos::kprintf("\n=== RISC-V CONTAINED (wild stack) ===\n");
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  thread '%s' sp=0x%x\n", who, static_cast<unsigned>(sp));
#else
    (void)sp;
    (void)who;
#endif
    return next;
}

// --- Unhandled trap (switch.S .Lfault) ----------------------------------------
// A TRUE return means .Lfault must mret instead of dumping: the redirect already re-pointed
// mepc/mstatus at the stub. ecall-from-U (8) reaches here only from .Ltrap_wild, when the
// guard refused the caller's sp before any frame was built.
bool kickos_rv_fault_report(uint32_t mcause, uint32_t mepc, uint32_t mtval,
                            uint32_t mstatus, void* frame)
{
    // Nothing may print above this: kpanic_enter's console reclaim is permanent and this
    // fault is survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return true;
    }
    kpanic_enter();
#if defined(KICKOS_ENABLE_SELFTEST)
    kickos_trapstack_witness_report();
#endif
    // An access fault FROM U-mode (fetch 1, load 5, store 7) is a PMP domain violation. From
    // M-mode it is a kernel bug, M-mode bypassing the unlocked PMP entries, and takes the
    // generic dump.
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

// --- One-time core bring-up ------------------------------------------------
// The chip has already set g_clint_msip and its timer base.
void kickos_rv32_init(void)
{
    g_isr_depth = 0;

    // VECTORED mode (low 2 bits = 01), the only mode the ESP32-C6 core supports; the table
    // in switch.S is 256B-aligned.
    uintptr_t tv = reinterpret_cast<uintptr_t>(kickos_rv_mtvec) | 1u;
    __asm volatile("csrw mtvec, %0" ::"r"(tv) : "memory");
    (void)trap_entry; // referenced by the asm vector table, not directly here

    // mscratch must hold this before the first trap, so before the first mret to U-mode.
    // Sized on the ROW rather than the array, so a second core takes its own and not the far
    // end of everyone's.
    uint8_t* const trap_stack = g_rv_trap_stack[arch_cpu_id()];
    uintptr_t const trap_sp =
        reinterpret_cast<uintptr_t>(&trap_stack[KICKOS_RV_TRAP_STACK_SIZE]);
    __asm volatile("csrw mscratch, %0" ::"r"(trap_sp) : "memory");

    // msip (bit 3, the deferred switch), mtip (bit 7, the tickless clock) and ssip (bit 1,
    // the injected-IRQ test channel). On an M/U-only core ssip is read-only-zero.
    uint32_t mie = (1u << 3) | (1u << 7) | (1u << 1);
    __asm volatile("csrw mie, %0" ::"r"(mie) : "memory");

    // mcounteren CY|TM|IR, so U-mode reads of cycle/time/instret do not trap. The write
    // itself traps on a core without the CSR (the ESP32-C6 HP core) and hangs bring-up.
    if (arch_rv_has_mcounteren() != 0)
    {
        __asm volatile("csrw mcounteren, %0" ::"r"(0x7u) : "memory");
    }

    // Permissive bootstrap PMP: ONE entry over the whole address space, R+W+X, U-accessible.
    // PMP is fail-CLOSED, so without it an unprivileged thread cannot fetch its first
    // instruction.
    //
    // TOR (A=01, top = pmpaddr0<<2) and not the all-ones NAPOT idiom: the ESP32-C6 PMP does
    // not honor all-ones NAPOT as match-everything and U-mode still takes an
    // instruction-access fault, whereas TOR with pmpaddr0 = 0xFFFFFFFF covers every 32-bit
    // address on both it and QEMU virt. pmpcfg0 byte0 = TOR(0x08)|X(0x4)|W(0x2)|R(0x1).
    __asm volatile("csrw pmpaddr0, %0" ::"r"(0xFFFFFFFFu) : "memory");
    __asm volatile("csrw pmpcfg0, %0" ::"r"(0x0Fu) : "memory");
}

}
