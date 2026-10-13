// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv8-A (AArch64) arch backend: the ISA-generic half of the arch.h seam. The chip layer
// (arch/arm64/chip/virt_arm64) supplies the hardware edges, switch.S the frame and entries.

#include <kickos/arch/arch.h>
#include "ctx_redirect.h"
#include <kickos/arch/armv8a_trap_stack.h>
#include <kickos/arch/idle_floor.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/core_stack.ld.h>
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>

#include "../common/gic.h"

#include <stddef.h>
#include <stdint.h>

// The arch layer carries no kernel headers, so the fault path's three are declared here.
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

#if KICKOS_NUM_CORES > 1
// Defined by the chip layer: this core's interrupt-controller interface and its timer.
extern "C" void kickos_armv8a_percore_init(void);
#endif

// aspace_armv8a.cc. Terminal paths only: it discards the running space, and the contained
// fault path prints the dead thread's name out of app text.
extern "C" void kickos_armv8a_ttbr0_to_boot(void);

// switch.S.
extern "C" void kickos_armv8a_switch_now(struct arch_context* from, struct arch_context* to);
extern "C" void kickos_armv8a_start(struct arch_context* first);
extern "C" void kickos_armv8a_thread_exit(void);

// switch.S spells these as literal displacements, so a change on one side alone is a silent
// wrong offset.
constexpr size_t ARMV8A_FRAME_SIZE = KICKOS_ARMV8A_TRAP_FRAME;
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

// The spawn stages its grant list on the kernel block, and _SYSK holds the list it was measured
// at (armv8a_trap_stack.h).
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 13,
              "KICKOS_ARMV8A_TRAP_DEPTH_SYSK was measured at a spawn staging 13 grants on the "
              "kernel block: re-measure with tests/static/check_trap_redzone.sh first");

// The gate reads KICKOS_ARMV8A_TRAP_NEST as an immediate, so the sum is spelled out there.
static_assert(KICKOS_ARMV8A_TRAP_NEST == KICKOS_ARMV8A_TRAP_FRAME + KICKOS_ARMV8A_TRAP_DEPTH_IRQ,
              "KICKOS_ARMV8A_TRAP_NEST is an interrupt's frame plus its dispatch");
static_assert(KICKOS_ARMV8A_TRAP_WINDOW == KICKOS_ARMV8A_TRAP_FRAME + KICKOS_ARMV8A_TRAP_NEST,
              "KICKOS_ARMV8A_TRAP_WINDOW is the EL0 entry's frame plus an interrupt's extent");
// The lowest word of a block is the overflow canary (kernel/thread/thread.cc).
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV8A_TRAP_WINDOW + KICKOS_ARMV8A_TRAP_DEPTH_SYSWIN,
              "the kernel block cannot hold an interrupt taken in a system call's window");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV8A_TRAP_FRAME + KICKOS_ARMV8A_TRAP_DEPTH_SYSK,
              "the kernel block cannot hold the EL0 synchronous entry plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV8A_TRAP_NEST + KICKOS_ARMV8A_TRAP_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_ARMV8A_TRAP_DEPTH_EXITKSW,
              "the kernel block cannot hold the relocated death path's switch");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_ARMV8A_PANIC_FRAME + KICKOS_ARMV8A_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");
// The link checks the same floor above idle's thread-local carve.
static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_ARCH_IDLE_FLOOR,
              "an EL1h interrupt does not fit this board's idle stack");

// The per-core block (percpu.h): the incoming thread's kernel block top is published in it
// because a vector slot cannot reach the TCB, and only the reporting slots (vectors.S) read it.
extern "C"
{
    struct armv8a_percpu_block kickos_armv8a_percpu[KICKOS_NUM_CORES] = {};
}
static_assert(offsetof(struct armv8a_percpu_block, kernel_sp) == 0,
              "vectors.S loads the kernel stack pointer at displacement 0 of the block");
static_assert(offsetof(struct armv8a_percpu_block, ctx_current) == 8,
              "switch.S spells PERCPU_CTX_CURRENT as 8");
