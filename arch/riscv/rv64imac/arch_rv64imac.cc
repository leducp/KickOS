// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV64IMAC arch backend: the ISA-generic half of the arch.h seam. switch.S holds the
// trap vector, the save frame and the entries; trap.S the supervisor-mode confirmation.
//
// THIS PORT RUNS IN SUPERVISOR MODE, so every CSR here is an s-prefixed one and nothing is
// shared with the machine-mode rv32imac backend beside it.
//
// satp belongs to the map editor in aspace_rv64imac.cc: one root serves both privilege levels,
// so it moves only when the space does.
//
// THE INTERRUPT CONTROLLER IS PURE SOFTWARE, there being no PLIC and no external interrupt in
// this image. mask/unmask/clear_pending are a bitmask, and one raise reaches the ISR path
// through sip.SSIP, which S-mode may write itself.

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/rv64_doorbell.h>
#include <kickos/arch/rv64_frame.h>
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

namespace
{
    // sstatus bits, from the header switch.S reads: SPP is ONE bit at 8, where the
    // machine-mode MPP is two at 11.
    constexpr uint64_t SSTATUS_SIE = KICKOS_RV64_SSTATUS_SIE;
    constexpr uint64_t SSTATUS_SPIE = KICKOS_RV64_SSTATUS_SPIE;
    constexpr uint64_t SSTATUS_SPP = KICKOS_RV64_SSTATUS_SPP;

    // sstatus.UXL at 33:32 when SXLEN is 64: 1 is 32, 2 is 64, nothing else is legal. The field
    // is WARL, so a written 0 reads back as whatever the hart substitutes, possibly 32, which
    // takes every U-mode fetch and effective address modulo 2^32. .Lrestore writes sstatus WHOLE
    // from the frame, so a fabricated frame leaving this field clear is a write of zero.
    constexpr unsigned SSTATUS_UXL_SHIFT = 32;
    constexpr uint64_t SSTATUS_UXL_MASK = 0x3ull << SSTATUS_UXL_SHIFT;
    constexpr uint64_t SSTATUS_UXL_64 = 0x2ull << SSTATUS_UXL_SHIFT;

    // scause: the top bit at XLEN 64 splits interrupt from exception, and the rest is the
    // code.
    constexpr uint64_t SCAUSE_INTERRUPT = 1ull << 63;

    // Interrupt causes, which are also the sie/sip bit positions for the same source.
    constexpr uint64_t INT_SUPERVISOR_SOFTWARE = 1;
    constexpr uint64_t INT_SUPERVISOR_TIMER = 5;
    constexpr uint64_t SIE_SSIE = 1ull << INT_SUPERVISOR_SOFTWARE;
    constexpr uint64_t SIE_STIE = 1ull << INT_SUPERVISOR_TIMER;
    constexpr uint64_t SIP_SSIP = 1ull << INT_SUPERVISOR_SOFTWARE;

    // The software controller's line count. Nothing here indexes hardware, so the width is
    // the bitmask's and not a chip's interrupt-ID count.
    constexpr int IRQ_LINES = 32;

    char const* interrupt_name(uint64_t code)
    {
        switch (code)
        {
        case 1:  { return "unexpected supervisor software interrupt"; }
        case 5:  { return "unexpected supervisor timer interrupt";    }
        case 9:  { return "unexpected supervisor external interrupt"; }
        case 13: { return "unexpected counter-overflow interrupt";    }
        default: { return "unexpected interrupt";                     }
        }
    }

    char const* exception_name(uint64_t code)
    {
        switch (code)
        {
        case 0:  { return "instruction address misaligned"; }
        case 1:  { return "instruction access fault";       }
        case 2:  { return "illegal instruction";            }
        case 3:  { return "breakpoint";                     }
        case 4:  { return "load address misaligned";        }
        case 5:  { return "load access fault";              }
        case 6:  { return "store address misaligned";       }
        case 7:  { return "store access fault";             }
        case 8:  { return "ecall from user mode";           }
        case 9:  { return "ecall from supervisor mode";     }
        case 12: { return "instruction page fault";         }
        case 13: { return "load page fault";                }
        case 15: { return "store page fault";               }
        default: { return "unknown exception";              }
        }
    }

