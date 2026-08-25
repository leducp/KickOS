// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-A (AArch64) arch backend: the ISA-generic half of the arch.h seam. The chip layer
// (arch/arm64/chip/virt_arm64) supplies the hardware edges, switch.S the frame and entries.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>

#include <stddef.h>
#include <stdint.h>

// The arch layer carries no kernel headers, so the fault path's three are declared here.
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// Acknowledge one interrupt, route it, complete it. A GIC's register layout is
// architectural and its addresses are not, so ownership follows the addresses.
extern "C" void kickos_armv8a_gic_dispatch(void);

// switch.S.
extern "C" void kickos_armv8a_switch_now(struct arch_context* from, struct arch_context* to);
extern "C" void kickos_armv8a_start(struct arch_context* first);
extern "C" void kickos_armv8a_thread_exit(void);

// The deferred-switch request, read by the IRQ exit in switch.S. Written from ISR context
// with interrupts masked, consumed by the exit that follows.
extern "C"
{
    struct arch_context* kickos_armv8a_switch_from = nullptr;
    struct arch_context* kickos_armv8a_switch_to = nullptr;
}
// switch.S spells these as literal displacements, so a change on one side alone is a silent
// wrong offset.
constexpr size_t ARMV8A_FRAME_SIZE = 800;
constexpr size_t ARMV8A_F_X30 = 240;
constexpr size_t ARMV8A_F_ELR = 248;
constexpr size_t ARMV8A_F_SPSR = 256;
constexpr size_t ARMV8A_F_SP_EL0 = 264;
constexpr size_t ARMV8A_F_FPCR = 272;
constexpr size_t ARMV8A_F_FPSR = 280;
constexpr size_t ARMV8A_F_FP = 288;
static_assert(ARMV8A_F_FP + 32 * 16 == ARMV8A_FRAME_SIZE,
              "the 32 q registers must end exactly at the frame's top");
static_assert(ARMV8A_F_FPSR + 8 == ARMV8A_F_FP, "FPSR abuts the vector bank");
static_assert(ARMV8A_F_FPCR + 8 == ARMV8A_F_FPSR, "FPCR abuts FPSR");
static_assert(ARMV8A_F_SP_EL0 + 8 == ARMV8A_F_FPCR, "SP_EL0 abuts FPCR");
static_assert(ARMV8A_F_FP % 16 == 0, "the vector bank needs 16-byte alignment for stp q");

// The knob ARCH_KERNEL_STACKS_MANDATORY forces, asserted rather than assumed.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "armv8a selects ARCH_KERNEL_STACKS_MANDATORY, so the blocks must exist");

// The incoming thread's kernel block top, published because the vector entry cannot reach
// the TCB. One core, so one cell; M7 moves it behind the TPIDR_EL1 per-CPU pointer.
extern "C"
{
    uintptr_t kickos_armv8a_kernel_sp = 0;
}

#if defined(KICKOS_TLS) && KICKOS_TLS
// The carve sits below the stack arch_context_init is handed, so recovering the TLS block
// base from those bounds is a subtraction.
namespace kickos
{
    size_t tls_block_size();
}

// Called wherever a switch is DECIDED, which is safe ahead of the physical swap because no
// privileged code reads this register (check_no_privileged_tls.sh).
static void armv8a_seat_thread_pointer(struct arch_context const* to)
{
    __asm volatile("msr tpidr_el0, %0" ::"r"(to->tls_base) : "memory");
}
#endif

// An unprivileged thread returns through the user-side stub; the privileged one is a kernel
// symbol EL0 cannot call.
extern "C" void kickos_user_thread_return(void);
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S uses CTX_SP 0");
static_assert(ARMV8A_FRAME_SIZE % 16 == 0, "SP must stay 16-byte aligned");

namespace
{
    // PSTATE.I within DAIF, which is read and written at bit 7.
    constexpr uint64_t DAIF_I = 1ULL << 7;

    // The IRQ entry is the only writer and runs with interrupts masked by the exception
    // itself, so no atomic is owed.
    uint32_t g_isr_depth = 0;

    // SPSR for a thread: debug masked, interrupts and SError live. EL1h means "EL1 with its
    // own SP"; EL0t is 0, EL0 having only SP_EL0 to run on.
    constexpr uint64_t SPSR_EL1H_IRQ_ON = (1ULL << 9) | 0x5ULL;
    constexpr uint64_t SPSR_EL0T_IRQ_ON = (1ULL << 9);

    // Indexed by the slot vectors.S passes, so a dump names which of the sixteen fired.
    char const* const VECTOR_NAMES[] = {
        "SP_EL0 sync", "SP_EL0 irq", "SP_EL0 fiq", "SP_EL0 serror",
        "SP_EL1 sync", "SP_EL1 irq", "SP_EL1 fiq", "SP_EL1 serror",
        "EL0 sync",    "EL0 irq",    "EL0 fiq",    "EL0 serror",
        "EL0/A32 sync", "EL0/A32 irq", "EL0/A32 fiq", "EL0/A32 serror",
    };
    constexpr unsigned long VECTOR_SLOTS = sizeof(VECTOR_NAMES) / sizeof(VECTOR_NAMES[0]);
    static_assert(VECTOR_SLOTS == 16, "the table in vectors.S has sixteen slots");