static_assert(offsetof(struct armv8a_percpu_block, switch_to) == 16,
              "switch.S spells PERCPU_SWITCH_TO as 16");
#if KICKOS_BENCH
static_assert(offsetof(struct armv8a_percpu_block, bench_sw_start) == 28,
              "switch.S spells PERCPU_BENCH_SW_START as 28");
#endif
static_assert(sizeof(struct armv8a_percpu_block) == 64,
              "the block must fill its line, and secondary.S spells PERCPU_SIZE as 64");

#if KICKOS_NUM_CORES > 1
extern "C"
{
    void kickos_armv8a_doorbell_park(void);
}

// --- Secondary bring-up -----------------------------------------------------
// The translation a core comes up under, read from the registers the chip programmed. A
// secondary reads this record with its MMU off (secondary.S); the displacements are spelled
// as literals on both sides.
extern "C"
{
    struct armv8a_boot_regs
    {
        uint64_t mair;
        uint64_t tcr;
        uint64_t ttbr0;
        uint64_t ttbr1;
        uint64_t sctlr;
        uint64_t cpacr;
    };
    struct armv8a_boot_regs kickos_armv8a_boot_regs = {};

    // Each core's boot and exception stack TOP, indexed by arch_cpu_id and read after
    // translation is on. The primary's slot is unused: its stack is the linker's _estack.
    uintptr_t kickos_armv8a_boot_sp[KICKOS_NUM_CORES] = {};

    // A core's arrival witness, set once it runs the kernel's translation on its own stack.
    // Arrival, not readiness: its interrupt-controller interface comes up after.
    kickos::Atomic<uint8_t, kickos::Order::RELAXED> kickos_armv8a_core_online[KICKOS_NUM_CORES] =
        {};
}
static_assert(offsetof(struct armv8a_boot_regs, mair) == 0, "secondary.S spells BOOT_MAIR");
static_assert(offsetof(struct armv8a_boot_regs, tcr) == 8, "secondary.S spells BOOT_TCR");
static_assert(offsetof(struct armv8a_boot_regs, ttbr0) == 16, "secondary.S spells BOOT_TTBR0");
static_assert(offsetof(struct armv8a_boot_regs, ttbr1) == 24, "secondary.S spells BOOT_TTBR1");
static_assert(offsetof(struct armv8a_boot_regs, sctlr) == 32, "secondary.S spells BOOT_SCTLR");
static_assert(offsetof(struct armv8a_boot_regs, cpacr) == 40, "secondary.S spells BOOT_CPACR");

namespace
{
    constexpr size_t ARMV8A_CORE_STACK = KICKOS_ARMV8A_CORE_STACK;
    alignas(16) uint8_t g_core_stack[KICKOS_NUM_CORES][ARMV8A_CORE_STACK];

    // Runs after .bss is zeroed and before arch_init, while TTBR0_EL1 still holds the boot
    // root a secondary must come up on rather than some process's.
    __attribute__((constructor)) void armv8a_boot_publish()
    {
        uint32_t const id = arch_cpu_id();
        __asm volatile("msr tpidr_el1, %0" ::"r"(&kickos_armv8a_percpu[id]) : "memory");

        uint64_t v = 0;
        __asm volatile("mrs %0, mair_el1" : "=r"(v));
        kickos_armv8a_boot_regs.mair = v;
        __asm volatile("mrs %0, tcr_el1" : "=r"(v));
        kickos_armv8a_boot_regs.tcr = v;
        __asm volatile("mrs %0, ttbr0_el1" : "=r"(v));
        kickos_armv8a_boot_regs.ttbr0 = v;
        __asm volatile("mrs %0, ttbr1_el1" : "=r"(v));
        kickos_armv8a_boot_regs.ttbr1 = v;
        __asm volatile("mrs %0, sctlr_el1" : "=r"(v));
        kickos_armv8a_boot_regs.sctlr = v;
        __asm volatile("mrs %0, cpacr_el1" : "=r"(v));
        kickos_armv8a_boot_regs.cpacr = v;

        for (uint32_t i = 0; i < KICKOS_NUM_CORES; i++)
        {
            kickos_armv8a_boot_sp[i] =
                reinterpret_cast<uintptr_t>(&g_core_stack[i][ARMV8A_CORE_STACK]);
        }

        // A released core reads the record with its MMU off, which is Device-nGnRnE and so
        // non-cacheable whatever SCTLR_EL1 says: a line still dirty here would not reach it.
        arch_dcache_flush(&kickos_armv8a_boot_regs, sizeof(kickos_armv8a_boot_regs));
        kickos_armv8a_core_online[id] = 1;
    }
}
#endif