    // THE SOFTWARE CONTROLLER'S STATE, IMAGE-WIDE AND NOT PER HART. Nothing on this board
    // implements an interrupt controller, so these mirror no per-hart registers: a line is one
    // logical resource, and the kernel's IRQ layer masks it on whichever core services it and
    // unmasks it on whichever core its driver runs on. Every caller holds the kernel lock
    // (kernel/irq/irq.cc and the syscall entries), which is the exclusion that covers them.
    //
    // UNMASKED rather than masked, and the inject line BIASED BY ONE, so that zero is the
    // arch.h reset contract and neither needs an initialiser.
    uint32_t g_irq_unmasked = 0;
    uint32_t g_irq_pending = 0;
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_inject_line_1 = 0;

    // An interrupt cause the dispatch does not handle. sie enables the timer and the software
    // channel alone, so this is delivery of a source nothing enabled.
    [[noreturn]] void rv64_unexpected_interrupt(uint64_t scause)
    {
        kpanic_enter();
        uint64_t sepc = 0;
        __asm volatile("csrr %0, sepc" : "=r"(sepc));
        ::kickos::kprintf("\n=== RISC-V S-TRAP (%s) ===\n",
                          interrupt_name(scause & ~SCAUSE_INTERRUPT));
#if KICKOS_PANIC_DUMP
        ::kickos::kprintf(KDIAG_F_RV64_CAUSE, scause, sepc);
#else
        (void)sepc;
#endif
        kfault_terminate();
    }

}

// trap.S.
extern "C" int kickos_rv64_privilege_probe(void);

// switch.S.
extern "C" void kickos_rv64_stvec(void);
extern "C" void kickos_rv64_switch_now(struct arch_context* from, struct arch_context* to);
extern "C" void kickos_rv64_start(struct arch_context* first);

// An unprivileged thread returns through the user-side stub; the privileged one is a kernel
// symbol U-mode cannot call.
extern "C" void kickos_user_thread_return(void);

extern "C"
{
    // One row per hart: the trusted trap stack, and above it the block sscratch points at.
    // Zero-initialised throughout, which is why the mask mirror is stored unmasked.
    struct rv64_percpu_row kickos_rv64_percpu[KICKOS_NUM_CORES] = {};
}

static_assert(offsetof(struct rv64_percpu_block, ctx_current) == 0,
              "switch.S spells PERCPU_CTX_CURRENT as 0");
static_assert(offsetof(struct rv64_percpu_block, switch_to) == 8,
              "switch.S spells PERCPU_SWITCH_TO as 8");
static_assert(offsetof(struct rv64_percpu_block, isr_depth) == 16,
              "switch.S spells PERCPU_ISR_DEPTH as 16");
static_assert(offsetof(struct rv64_percpu_row, block) == KICKOS_RV64_TRAP_STACK_SIZE,
              "sscratch holds the trap-stack top, so the block must begin exactly there");
static_assert(sizeof(kickos_rv64_percpu[0].trap_stack) == KICKOS_RV64_TRAP_STACK_SIZE,
              "switch.S reaches the row's canary at the top minus this constant");
static_assert(sizeof(struct rv64_percpu_block) == KICKOS_RV64_PERCPU_BLOCK_SIZE,
              "the block must fill its line, and startup.S spells the row stride from this");
static_assert(sizeof(struct rv64_percpu_row) == KICKOS_RV64_PERCPU_ROW_SIZE,
              "startup.S indexes this array in machine mode with that stride");

static_assert(offsetof(struct arch_context, sp) == KICKOS_RV64_CTX_OFF_SP,
              "switch.S expects ctx.sp at CTX_SP");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_RV64_CTX_OFF_TRACE_TID,
              "the frame header disagrees with the struct on trace_tid");
#endif
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_RV64_CTX_OFF_STACK_LO,
              "the frame header disagrees with the struct on stack_lo");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_RV64_CTX_OFF_STACK_HI,
              "the frame header disagrees with the struct on stack_hi");
#if defined(KICKOS_TLS) && KICKOS_TLS
static_assert(offsetof(struct arch_context, tls_base) == KICKOS_RV64_CTX_OFF_TLS_BASE,
              "the frame header disagrees with the struct on tls_base");
#endif
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_RV64_CTX_OFF_KERNEL_SP,
              "the U-mode entry loads ctx.kernel_sp at CTX_KERNEL_SP");