    // The slot a fault in privileged code arrives at, and the only one this stage services.
    constexpr unsigned long SLOT_SP_EL1_SYNC = 4;

    // FAR_EL1 is stale outside the abort, alignment and watchpoint classes, so it is read
    // only when the class says so and ESR_EL1.FnV is clear (MMFAR behind MMARVALID).
    bool far_is_valid(uint64_t esr)
    {
        unsigned const ec = static_cast<unsigned>((esr >> 26) & 0x3F);
        bool classed = false;
        if (ec == 0x20 or ec == 0x21) // instruction abort, lower EL / current EL
        {
            classed = true;
        }
        if (ec == 0x24 or ec == 0x25) // data abort, lower EL / current EL
        {
            classed = true;
        }
        if (ec == 0x22 or ec == 0x26) // PC alignment / SP alignment
        {
            classed = true;
        }
        if (ec == 0x34 or ec == 0x35) // watchpoint, lower EL / current EL
        {
            classed = true;
        }
        if (not classed)
        {
            return false;
        }
        return (esr & (1ULL << 10)) == 0; // FnV
    }
}

extern "C"
{

// --- Context / switching ----------------------------------------------------
// Builds the frame switch.S resumes. Every resume being an eret, the entry needs no
// trampoline: ELR carries it and x0 its argument.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    ctx->stack_lo = reinterpret_cast<uintptr_t>(stack_base);
    ctx->stack_hi = ctx->stack_lo + stack_size;
    ctx->kernel_sp = 0;
#if defined(KICKOS_TLS) && KICKOS_TLS
    // A subtraction rather than a mask, so stack_lo owes no alignment past the ABI's 16.
    ctx->tls_base = ctx->stack_lo - ::kickos::tls_block_size();
#endif

    // At the top of the stack, 16-byte aligned. For an EL0 thread that is its USER stack and
    // the one eret that starts it consumes this frame; every later frame lands on the kernel
    // block, which thread_create has not seated yet (switch.S reads it from the record).
    uintptr_t top = (ctx->stack_hi) & ~static_cast<uintptr_t>(15);
    uintptr_t base = top - ARMV8A_FRAME_SIZE;
    uint64_t* f = reinterpret_cast<uint64_t*>(base);
    for (size_t i = 0; i < ARMV8A_FRAME_SIZE / sizeof(uint64_t); i++)
    {
        f[i] = 0;
    }
    f[0] = reinterpret_cast<uint64_t>(arg);                         // x0
    f[ARMV8A_F_ELR / 8] = reinterpret_cast<uint64_t>(entry);
    // Interrupts LIVE in both arms, and this is the system's FIRST enable: nothing earlier
    // can perform it, arch_irq_restore clearing only what its own paired save set. RISC-V
    // seats mstatus.MPIE here for the same reason.
    if (privileged != 0)
    {
        f[ARMV8A_F_X30 / 8] = reinterpret_cast<uint64_t>(&kickos_armv8a_thread_exit);
        f[ARMV8A_F_SPSR / 8] = SPSR_EL1H_IRQ_ON;
    }
    else
    {
        f[ARMV8A_F_X30 / 8] = reinterpret_cast<uint64_t>(&kickos_user_thread_return);
        f[ARMV8A_F_SPSR / 8] = SPSR_EL0T_IRQ_ON;
        f[ARMV8A_F_SP_EL0 / 8] = top; // its own stack, the consumed frame included again
    }
    ctx->sp = base;
}

// Resumes the thread at `entry`, privileged, on its own KERNEL block: the death path must
// not run on memory an unprivileged thread can write.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp SURVIVES THE REBUILD: arch_context_init clears it, which is right for a
    // fresh TCB and wrong for a live pool thread whose block is seated by slot.
    uintptr_t const kernel_sp = ctx->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    uintptr_t const tls_base = ctx->tls_base;
#endif
#if KICKOS_KERNEL_STACKS
    // stack_lo and stack_hi are saved and put back: arch_context_init derives them from what
    // it is handed, and handing it the block would leave the context describing kernel .bss
    // as this thread's stack. check_death_stack_seating.sh holds this shape.
    //
    // The `if` covers a TCB outside the pool, which has no block. Idle is that TCB.
    if (kernel_sp != 0)
    {
        uintptr_t const lo = ctx->stack_lo;
        uintptr_t const hi = ctx->stack_hi;
        void* const block = reinterpret_cast<void*>(kernel_sp - KICKOS_KERNEL_STACK_SIZE);
        arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1);
        ctx->stack_lo = lo;
        ctx->stack_hi = hi;
        ctx->kernel_sp = kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
        // Survives for the same reason: the rebuild derived it from the BLOCK, and a
        // thread's thread_locals live where they were carved.
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

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (g_isr_depth != 0)
    {
        // DEFERRED, which arch.h permits: the interrupted thread's state is already in the
        // frame the IRQ entry built, so the swap is two stores at the exception exit.
        kickos_armv8a_switch_from = from;
        kickos_armv8a_switch_to = to;
        kickos_armv8a_kernel_sp = to->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
        armv8a_seat_thread_pointer(to);
#endif
        return;
    }
    kickos_armv8a_kernel_sp = to->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    armv8a_seat_thread_pointer(to);