#if KICKOS_TLS
namespace kickos
{
    size_t tls_block_size();
}

// Safe ahead of the physical swap: no privileged code reads this register
// (check_no_privileged_tls.sh).
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
    // SPSR for a thread: debug masked, interrupts and SError live.
    constexpr uint64_t SPSR_EL1H_IRQ_ON = (1ULL << 9) | 0x5ULL;
    constexpr uint64_t SPSR_EL0T_IRQ_ON = (1ULL << 9);

    // Indexed by the slot vectors.S passes.
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

#if KICKOS_NUM_CORES > 1
// --- Core identity ----------------------------------------------------------
// MPIDR_EL1.Aff0 alone: one cluster of A53s, where Aff0 is dense from 0. It stops being the
// core index with more than one cluster (it restarts inside each) and with MPIDR_EL1.MT set
// (Aff0 names a hardware thread); such a part owes a wider decode here. Read from hardware: the
// secondary entry needs an identity before its per-core block is seated (secondary.S).
uint32_t arch_cpu_id(void)
{
    uint64_t mpidr = 0;
    __asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t const id = static_cast<uint32_t>(mpidr) & 0xFFu;
    if (id >= KICKOS_NUM_CORES)
    {
        // Per-core arrays are indexed with this.
        while (true)
        {
            __asm volatile("wfi");
        }
    }
    return id;
}

// TPIDR_EL1 is seated by the secondary entry stub before translation, and on the primary by
// the constructor above.
struct armv8a_percpu_block* armv8a_percpu(void)
{
    uint64_t block = 0;
    __asm volatile("mrs %0, tpidr_el1" : "=r"(block));
    return reinterpret_cast<struct armv8a_percpu_block*>(static_cast<uintptr_t>(block));
}

// Publishes core `id`'s arrival and parks it waiting for a doorbell. `id` is the caller's own
// index; 0 is refused.
//
// The interface comes up before the arrival is published: the bring-up discovers this core's
// GICD_SGIR target bit, and a doorbell can name no core whose bit is unpublished.
void kickos_armv8a_park_core(uint32_t id)
{
    if (id != 0 and id < KICKOS_NUM_CORES)
    {
        kickos_armv8a_percore_init();
        kickos_armv8a_core_online[id] = 1;
        __asm volatile("dsb ish" ::: "memory");
        kickos_armv8a_doorbell_park();
    }
    while (true)
    {
        __asm volatile("wfi");
    }
}
#endif

// --- Context / switching ----------------------------------------------------
// Builds the frame switch.S resumes: ELR carries the entry and x0 its argument.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    ctx->stack_lo = reinterpret_cast<uintptr_t>(stack_base);
    ctx->stack_hi = ctx->stack_lo + stack_size;
    // ctx->kernel_sp is read here, not written: thread_create seats it before this call.
#if KICKOS_TLS
    // stack_lo owes no alignment past the ABI's 16.
    ctx->tls_base = ctx->stack_lo - ::kickos::tls_block_size();
