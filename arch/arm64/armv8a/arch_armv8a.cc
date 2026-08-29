// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-A (AArch64) arch backend: the ISA-generic half of the arch.h seam. The chip layer
// (arch/arm64/chip/virt_arm64) supplies the hardware edges, switch.S the frame and entries.

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
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

extern "C" void kickos_armv8a_gic_dispatch(void);

// aspace_armv8a.cc. Called on the TERMINAL path only: it discards the running space, so the
// contained path must not use it, that one printing the dead thread's name out of app text.
extern "C" void kickos_armv8a_ttbr0_to_boot(void);

// switch.S.
extern "C" void kickos_armv8a_switch_now(struct arch_context* from, struct arch_context* to);
extern "C" void kickos_armv8a_start(struct arch_context* first);
extern "C" void kickos_armv8a_thread_exit(void);

// kickos_armv8a_ctx_current names the context PHYSICALLY on the CPU, which is not arch_switch's
// `from`: one ISR can reschedule several times, and every call after the first names a thread
// the scheduler has merely published, whose registers are still nowhere. The IRQ exit in
// switch.S saves through this cell and re-seats it to the incoming context, so the request below
// is a target alone and the last one written wins.
extern "C"
{
    struct arch_context* kickos_armv8a_ctx_current = nullptr;
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

static_assert(KICKOS_KERNEL_STACKS != 0,
              "armv8a selects ARCH_KERNEL_STACKS_MANDATORY, so the blocks must exist");

// The incoming thread's kernel block top, published because a vector slot cannot reach the TCB.
// Read by the REPORTING slots alone (vectors.S), which cover exception classes this port never
// enters, so SP_EL1 carries no promise there.
//
// PER CORE, so the cell a slot reads is the one the core that took the exception wrote. At one
// core the accessor folds to the array's first element (percpu.h), which keeps the displacement
// vectors.S spells a link-time constant.
extern "C"
{
    struct armv8a_percpu kickos_armv8a_percpu[KICKOS_NUM_CORES] = {};
}
static_assert(offsetof(struct armv8a_percpu, kernel_sp) == 0,
              "vectors.S loads the kernel stack pointer at displacement 0 of the block");

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

    // How deep the reporter below is. A reporting slot keeps the SP_EL1 it arrives on and
    // branches straight to C, so a fault inside the report re-enters at slot 4 with nothing but
    // this count to tell it from the first arrival. Written under the mask the exception applies,
    // so no atomic is owed; nothing clears it, the reporter never returning.
    unsigned long g_report_depth = 0;

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

    // The slot a fault in privileged code arrives at.
    constexpr unsigned long SLOT_SP_EL1_SYNC = 4;

    // The EL0 synchronous slot, which reaches the reporter only for a fault the kill rule
    // declined (switch.S .Lel0_fault_panic): root before the scheduler runs, a privileged
    // thread, a thread already dying, or a frame the entry did not build.
    constexpr unsigned long SLOT_EL0_SYNC = 8;

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
    // ctx->kernel_sp IS READ, NOT WRITTEN, HERE: thread_create seats the block before this
    // call and owns the zero that means none is seated.
#if defined(KICKOS_TLS) && KICKOS_TLS
    // stack_lo owes no alignment past the ABI's 16.
    ctx->tls_base = ctx->stack_lo - ::kickos::tls_block_size();
#endif

    // WHERE THIS FRAME SITS IS THE PRIVILEGE BOUNDARY: it carries SPSR and ELR, so whoever can
    // write it chooses the exception level and the PC of the eret that starts the thread. An EL0
    // thread's stack is a TASK-WIDE mapping (docs/design-m6-mmu.md F9), so a sibling can write
    // both fields; the frame goes on this thread's own KERNEL block. A privileged thread resumes
    // at EL1 on this sp and would then run its whole life on a block sized for one dispatch, so
    // its frame stays on the stack it was handed.
    //
    // The eret pops the frame, leaving SP_EL1 at the block top, so the first trap an EL0 thread
    // takes already arrives on its own block: that is what lets ENTER_FROM_EL0 trust SP_EL1 and
    // spend no scratch register (switch.S).
    uintptr_t const user_top = (ctx->stack_hi) & ~static_cast<uintptr_t>(15);
    uintptr_t frame_top = user_top;
    if (privileged == 0)
    {
        // Never zero here: this arch selects ARCH_KERNEL_STACKS_MANDATORY, every unprivileged
        // thread holds a pool slot, and the one TCB outside the pool is the privileged idle.
        // thread_create asserts it.
        frame_top = ctx->kernel_sp;
    }
    uintptr_t const base = frame_top - ARMV8A_FRAME_SIZE;
    uint64_t* f = reinterpret_cast<uint64_t*>(base);
    for (size_t i = 0; i < ARMV8A_FRAME_SIZE / sizeof(uint64_t); i++)
    {
        f[i] = 0;
    }
    f[0] = reinterpret_cast<uint64_t>(arg);                         // x0
    f[ARMV8A_F_ELR / 8] = reinterpret_cast<uint64_t>(entry);
    // Interrupts LIVE in both arms, and this is the system's FIRST enable: nothing earlier
    // can perform it, arch_irq_restore clearing only what its own paired save set.
    if (privileged != 0)
    {
        f[ARMV8A_F_X30 / 8] = reinterpret_cast<uint64_t>(&kickos_armv8a_thread_exit);
        f[ARMV8A_F_SPSR / 8] = SPSR_EL1H_IRQ_ON;
    }
    else
    {
        f[ARMV8A_F_X30 / 8] = reinterpret_cast<uint64_t>(&kickos_user_thread_return);
        f[ARMV8A_F_SPSR / 8] = SPSR_EL0T_IRQ_ON;
        f[ARMV8A_F_SP_EL0 / 8] = user_top; // its own stack, whole: no frame stands on it
    }
    ctx->sp = base;
}