// With no block seated every U-mode trap takes the refusal path, so this arch cannot be
// configured without the blocks. ARCH_KERNEL_STACKS_MANDATORY puts `range 1 1` on the knob;
// this fires if that select is ever dropped.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "rv64imac's trap entry builds every U-mode frame on ctx.kernel_sp");
static_assert(KICKOS_RV64_FRAME % KICKOS_RV64_SP_ALIGN == 0,
              "the frame size must preserve the psABI stack alignment");
static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_RV64_SP_ALIGN == 0,
              "a kernel block's top must land on the alignment the prologue requires");
static_assert(KICKOS_RV64_TRAP_STACK_SIZE % KICKOS_RV64_SP_ALIGN == 0,
              "the trap-stack top must land on the alignment the prologue requires");
// STRUCTURAL ONLY: a blocking syscall holds the ecall frame and the switch frame on the block
// at once, and the lowest word of a block is its overflow canary. The DISPATCH depth below them
// is unmeasured on this arch, so no figure here stands in for it (rv64_frame.h).
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint64_t) >= 2 * KICKOS_RV64_FRAME,
              "the kernel block cannot hold a blocking syscall's two frames plus its canary");

extern "C"
{

// --- Context / switching ----------------------------------------------------
// Builds the frame .Lrestore resumes (switch.S). Every resume being an sret, the entry needs
// no trampoline: sepc carries it and a0 its argument.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    ctx->stack_lo = reinterpret_cast<uintptr_t>(stack_base);
    uintptr_t const top = (ctx->stack_lo + stack_size)
                          & ~static_cast<uintptr_t>(KICKOS_RV64_SP_ALIGN - 1);
    ctx->stack_hi = top;
#if defined(KICKOS_TLS) && KICKOS_TLS
    ctx->tls_base = 0;
#endif
    // ctx->kernel_sp IS READ, NOT WRITTEN, HERE: thread_create seats the block before this
    // call and owns the zero that means none is seated.

    // WHERE THIS FRAME SITS IS THE PRIVILEGE BOUNDARY: it carries sstatus and sepc, so whoever
    // can write it chooses the level and the PC of the sret that starts the thread, and a
    // thread's own stack is writable by its task. An unprivileged thread's first frame goes on
    // its KERNEL block. A privileged thread resumes at S-mode on this sp and would then run its
    // whole life on a block sized for one dispatch, so its frame stays on the stack handed in.
    uintptr_t frame_top = top;
    if (privileged == 0)
    {
        // Never zero here: this arch selects ARCH_KERNEL_STACKS_MANDATORY, every unprivileged
        // thread holds a pool slot, and the one TCB outside the pool is the privileged idle.
        // thread_create asserts it.
        frame_top = ctx->kernel_sp;
    }
    uintptr_t const base = frame_top - KICKOS_RV64_FRAME;
    uint64_t* const f = reinterpret_cast<uint64_t*>(base);
    for (size_t i = 0; i < KICKOS_RV64_FRAME / sizeof(uint64_t); i++)
    {
        f[i] = 0;
    }

    // SPIE: the sret sets SIE from it, and this is the system's FIRST enable. SIE stays 0 in
    // the word because .Lrestore writes sstatus while still inside the epilogue.
    //
    // UXL carries the RV64 encoding: this word reaches the CSR whole, and a clear field is a
    // reserved value the hart may answer with UXLEN 32.
    uint64_t sstatus = SSTATUS_SPIE | SSTATUS_UXL_64;
    uintptr_t ret = reinterpret_cast<uintptr_t>(&kickos_thread_return);
    if (privileged != 0)
    {
        sstatus |= SSTATUS_SPP;
    }
    else
    {
        ret = reinterpret_cast<uintptr_t>(&kickos_user_thread_return);
    }

    f[KICKOS_RV64_F_SEPC / 8] = reinterpret_cast<uint64_t>(entry);
    f[KICKOS_RV64_F_SSTATUS / 8] = sstatus;
    f[KICKOS_RV64_F_RA / 8] = ret;              // entry() returns here
    f[KICKOS_RV64_F_A0 / 8] = reinterpret_cast<uint64_t>(arg);
    // The sp .Lrestore leaves on: this thread's OWN stack, whole, no frame standing on it.
    // Nothing else seats it in a fabricated frame.
    f[KICKOS_RV64_F_SP / 8] = top;
    ctx->sp = base;
}