#endif

    // Where this frame sits is the privilege boundary: it carries SPSR and ELR, so whoever can
    // write it chooses the exception level and PC of the first eret. An EL0 thread's stack is
    // a task-wide mapping a sibling can write, so its frame goes on its own KERNEL block. A
    // privileged thread resumes at EL1 on this sp and would run its life on a block sized for
    // one dispatch, so its frame stays on the stack it was handed.
    //
    // The eret pops the frame, leaving SP_EL1 at the block top, which is what lets
    // ENTER_FROM_EL0 trust SP_EL1 (switch.S).
    uintptr_t const user_top = (ctx->stack_hi) & ~static_cast<uintptr_t>(15);
    uintptr_t frame_top = user_top;
    if (privileged == 0)
    {
        // Never zero: every unprivileged thread holds a pool slot (thread_create asserts it).
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
    // Interrupts are live in both arms: this is the system's first enable, which
    // arch_irq_restore cannot perform.
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
#if KICKOS_TLS
    // The rebuild derives it from the block; the thread_locals live where they were carved.
    uintptr_t const tls_base = ctx->tls_base;
#endif
    arch_ctx_redirect_to_block(ctx, entry, stack_base, stack_size);
#if KICKOS_TLS
    ctx->tls_base = tls_base;
#endif
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (armv8a_percpu()->isr_depth != 0)
    {
        // Deferred to the exception exit. `from` is dropped: see ctx_current in percpu.h.
        armv8a_percpu()->switch_to = to;
        armv8a_percpu()->kernel_sp = to->kernel_sp;
#if KICKOS_TLS
        armv8a_seat_thread_pointer(to);
#endif
        return;
    }
    armv8a_percpu()->kernel_sp = to->kernel_sp;
#if KICKOS_TLS
    armv8a_seat_thread_pointer(to);
#endif
    // Under the kernel IrqLock, so no interrupt observes the cell before the frame it names
    // is on the CPU.
    armv8a_percpu()->ctx_current = to;
    kickos_armv8a_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot; // abandoned
    armv8a_percpu()->ctx_current = first;
    armv8a_percpu()->kernel_sp = first->kernel_sp;
#if KICKOS_TLS
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

// Only the IRQ dispatcher bumps it, so it reads false inside syscall dispatch as arch.h
// requires.
int arch_in_isr(void)
{
    return armv8a_percpu()->isr_depth != 0;
}

// --- Clocks -----------------------------------------------------------------
uint64_t arch_cpu_clock_hz(void)
{
    return 0;
}

int arch_bitband_present(void)
{
    return 0;
}

// The mask/unmask/clear/inject quartet is the chip's: a GIC sits wherever the SoC put it.

// --- Fault isolation --------------------------------------------------------
// SPSR_EL1 is read from the register, not the frame. M[3:0] == 0 is EL0t, the only mode an
// unprivileged thread runs in.
//
// The frame test is exact: where the frame ends is where the stub's SP_EL1 lands, and arch.h
// owes the stub the block TOP. That holds only for a frame ENTER_FROM_EL0 pushed; anything else
// fails closed to the panic dump.
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

// Rewriting ELR and SPSR is the redirect; the pop seats the stub's SP at the block top.
//
// TTBR0 is left on the faulting space: the stub prints the dead thread's name, a literal in app
// text that only that space maps. The release path puts the dying space back
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
// Every reporting slot lands here with its index and the interrupted x30 (vectors.S); nothing
// resumes from here. A fault the EL0 synchronous entry cannot contain arrives as slot 8.
void kickos_armv8a_exception(unsigned long slot, unsigned long lr)
{
    // First, ahead of the space swap and the console, which can fault back into here.
    kickos_fault_report_begin();
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
#else
    (void)slot;
    (void)lr;
#endif
    kfault_terminate();
}

// The IRQ vector's C half (vectors.S); it keeps the depth behind arch_in_isr.
void kickos_armv8a_irq(void)
{
    uint32_t& depth = armv8a_percpu()->isr_depth;
    depth++;
    kickos_armv8a_gic_dispatch();
    depth--;
}

// The ISB takes an interrupt already signalled to the PE before the re-mask; one still on its
// way from the controller may wait for a later window.
void arch_irq_window(void)
{
    __asm volatile("msr daifclr, #2\n\tisb\n\tmsr daifset, #2" ::: "memory");
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

}