// Resumes the thread at `entry`, privileged, on its own KERNEL block: the death path must
// not run on memory an unprivileged thread can write.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp SURVIVES THE REBUILD, put back explicitly. The stub is privileged, so the
    // rebuild below places its frame from the block it is HANDED.
    uintptr_t const kernel_sp = ctx->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    uintptr_t const tls_base = ctx->tls_base;
#endif
#if KICKOS_KERNEL_STACKS
    // stack_lo and stack_hi are saved and put back: arch_context_init derives them from what it
    // is handed, and handing it the block would leave the context describing kernel .bss as this
    // thread's stack. check_death_stack_seating.sh holds this shape. The `if` covers a TCB
    // outside the pool, which has no block.
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
        // `from` is dropped here on purpose; see kickos_armv8a_ctx_current.
        kickos_armv8a_switch_to = to;
        armv8a_percpu()->kernel_sp = to->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
        armv8a_seat_thread_pointer(to);
#endif
        return;
    }
    armv8a_percpu()->kernel_sp = to->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    armv8a_seat_thread_pointer(to);
#endif
    // Thread context under the kernel IrqLock, so no interrupt observes the cell between
    // this write and the frame it describes being the one on the CPU.
    kickos_armv8a_ctx_current = to;
    kickos_armv8a_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot; // abandoned, as arch.h permits and the M-profile backend also does
    kickos_armv8a_ctx_current = first;
    armv8a_percpu()->kernel_sp = first->kernel_sp;
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
// NESTING-SAFE: the state is the one bit this touches and the restore clears only what its own
// save set. A wholesale `msr daif, saved` would write back D, A and F too, clobbering any change
// made between the two.
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
// core-fixed address, a GIC wherever the SoC put it, so ownership follows the addresses.

// --- Fault isolation --------------------------------------------------------
// SPSR_EL1 IS READ FROM THE REGISTER, so the privilege question is answered without believing a
// word of the frame. M[3:0] == 0 is EL0t, the only mode an unprivileged thread runs in here.
//
// THE FRAME TEST IS EXACT, carrying two claims at once: the redirect writes through the frame and
// RESTORE_FRAME_AND_ERET pops it, so where the frame ends is where the stub's SP_EL1 lands, and
// arch.h owes the stub the block TOP. Both hold only for a frame ENTER_FROM_EL0 pushed, SP_EL1
// sitting at the block top whenever EL0 is running. Anything else fails closed to the panic
// dump.
bool arch_fault_is_user_thread(void* frame)
{
    uint64_t spsr = 0;
    __asm volatile("mrs %0, spsr_el1" : "=r"(spsr));
    if ((spsr & 0xFULL) != 0)
    {
        return false;
    }
    uintptr_t const top = kickos_fault_stack_top();
    if (top < ARMV8A_FRAME_SIZE)
    {
        return false; // 0 for no current thread, for idle, and for no block seated
    }
    return reinterpret_cast<uintptr_t>(frame) == top - ARMV8A_FRAME_SIZE;
}

// ELR and SPSR are the two fields the eret consumes, so rewriting them IS the redirect; the pop
// seats the stub's SP at the block top by itself.
//
// TTBR0 IS LEFT ON THE FAULTING SPACE: the stub prints the dead thread's name, a string literal
// in APP text that only that space maps. The release path puts the dying space back
// (kernel/mem/aspace.cc).
void arch_fault_redirect_to_exit(void* frame)
{
    uint64_t esr = 0;
    uint64_t elr = 0;
    uint64_t far = 0;
    __asm volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm volatile("mrs %0, elr_el1" : "=r"(elr));
    __asm volatile("mrs %0, far_el1" : "=r"(far));
    int addr_valid = 0;
    if (far_is_valid(esr))
    {
        addr_valid = 1;
    }
    kickos_fault_record("ESR_EL1", esr, static_cast<uintptr_t>(elr),
                        static_cast<uintptr_t>(far), addr_valid);

    uint64_t* const f = static_cast<uint64_t*>(frame);
    f[ARMV8A_F_ELR / 8] = reinterpret_cast<uint64_t>(&kickos_thread_fault_exit);
    f[ARMV8A_F_SPSR / 8] = SPSR_EL1H_IRQ_ON;
}

// --- Exceptions -------------------------------------------------------------
// Every reporting slot lands here with its index and the interrupted x30 (vectors.S). Nothing
// resumes from here: a reporting slot reaches C by a plain branch and builds no frame to return
// through. A fault the EL0 synchronous entry cannot contain arrives as slot 8.
void kickos_armv8a_exception(unsigned long slot, unsigned long lr)
{
    // FIRST, ahead of the space swap and the console: both can fault, and a fault in either
    // arrives back here. The second arrival still emits; the third emits nothing.
    g_report_depth++;
    if (g_report_depth >= 3)
    {
        kfault_terminate();
    }
    if (g_report_depth == 2)
    {
        kpanic_enter(); // idempotent: the first arrival may have faulted short of it
        ::kickos::kprintf("\n=== ARMV8A EXCEPTION (taken inside the reporter) ===\n");
        kfault_terminate();
    }
    kickos_armv8a_ttbr0_to_boot(); // before the console is touched
    kpanic_enter();
    if (slot == SLOT_SP_EL1_SYNC or slot == SLOT_EL0_SYNC)
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