void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp SURVIVES THE REBUILD, put back explicitly rather than assumed untouched.
    uintptr_t const kernel_sp = ctx->kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    uintptr_t const tls_base = ctx->tls_base;
#endif
#if KICKOS_KERNEL_STACKS
    // stack_lo and stack_hi are saved and put back: arch_context_init derives them from what it
    // is handed, and handing it the block would leave the context describing kernel .bss as this
    // thread's stack. The `if` covers a TCB outside the pool, which has no block.
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

// SYNCHRONOUS in thread context and DEFERRED from an ISR, which arch.h permits.
//
// The deferred arm rests on an invariant the entry maintains: every interrupt frame is a
// resumable thread context standing on a stack that outlives the trap. A U-mode interrupt puts
// it on the thread's own kernel block, an S-mode one on the interrupted thread's own stack, and
// syscall dispatch runs with SIE masked so no interrupt lands on the trap stack.
//
// THE THREAD-CONTEXT ARM REQUIRES THE CALLER TO HAVE INTERRUPTS MASKED. The publish below and
// the register save inside kickos_rv64_switch_now are two steps, so an interrupt between them
// reaches .Lintr with ctx_current already naming `to`, and the booked swap would store a pointer
// into `from`'s stack as `to`'s saved context.
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (rv64_percpu()->isr_depth != 0)
    {
        // `from` is dropped: switch.S reads the outgoing context from the block's
        // ctx_current, which is the one the interrupt frame belongs to.
        rv64_percpu()->switch_to = to;
        return;
    }
    rv64_percpu()->ctx_current = to;
    kickos_rv64_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot; // abandoned, as arch.h permits
    rv64_percpu()->ctx_current = first;
    kickos_rv64_start(first);

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
arch_irq_state_t arch_irq_save(void)
{
    uint64_t old = 0;
    __asm volatile("csrrci %0, sstatus, 2" : "=r"(old)::"memory");
    return static_cast<arch_irq_state_t>(old & SSTATUS_SIE);
}

void arch_irq_restore(arch_irq_state_t state)
{
    // csrs only SETS bits, and state is 0 or SSTATUS_SIE, so this re-enables SIE exactly
    // when the paired save disabled it. That is what makes it nesting-safe.
    __asm volatile("csrs sstatus, %0" ::"r"(static_cast<uint64_t>(state)) : "memory");
}

// The interrupt leg of the entry alone bumps it, so it reads FALSE inside syscall dispatch as
// arch.h requires: the kernel's blocking primitives depend on that.
int arch_in_isr(void)
{
    return rv64_percpu()->isr_depth != 0;
}

// --- Clocks -----------------------------------------------------------------
uint32_t arch_cpu_clock_hz(void)
{
    return 0;
}

// --- Region descriptors: none on this arch ----------------------------------
// arch_mpu_min_region returning 0 makes arch_ram_region_size 16-byte granular, so
// arch_mpu_region_pow2 is never read. Both bodies are scraped textually by
// cmake/boot_arena.cmake and must stay a plain integer return.
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

// Rule 7 (arch.h): RISC-V has no bit-band alias.
int arch_bitband_present(void)
{
    return 0;
}

// --- Interrupt controller ---------------------------------------------------
// No hardware line exists on this board, so mask/unmask/clear_pending are the bitmask above and
// a raise reaches the ISR path through ONE doorbell, sip.SSIP, with the block's inject line
// telling the dispatch which logical line it was. Each body is self-bracketed as arch.h requires.
//
// SINGLE-DOORBELL: at most one unmask carrying a latched raise per interrupts-masked region, a
// second overwriting the first's identity. irq_claim/wait/ack unmask one line per lock
// section.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_unmasked &= ~(1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_unmasked |= (1u << line);
    // A raise taken while the line was masked redelivers now through the doorbell: sip.SSIP is
    // set with SIE clear, so it fires at arch_irq_restore on the normal ISR path.
    if ((g_irq_pending & (1u << line)) != 0)
    {
        g_irq_pending &= ~(1u << line);
        g_inject_line_1 = static_cast<uint32_t>(line) + 1u;
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }
    arch_irq_restore(s);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_pending &= ~(1u << line);
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return;
    }
    // An ISR reaching arch_irq_mask/unmask touches the same words.
    arch_irq_state_t s = arch_irq_save();
    if ((g_irq_unmasked & (1u << irq)) == 0)
    {
        g_irq_pending |= (1u << irq);
    }
    else
    {
        g_inject_line_1 = static_cast<uint32_t>(irq) + 1u; // BEFORE the raise
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }
    arch_irq_restore(s);
}