#endif
    kickos_armv8a_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot; // abandoned, as arch.h permits and the M-profile backend also does
    kickos_armv8a_kernel_sp = first->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    armv8a_seat_thread_pointer(first);
#endif
    kickos_armv8a_start(first);

    while (true)
    {
        __asm volatile("wfi");
    }
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

// --- Critical section -------------------------------------------------------
// NESTING-SAFE: the state is the one bit this touches and the restore clears only what its
// own save set. A wholesale `msr daif, saved` would write back D, A and F too, clobbering any
// change made between the two.
arch_irq_state_t arch_irq_save(void)
{
    uint64_t daif = 0;
    __asm volatile("mrs %0, daif" : "=r"(daif));
    __asm volatile("msr daifset, #2" ::: "memory"); // DAIFSet bit 1 == I
    return static_cast<arch_irq_state_t>(daif & DAIF_I);
}

void arch_irq_restore(arch_irq_state_t state)
{
    if (state == 0) // I was clear before the save, so the save is what masked it
    {
        __asm volatile("msr daifclr, #2" ::: "memory");
    }
}

// The IRQ dispatcher alone bumps it, so it reads FALSE inside syscall dispatch as arch.h
// requires: the kernel's blocking primitives depend on that.
int arch_in_isr(void)
{
    return g_isr_depth != 0;
}

// --- Clocks -----------------------------------------------------------------
uint32_t arch_cpu_clock_hz(void)
{
    return 0;
}

// --- MPU: none on this arch ------------------------------------------------
// Protection here is the stage-1 tables rather than a region MPU. arch_mpu_min_region
// returning 0 makes arch_ram_region_size 16-byte granular, so region_pow2 is never read.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

void kickos_arch_mpu_commit(void) {}

size_t arch_mpu_min_region(void)
{
    return 0;
}

int arch_mpu_region_pow2(void)
{
    return 0;
}

bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    (void)base;
    (void)size;
    return false;
}

int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_REFUSED;
}

// Rule 7 (arch.h): AArch64 has no bit-band alias.
int arch_bitband_present(void)
{
    return 0;
}

// --- Interrupt controller ---------------------------------------------------
// The mask/unmask/clear/inject quartet belongs to the CHIP here: the NVIC sits at a
// core-fixed address, a GIC wherever the SoC put it, so ownership follows the addresses. A
// second arm64 chip hoists the layout up here and leaves the bases down there.

// --- Exceptions -------------------------------------------------------------
// Every reporting slot lands here with its index and the interrupted x30 (vectors.S).
// Nothing resumes: the vector reaches C by a plain branch and builds no resumable frame, so
// kickos_fault_kill_thread could not be honoured even if it answered true. The thread-kill
// path arrives with the enforcement that can act on it.
void kickos_armv8a_exception(unsigned long slot, unsigned long lr)
{
    kpanic_enter(); // mask IRQs + force the sync console + flush queued bytes, in order
    if (slot == SLOT_SP_EL1_SYNC)
    {
        ::kickos::kprintf("\n=== ARMV8A EXCEPTION ===\n");
    }
    else
    {
        ::kickos::kprintf("\n=== ARMV8A EXCEPTION (unexpected vector) ===\n");
    }
#if KICKOS_PANIC_DUMP
    uint64_t esr = 0;
    uint64_t elr = 0;
    uint64_t spsr = 0;
    uint64_t far = 0;
    __asm volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    __asm volatile("mrs %0, far_el1" : "=r"(far));

    char const* name = "out of range";
    if (slot < VECTOR_SLOTS)
    {
        name = VECTOR_NAMES[slot];
    }
    ::kickos::kprintf(KDIAG_F_A64_VECTOR, name, lr);
    ::kickos::kprintf(KDIAG_F_A64_SYND, esr, elr);
    ::kickos::kprintf(KDIAG_F_A64_SPSR, spsr);
    if (far_is_valid(esr))
    {
        ::kickos::kprintf(KDIAG_F_A64_FAR, far);
    }
    else
    {
        ::kickos::kprintf(KDIAG_F_A64_FAR_NA);
    }
    // No arch_fault_report_extra: its default TU is armv7m's alone, so a call here is an
    // undefined reference for a hook with no protection seam to report about.
#else
    (void)slot;
    (void)lr;
#endif
    kfault_terminate();
}

// The IRQ vector's C half (vectors.S). Which interrupt fired is the CONTROLLER's answer, so
// the chip is asked; this only keeps the depth that makes arch_in_isr honest.
void kickos_armv8a_irq(void)
{
    g_isr_depth++;
    kickos_armv8a_gic_dispatch();
    g_isr_depth--;
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

}