// Whether a device line is still latched in the controller above. The doorbell poll reads it
// to know whether the raise it absorbed carried something it did not service.
int kickos_rv64_inject_owed(void)
{
    return g_inject_line_1.load() != 0u;
}

// The interrupt leg of the entry (switch.S .Lintr), ISR context with SIE clear. scause is
// architectural, so the demux is the arch's. `frame` is kept for a cause that has no handler.
//
// THE TIMER'S STIP MUST NOT BE CLEARED HERE: Sstc drives it from `time >= stimecmp`, so
// kickos_isr_timer's own re-arm or disarm is what lowers it, and a write to sip would be a
// second writer of a bit that is read-only there.
void kickos_rv64_isr_dispatch(void* frame)
{
    (void)frame;
    uint64_t scause = 0;
    __asm volatile("csrr %0, scause" : "=r"(scause));
    uint64_t const code = scause & ~SCAUSE_INTERRUPT;
    if (code == INT_SUPERVISOR_TIMER)
    {
        kickos_isr_timer();
        return;
    }
    if (code == INT_SUPERVISOR_SOFTWARE)
    {
        // ONE CAUSE, THREE SOURCES, AND THE ORDER IS THE CONTRACT. sip.SSIP carries a peer's
        // cross-hart doorbell, the reschedule that doorbell may stand for, and this hart's own
        // device-line injection. Each arm is gated on its OWN state, so a raise carrying two of
        // them loses neither, and getting the order wrong drops a device raise or a rendezvous
        // and shows up as a hang under load rather than as a red gate.

        // FIRST, AND BEFORE ANY SERVICE: a raise landing during the work below stays pending
        // and is delivered again, rather than being cleared away underneath.
        __asm volatile("csrc sip, %0" ::"r"(SIP_SSIP) : "memory");

#if KICKOS_NUM_CORES > 1
        // SECOND, the doorbell's far side, which takes no kernel lock: an initiator may be
        // holding it while it waits here. THE CELL IS THE AUTHORITY, NOT THE RAISE.
        if (kickos_rv64_doorbell_pending() != 0)
        {
            kickos_rv64_doorbell_service();
        }
#endif
#if KICKOS_KERNEL_CORES > 1
        // THIRD, and OUTSIDE the service body because it takes the kernel lock. The take is
        // what tells a reschedule from a rendezvous whose target owes no scheduler entry.
        if (kickos_kernel_core_resched_take() != 0)
        {
            kickos_kernel_core_resched();
        }
#endif
        // FOURTH, this hart's own device line, which shares the cause with everything above.
        uint32_t const line_1 = g_inject_line_1;
        g_inject_line_1 = 0;
        if (line_1 != 0)
        {
            // kickos_isr_irq masks the line and wakes its driver (kernel/irq/irq.cc); the
            // driver re-unmasks via irq_ack.
            kickos_isr_irq(static_cast<int>(line_1 - 1u));
        }
        return;
    }
    rv64_unexpected_interrupt(scause);
}

// --- Fault isolation --------------------------------------------------------
// sstatus.SPP IS READ FROM THE REGISTER, so the privilege question is answered without
// believing a word of the frame. It is not the thread's identity: .Lecall runs the syscall
// dispatch in S-mode on the thread's kernel block, so a fault there is a kernel bug.
//
// The second test asks whether the frame is the one the U-mode entry built on that block. Every
// other frame in the image fails closed to the panic dump.
bool arch_fault_is_user_thread(void* frame)
{
    uint64_t sstatus = 0;
    __asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    if ((sstatus & SSTATUS_SPP) != 0)
    {
        return false;
    }
    return kickos_fault_frame_on_kernel_stack(frame, KICKOS_RV64_FRAME);
}

// Three fields of the frame, all consumed by .Lrestore: sepc and sstatus are the return address
// and the level, and F_SP is the sp it leaves on. Without the third the stub would run
// privileged on the sp the faulting thread chose.
void arch_fault_redirect_to_exit(void* frame)
{
    uint64_t scause = 0;
    uint64_t sepc = 0;
    uint64_t stval = 0;
    __asm volatile("csrr %0, scause" : "=r"(scause));
    __asm volatile("csrr %0, sepc" : "=r"(sepc));
    __asm volatile("csrr %0, stval" : "=r"(stval));
    // stval is an address for the access, misaligned and page-fault causes; for an illegal
    // instruction it holds the instruction bits instead.
    int addr_valid = 0;
    if (scause == 1 or scause == 4 or scause == 5 or scause == 6 or scause == 7
        or scause == 12 or scause == 13 or scause == 15)
    {
        addr_valid = 1;
    }
    kickos_fault_record("scause", scause, static_cast<uintptr_t>(sepc),
                        static_cast<uintptr_t>(stval), addr_valid);

    uint64_t* const f = static_cast<uint64_t*>(frame);
    f[KICKOS_RV64_F_SEPC / 8] = reinterpret_cast<uint64_t>(&kickos_thread_fault_exit);
    f[KICKOS_RV64_F_SSTATUS / 8] = SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_UXL_64;
    f[KICKOS_RV64_F_SP / 8] = kickos_fault_stack_top();
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Unhandled supervisor trap (switch.S .Lfault) ---------------------------
// A TRUE return means .Lfault must sret off the frame instead of dumping: fault isolation
// claimed the fault and arch_fault_redirect_to_exit above has re-pointed the frame at
// kickos_thread_fault_exit.
//
// Every CSR is read ONCE at the top, before anything below can take a trap of its own and
// overwrite them. sstatus comes out of the FRAME, that being the value .Lrestore will
// consume.
bool kickos_rv64_fault_report(void* frame)
{
    uint64_t scause = 0;
    uint64_t sepc = 0;
    uint64_t stval = 0;
    __asm volatile("csrr %0, scause" : "=r"(scause));
    __asm volatile("csrr %0, sepc" : "=r"(sepc));
    __asm volatile("csrr %0, stval" : "=r"(stval));
    uint64_t const* const f = static_cast<uint64_t const*>(frame);
    uint64_t const sstatus = f[KICKOS_RV64_F_SSTATUS / 8];

    // Nothing may print above this: kpanic_enter's console reclaim is permanent and this
    // fault is meant to be survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return true;
    }

    kpanic_enter();

    // Only an EXCEPTION reaches here: switch.S sends every interrupt cause to .Lintr.
    char const* const what = exception_name(scause & ~SCAUSE_INTERRUPT);
    ::kickos::kprintf("\n=== RISC-V S-TRAP (%s) ===\n", what);
#if KICKOS_PANIC_DUMP
    char const* from = "user";
    if ((sstatus & SSTATUS_SPP) != 0)
    {
        from = "supervisor";
    }
    ::kickos::kprintf(KDIAG_F_RV64_CAUSE, scause, sepc);
    ::kickos::kprintf(KDIAG_F_RV64_STATUS, stval, sstatus);
    ::kickos::kprintf(KDIAG_F_RV64_FRAME, f[KICKOS_RV64_F_SP / 8], f[KICKOS_RV64_F_RA / 8]);
    ::kickos::kprintf(KDIAG_F_RV64_FROM, from);
#else
    (void)sepc;
    (void)stval;
    (void)sstatus;
#endif
    kfault_terminate();
}

// A U-mode trap from a thread whose ctx.kernel_sp is 0 (switch.S .Ltrap_nokstack): a
// provisioning bug, and containment has no block to rebuild the slain thread onto.
[[noreturn]] void kickos_rv64_no_kernel_stack(void)
{
    kpanic_enter();
    ::kickos::kprintf("\n=== RISC-V S-TRAP (no kernel block seated) ===\n");
    kfault_terminate();
}

#if KICKOS_NUM_CORES > 1
// --- Core identity ----------------------------------------------------------
// sscratch, which the machine-mode prologue seats on every hart before its mret and which
// U-mode can neither read nor write. Outside the trap entry's two-instruction prologue it
// holds this hart's block address.
struct rv64_percpu_block* rv64_percpu(void)
{
    uintptr_t blk = 0;
    __asm volatile("csrr %0, sscratch" : "=r"(blk));
    return reinterpret_cast<struct rv64_percpu_block*>(blk);
}

// mhartid IS NOT THE INDEX. It is integrator-chosen (the openc906 core hardwires it to zero
// and its integrator customises it per instance), so the dense index the kernel's per-core
// arrays are keyed by is derived HERE, from the row sscratch names, and a pointer naming no
// row is refused rather than indexed with.
struct rv64_percpu_block* rv64_percpu_seat(void)
{
    struct rv64_percpu_block* const blk = rv64_percpu();
    uintptr_t const base = reinterpret_cast<uintptr_t>(&kickos_rv64_percpu[0].block);
    uintptr_t const offset = reinterpret_cast<uintptr_t>(blk) - base;
    size_t const stride = sizeof(struct rv64_percpu_row);
    size_t const id = offset / stride;
    if (offset % stride != 0 or id >= KICKOS_NUM_CORES)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (sscratch names no per-hart row) ===\n");
        kfault_terminate();
    }
    blk->id = static_cast<uint32_t>(id);
    return blk;
}

uint32_t arch_cpu_id(void)
{
    return rv64_percpu()->id;
}
#endif

// --- One-time core bring-up ------------------------------------------------
void kickos_rv64_init(void)
{
    // DIRECT mode (low 2 bits = 00): one entry point for every cause.
    uintptr_t const tv = reinterpret_cast<uintptr_t>(&kickos_rv64_stvec);
    __asm volatile("csrw stvec, %0" ::"r"(tv) : "memory");

    // THE IDENTITY IS SEATED FIRST: everything below indexes per-hart state with it.
    struct rv64_percpu_block* const blk = rv64_percpu_seat();

    // The entry swaps sp with sscratch, so sscratch must hold the trusted top before the first
    // trap, and thus before the first sret to U-mode. The block's own address IS that top.
    __asm volatile("csrw sscratch, %0" ::"r"(blk) : "memory");

    // The row's low doubleword, read by switch.S's .Ltrap_reentry to tell a fault inside the
    // reporter from a descent that ran off the row: a store past the bottom lands in ordinary
    // .bss and takes no trap of its own.
    *reinterpret_cast<uint64_t*>(kickos_rv64_percpu[arch_cpu_id()].trap_stack) =
        KICKOS_RV64_TRAP_CANARY;

    // The chip's startup already installed the root, so it is read back: a zero here means the
    // boot table never took and every address below is a physical one.
    uint64_t boot_satp = 0;
    __asm volatile("csrr %0, satp" : "=r"(boot_satp));
    if (boot_satp == 0)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (no translation root) ===\n");
        kfault_terminate();
    }

    // SUM STAYS CLEAR: S-mode cannot load or store a page carrying U at all. The kernel reaches
    // memory a process owns ONLY through the kaccess seam (kickos/aspace.h), whose acquire hands
    // back a kernel-half pointer to the frame the space's tables name. A kernel dereference of a
    // low-half pointer FAULTS.

    // The drop startup.S performs is confirmed here and cannot be confirmed earlier: current
    // privilege is not readable on RISC-V, so the probe's refused read needs a vector to land
    // in.
    if (kickos_rv64_privilege_probe() == 0)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (hart is not in supervisor mode) ===\n");
        kfault_terminate();
    }

    // THE WIDTH U-MODE RUNS AT, seated and read back before the first sret to it, and AFTER the
    // probe above whose trap leg rewrites sstatus. UXL is WARL and may be read-only; a hart
    // keeping the RV32 encoding takes every U-mode fetch and effective address modulo 2^32.
    // Written whole: an intermediate 0 or 3 in the field is a reserved value.
    uint64_t uxl_seated = 0;
    __asm volatile("csrr %0, sstatus" : "=r"(uxl_seated));
    uxl_seated = (uxl_seated & ~SSTATUS_UXL_MASK) | SSTATUS_UXL_64;
    __asm volatile("csrw sstatus, %0" ::"r"(uxl_seated) : "memory");
    __asm volatile("csrr %0, sstatus" : "=r"(uxl_seated));
    if ((uxl_seated & SSTATUS_UXL_MASK) != SSTATUS_UXL_64)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (sstatus.UXL is not RV64) ===\n");
        kfault_terminate();
    }

    // STIE (the tickless deadline) and SSIE (the injected-IRQ doorbell), AFTER the probe: its
    // trap leg clears sstatus.SIE and never srets it back, so it has to run before any source
    // can fire. sstatus.SIE is still 0 here; the first sret to a thread enables delivery.
    uint64_t const sie = SIE_STIE | SIE_SSIE;
    __asm volatile("csrw sie, %0" ::"r"(sie) : "memory");
}

}
