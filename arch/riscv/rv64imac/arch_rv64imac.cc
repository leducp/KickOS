// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RV64IMAC arch backend, running in supervisor mode. switch.S holds the trap vector, the save
// frame and the entries; trap.S the privilege probe.
//
// satp is written only by the map editor (aspace_rv64imac.cc).
//
// The interrupt controller is software: mask/unmask/clear_pending are a bitmask, and a raise
// reaches the ISR path through sip.SSIP.

#include <kickos/arch/arch.h>
#include "ctx_redirect.h"
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/rv64_doorbell.h>
#include <kickos/arch/rv64_frame.h>
#include <kickos/arch/rv64_uguard.h>
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    void kprintf(char const* fmt, ...);
    void kpanic(char const* msg) __attribute__((noreturn));
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

namespace
{
    constexpr uint64_t SSTATUS_SPIE = KICKOS_RV64_SSTATUS_SPIE;
    constexpr uint64_t SSTATUS_SPP = KICKOS_RV64_SSTATUS_SPP;

    // sstatus.UXL at 33:32: 1 is 32, 2 is 64. WARL, so a written 0 may read back as 32 and
    // truncate every U-mode address. .Lrestore writes sstatus whole from the frame, so a
    // fabricated frame must set this field.
    constexpr unsigned SSTATUS_UXL_SHIFT = 32;
    constexpr uint64_t SSTATUS_UXL_MASK = 0x3ull << SSTATUS_UXL_SHIFT;
    constexpr uint64_t SSTATUS_UXL_64 = 0x2ull << SSTATUS_UXL_SHIFT;

    constexpr uint64_t SCAUSE_INTERRUPT = 1ull << 63;

    // Interrupt causes, which are also the sie/sip bit positions.
    constexpr uint64_t INT_SUPERVISOR_SOFTWARE = 1;
    constexpr uint64_t INT_SUPERVISOR_TIMER = 5;
    constexpr uint64_t SIE_SSIE = 1ull << INT_SUPERVISOR_SOFTWARE;
    constexpr uint64_t SIE_STIE = 1ull << INT_SUPERVISOR_TIMER;
    constexpr uint64_t SIP_SSIP = 1ull << INT_SUPERVISOR_SOFTWARE;

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

#if KICKOS_KERNEL_CORES > 1
    using kickos::Atomic;
    using kickos::Order;
    using RowWord = Atomic<uint32_t, Order::RELAXED>;

    // One row per hart, every word written by its own hart alone. A line's state lives in the
    // row of the hart it is routed to (arch_irq_line_core), where every mask, unmask, clear and
    // raise lands. The row's thread-context and ISR-context writers are one hart and
    // arch_irq_save excludes them from each other, so no word needs an atomic
    // read-modify-write and no local handshake needs a fence.
    //
    // A row keeps a line unmasked only while it is that line's route: the release masks a line
    // before it drops the route. A pending bit left in a row the line has left is discarded by
    // the next claim's first arm.
    //
    // A set bit in `unmasked` means unmasked, so zero is the arch.h reset state.
    struct alignas(KICKOS_DOORBELL_LINE) IrqRow
    {
        RowWord unmasked;
        RowWord pending;
        // A set, so two raises landing between dispatches both survive.
        RowWord raised;
    };
    IrqRow g_irq_row[KICKOS_NUM_CORES] = {};

    // The hart arch_irq_route named for a line, biased by one so that zero is unrouted.
    // Written under the kernel lock. An unrouted line is hart zero's.
    Atomic<uint8_t, Order::ACQUIRE | Order::RELEASE> g_line_hart[IRQ_LINES] = {};

    // Raises for a line routed to a peer. A line's bit is owed from a to b while
    // g_posted[a].to[b] and g_acked[b].to[a] differ in it: a flips its posted bit only when
    // it reads nothing owed, and b copies the posted word it read into its acked word when it
    // takes them, so each word keeps one writer and a second raise before the take coalesces.
    struct alignas(KICKOS_DOORBELL_LINE) PostRow
    {
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> to[KICKOS_NUM_CORES];
    };
    PostRow g_posted[KICKOS_NUM_CORES] = {};
    PostRow g_acked[KICKOS_NUM_CORES] = {};

    // Store->load, the one order RVWMO does not preserve. FENCE.TSO is not a substitute: it
    // omits exactly that edge.
    void fence_store_load(void)
    {
        __asm volatile("fence rw, rw" ::: "memory");
    }

    uint32_t line_hart(int line)
    {
        uint32_t const cell = g_line_hart[line].load();
        if (cell == 0u)
        {
            return 0u;
        }
        return cell - 1u;
    }

    void raise_here(IrqRow& row, uint32_t bit)
    {
        row.raised = row.raised.load() | bit;
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

    // A raise on a masked line latches one-deep and is redelivered at the unmask.
    void inject_here(IrqRow& row, uint32_t bit)
    {
        if ((row.unmasked.load() & bit) != 0u)
        {
            raise_here(row, bit);
        }
        else
        {
            row.pending = row.pending.load() | bit;
        }
    }

    // The fence pairs with take_posts', which writes its acked word and then reads what the
    // raise publishes. Without a fence on both sides both reads may be stale at once: this finds
    // the line still owed and coalesces, the owner's handler reads the caller's state from
    // before the raise, and the driver sleeps for good.
    void post_to(uint32_t hart, uint32_t me, uint32_t bit)
    {
        uint32_t const posted = g_posted[me].to[hart].load();
        fence_store_load();
        if (((posted ^ g_acked[hart].to[me].load()) & bit) != 0u)
        {
            return;
        }
        g_posted[me].to[hart] = posted ^ bit;
        kickos_rv64_doorbell_send(1u << hart);
    }

    // Moves every raise a peer has posted for this hart into its own row, as raised or, for a
    // line masked here, as pending. Caller's interrupts masked.
    //
    // Inlined: check_rv64_irq_fence.sh reads the acked release out of every body that takes.
    __attribute__((always_inline)) inline void take_posts(uint32_t me, IrqRow& row)
    {
        uint32_t taken = 0u;
        for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
        {
            uint32_t const posted = g_posted[from].to[me].load();
            uint32_t const owed = posted ^ g_acked[me].to[from].load();
            if (owed != 0u)
            {
                g_acked[me].to[from] = posted;
                // Pairs with post_to's fence: this store before any load the handler makes.
                fence_store_load();
                taken = taken | owed;
            }
        }
        uint32_t const now = taken & row.unmasked.load();
        row.pending = row.pending.load() | (taken & ~now);
        row.raised = row.raised.load() | now;
    }

    // Takes this hart's whole set, posts included, and leaves it empty.
    uint32_t take_raised(void)
    {
        uint32_t const me = arch_cpu_id();
        IrqRow& row = g_irq_row[me];
        take_posts(me, row);
        uint32_t const taken = row.raised.load();
        row.raised = 0u;
        return taken;
    }
#else
    // Controller state shared by all cores: the kernel's IRQ layer masks a line on whichever
    // core services it and unmasks it on whichever core its driver runs on.
    //
    // A set bit in g_irq_unmasked means unmasked, so zero is the arch.h reset state.
    uint32_t g_irq_unmasked = 0;
    uint32_t g_irq_pending = 0;

    // Bit set = the line has a raise no dispatch has taken yet. A set and not one line
    // identity: a single identity is overwritten by the next producer before the first core's
    // dispatch consumes it, losing the raise and leaving its driver asleep for good.
    //
    // This word owes no fence: raise_line sets sip.SSIP on the calling hart, so the consumer
    // that must not miss a bit is that hart's own dispatch and doorbell poll, and same-hart
    // accesses to one address are ordered. A peer's poll may read it stale at no cost.
    uint32_t g_irq_raised = 0;

    // The three words above are touched from ISR and thread context on any core, and the ISR
    // path holds no kernel lock, so a plain read-modify-write could let a mask on one core
    // clobber a rearm's unmask on another and leave the line masked for good. Every mutation
    // below is one AMO, and none of them orders anything: the "memory" clobber constrains GCC
    // and not the hardware, and an AMO with .aq and .rl both clear adds no order. A caller that
    // publishes to one of these words and then reads another owes the fence below.
    uint32_t load_word(uint32_t const* w)
    {
        uint32_t v = 0;
        __asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(w) : "memory");
        return v;
    }

    void set_bit(uint32_t* w, uint32_t bit)
    {
        __asm volatile("amoor.w zero, %0, (%1)" ::"r"(bit), "r"(w) : "memory");
    }

    // Answers whether THIS caller cleared the bit, so a redelivery happens exactly once when
    // two cores race for it.
    bool take_bit(uint32_t* w, uint32_t bit)
    {
        uint32_t old = 0;
        __asm volatile("amoand.w %0, %1, (%2)" : "=&r"(old) : "r"(~bit), "r"(w) : "memory");
        return (old & bit) != 0;
    }

    // Store->load, the one order RVWMO does not preserve. FENCE.TSO is not a substitute: it
    // omits exactly that edge.
    void fence_store_load(void)
    {
        __asm volatile("fence rw, rw" ::: "memory");
    }

    void raise_line(int line)
    {
        uint32_t const bit = 1u << line;
        __asm volatile("amoor.w zero, %0, (%1)" ::"r"(bit), "r"(&g_irq_raised) : "memory");
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

    // Takes the whole set and leaves it empty, so a raise landing after this is a new one.
    uint32_t take_raised(void)
    {
        uint32_t taken = 0;
        __asm volatile("amoswap.w %0, zero, (%1)" : "=r"(taken) : "r"(&g_irq_raised) : "memory");
        return taken;
    }
#endif

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

extern "C" int kickos_rv64_privilege_probe(void);

extern "C" void kickos_rv64_stvec(void);
extern "C" void kickos_rv64_switch_now(struct arch_context* from, struct arch_context* to);
extern "C" void kickos_rv64_start(struct arch_context* first);

extern "C" void kickos_user_thread_return(void);

extern "C"
{
    // One row per hart: the trusted trap stack, and above it the block sscratch points at.
    struct rv64_percpu_row kickos_rv64_percpu[KICKOS_NUM_CORES] = {};
}

static_assert(offsetof(struct rv64_percpu_block, ctx_current) == 0,
              "switch.S spells PERCPU_CTX_CURRENT as 0");
static_assert(offsetof(struct rv64_percpu_block, switch_to) == 8,
              "switch.S spells PERCPU_SWITCH_TO as 8");
static_assert(offsetof(struct rv64_percpu_block, isr_depth) == 16,
              "switch.S spells PERCPU_ISR_DEPTH as 16");
#if KICKOS_BENCH
static_assert(offsetof(struct rv64_percpu_block, bench_sw_start) == 24,
              "switch.S spells PERCPU_BENCH_SW_START as 24");
#endif
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
#if KICKOS_TLS
static_assert(offsetof(struct arch_context, tls_base) == KICKOS_RV64_CTX_OFF_TLS_BASE,
              "the frame header disagrees with the struct on tls_base");
#endif
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_RV64_CTX_OFF_KERNEL_SP,
              "the U-mode entry loads ctx.kernel_sp at CTX_KERNEL_SP");
#if KICKOS_REENT_PER_THREAD
static_assert(offsetof(struct arch_context, reent_tp) == KICKOS_RV64_CTX_OFF_REENT_TP,
              "the restore loads ctx.reent_tp at CTX_REENT_TP");
#endif

// ARCH_KERNEL_STACKS_MANDATORY puts `range 1 1` on the knob; this fires if that select is
// dropped.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "rv64imac's trap entry builds every U-mode frame on ctx.kernel_sp");
static_assert(KICKOS_RV64_FRAME % KICKOS_RV64_SP_ALIGN == 0,
              "the frame size must preserve the psABI stack alignment");
static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_RV64_SP_ALIGN == 0,
              "a kernel block's top must land on the alignment the prologue requires");
static_assert(KICKOS_RV64_TRAP_STACK_SIZE % KICKOS_RV64_SP_ALIGN == 0,
              "the trap-stack top must land on the alignment the prologue requires");
static_assert(KICKOS_RV64_TRAP_FRAME == KICKOS_RV64_FRAME,
              "rv64_trap_stack.h prices the frame every entry builds");
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RV64_TASK_START,
              "a thread on the least stack a spawn admits may run its space's startup");
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 9,
              "KICKOS_RV64_TRAP_DEPTH_SYSK was measured at a spawn staging 9 grants on the "
              "kernel block: re-measure with tests/static/check_trap_redzone.sh first");
// The gate reads KICKOS_RV64_TRAP_NEST as an immediate.
static_assert(KICKOS_RV64_TRAP_NEST == KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_TRAP_DEPTH_IRQ,
              "KICKOS_RV64_TRAP_NEST is an interrupt's frame plus its dispatch");
static_assert(KICKOS_RV64_TRAP_WINDOW == KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_TRAP_NEST,
              "KICKOS_RV64_TRAP_WINDOW is the U-mode entry's frame plus an interrupt's extent");
// The lowest word of a block is the overflow canary (kernel/thread/thread.cc).
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_TRAP_DEPTH_SYSK,
              "the kernel block cannot hold the U-mode entry plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV64_TRAP_WINDOW + KICKOS_RV64_TRAP_DEPTH_SYSWIN,
              "the kernel block cannot hold an interrupt taken in a system call's window");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV64_TRAP_NEST + KICKOS_RV64_TRAP_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_RV64_TRAP_DEPTH_EXITKSW,
              "the kernel block cannot hold the relocated death path's switch");
static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_RV64_TRAP_NEST + KICKOS_RV64_TRAP_DEPTH_IDLE,
              "an S-mode interrupt does not fit this board's idle stack");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_RV64_PANIC_FRAME + KICKOS_RV64_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

extern "C"
{

// --- Context / switching ----------------------------------------------------
// Builds the frame .Lrestore resumes: sepc carries the entry and a0 its argument.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    ctx->stack_lo = reinterpret_cast<uintptr_t>(stack_base);
    uintptr_t const top = (ctx->stack_lo + stack_size)
                          & ~static_cast<uintptr_t>(KICKOS_RV64_SP_ALIGN - 1);
    ctx->stack_hi = top;
#if KICKOS_TLS
    ctx->tls_base = 0;
#endif
#if KICKOS_REENT_PER_THREAD
    ctx->reent_tp = 0;
#endif
    // Where this frame sits is the privilege boundary: it carries sstatus and sepc, so whoever
    // can write it chooses the level and PC of the sret, and a thread's own stack is writable
    // by its task. An unprivileged thread's first frame goes on its kernel block. A privileged
    // thread runs its whole life on this sp, which a block sized for one dispatch cannot hold,
    // so its frame stays on the stack handed in.
    uintptr_t frame_top = top;
    if (privileged == 0)
    {
        // Nonzero: thread_create asserts it (ARCH_KERNEL_STACKS_MANDATORY).
        frame_top = ctx->kernel_sp;
    }
    uintptr_t const base = frame_top - KICKOS_RV64_FRAME;
    uint64_t* const f = reinterpret_cast<uint64_t*>(base);
    for (size_t i = 0; i < KICKOS_RV64_FRAME / sizeof(uint64_t); i++)
    {
        f[i] = 0;
    }

    // SIE stays 0 in the word because .Lrestore writes sstatus while still inside the
    // epilogue; the sret sets it from SPIE.
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
    f[KICKOS_RV64_F_RA / 8] = ret;
    f[KICKOS_RV64_F_A0 / 8] = reinterpret_cast<uint64_t>(arg);
    // The sp .Lrestore leaves on: the thread's own stack, whole.
    f[KICKOS_RV64_F_SP / 8] = top;
    ctx->sp = base;
}

void arch_context_second_arg(struct arch_context* ctx, uintptr_t value)
{
    reinterpret_cast<uint64_t*>(ctx->sp)[KICKOS_RV64_F_A1 / 8] = value;
}

void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
#if KICKOS_TLS
    uintptr_t const tls_base = ctx->tls_base;
#endif
#if KICKOS_REENT_PER_THREAD
    uintptr_t const reent_tp = ctx->reent_tp;
#endif
    arch_ctx_redirect_to_block(ctx, entry, stack_base, stack_size);
#if KICKOS_TLS
    ctx->tls_base = tls_base;
#endif
#if KICKOS_REENT_PER_THREAD
    ctx->reent_tp = reent_tp;
#endif
}

#if KICKOS_REENT_PER_THREAD
void arch_context_seat_reent(struct arch_context* ctx, void* state, uint32_t)
{
    ctx->reent_tp = reinterpret_cast<uintptr_t>(state);
}
#endif

// From an ISR the switch is deferred to the interrupt's resume.
//
// The deferred arm relies on every interrupt frame being a resumable thread context on a stack
// that outlives the trap: a U-mode interrupt lands on the thread's kernel block, an S-mode one
// on the interrupted thread's stack, and syscall dispatch runs with SIE masked.
//
// The thread-context arm requires interrupts masked. The publish below and the register save
// in kickos_rv64_switch_now are two steps, so an interrupt between them reaches .Lintr with
// ctx_current already naming `to`, and the booked swap would store a pointer into `from`'s
// stack as `to`'s saved context.
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (rv64_percpu()->isr_depth != 0)
    {
        // switch.S reads the outgoing context from ctx_current.
        rv64_percpu()->switch_to = to;
        return;
    }
    rv64_percpu()->ctx_current = to;
    kickos_rv64_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot;
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

// Only the interrupt leg bumps isr_depth, so this reads FALSE inside syscall dispatch, which
// the kernel's blocking primitives depend on.
int arch_in_isr(void)
{
    return rv64_percpu()->isr_depth != 0;
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

// --- Interrupt controller ---------------------------------------------------
#if KICKOS_KERNEL_CORES > 1
// Each body is self-bracketed as arch.h requires, and reads its hart index inside the bracket.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    IrqRow& row = g_irq_row[arch_cpu_id()];
    row.unmasked = row.unmasked.load() & ~(1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    uint32_t const bit = 1u << line;
    arch_irq_state_t s = arch_irq_save();
    IrqRow& row = g_irq_row[arch_cpu_id()];
    row.unmasked = row.unmasked.load() | bit;
    if ((row.pending.load() & bit) != 0u)
    {
        row.pending = row.pending.load() & ~bit;
        raise_here(row, bit);
    }
    arch_irq_restore(s);
}

// Discards every raise of the line this hart holds: the latch, an undispatched raise, and a
// peer's untaken post, which the take here turns into one of the other two.
void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    uint32_t const bit = 1u << line;
    arch_irq_state_t s = arch_irq_save();
    uint32_t const me = arch_cpu_id();
    IrqRow& row = g_irq_row[me];
    take_posts(me, row);
    row.pending = row.pending.load() & ~bit;
    row.raised = row.raised.load() & ~bit;
    if (row.raised.load() != 0u)
    {
        // The take may have raised other lines, and only the dispatch delivers them.
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }
    arch_irq_restore(s);
}

bool arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return false;
    }
    uint32_t const bit = 1u << irq;
    arch_irq_state_t s = arch_irq_save();
    uint32_t const me = arch_cpu_id();
    uint32_t const hart = line_hart(irq);
    if (hart == me)
    {
        inject_here(g_irq_row[me], bit);
    }
    else
    {
        post_to(hart, me, bit);
    }
    arch_irq_restore(s);
    return true;
}

// Called with the line masked in its old route's row.
void arch_irq_route(int line, uint32_t core)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    if (core == KICKOS_IRQ_ROUTE_NONE)
    {
        g_line_hart[line] = 0u;
        return;
    }
    if (core >= KICKOS_KERNEL_CORES)
    {
        return;
    }
    g_line_hart[line] = static_cast<uint8_t>(core + 1u);
}

int arch_irq_line_core(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return KICKOS_IRQ_LINE_CORE_NONE;
    }
    return static_cast<int>(line_hart(line));
}

// Whether this hart owes a dispatch a line: its own row's set, or a peer's untaken post.
int kickos_rv64_inject_owed(void)
{
    uint32_t const me = arch_cpu_id();
    if (g_irq_row[me].raised.load() != 0u)
    {
        return 1;
    }
    for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
    {
        if (g_posted[from].to[me].load() != g_acked[me].to[from].load())
        {
            return 1;
        }
    }
    return 0;
}
#else
// Each body is self-bracketed as arch.h requires.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    (void)take_bit(&g_irq_unmasked, 1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    uint32_t const bit = 1u << line;
    arch_irq_state_t s = arch_irq_save();
    set_bit(&g_irq_unmasked, bit);
    // A raise latched while the line was masked is redelivered here: sip.SSIP is set with SIE
    // clear, so it fires at arch_irq_restore.
    //
    // The fence is what makes the handshake hold. This writes g_irq_unmasked then reads
    // g_irq_pending and arch_irq_inject does the reverse, so without a fence on both sides both
    // writes may sit behind both reads and neither side raises: the bit stays pending, the line
    // unmasked, and the driver asleep for good. The take only settles which side delivers.
    // arch_irq_save masks this hart alone, which is no exclusion against a peer.
    fence_store_load();
    if (take_bit(&g_irq_pending, bit))
    {
        raise_line(line);
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
    (void)take_bit(&g_irq_pending, 1u << line);
    arch_irq_restore(s);
}

bool arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return false;
    }
    uint32_t const bit = 1u << irq;
    arch_irq_state_t s = arch_irq_save();
    if ((load_word(&g_irq_unmasked) & bit) != 0)
    {
        raise_line(irq);
    }
    else
    {
        set_bit(&g_irq_pending, bit);
        // A rearm may have unmasked the line between the read above and the latch without
        // seeing this store. Re-read and take the bit back: exactly one side takes it and
        // delivers.
        //
        // Pairs with arch_irq_unmask's fence: the latch must be visible to that hart's take
        // before this re-read answers, or both sides read stale and no raise is made.
        fence_store_load();
        if ((load_word(&g_irq_unmasked) & bit) != 0 and take_bit(&g_irq_pending, bit))
        {
            raise_line(irq);
        }
    }
    arch_irq_restore(s);
    return true;
}

// Whether a device line is still raised.
int kickos_rv64_inject_owed(void)
{
    uint32_t raised = 0;
    __asm volatile("lw %0, 0(%1)" : "=r"(raised) : "r"(&g_irq_raised) : "memory");
    return raised != 0u;
}
#endif

// The interrupt leg of the entry (switch.S .Lintr), ISR context with SIE clear.
//
// The timer's STIP must not be cleared here: Sstc drives it from `time >= stimecmp`, so
// kickos_isr_timer's re-arm or disarm lowers it, and sip.STIP is read-only.
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
        // sip.SSIP carries a peer's doorbell, the reschedule it may stand for, and a device-line
        // raise. Each arm is gated on its own state, so a raise carrying two loses neither. The
        // order is the contract: a wrong order drops a device raise or a rendezvous and shows up
        // as a hang under load.

        // First, so a raise landing during the work below stays pending and is delivered again.
        __asm volatile("csrc sip, %0" ::"r"(SIP_SSIP) : "memory");

#if KICKOS_NUM_CORES > 1
        // Second, the doorbell's service, which takes no kernel lock: an initiator may hold it
        // while it waits here. The cell is the authority, not the raise.
        if (kickos_rv64_doorbell_pending() != 0)
        {
            kickos_doorbell_service();
        }
#endif
#if KICKOS_KERNEL_CORES > 1
        // Third, outside the service body because it takes the kernel lock.
        kickos_kernel_core_resched_if_owed();
#endif
        // Fourth, every line in the set: two raises can land between dispatches, and a line
        // left in the set with the cause already cleared is a driver that never wakes.
        uint32_t raised = take_raised();
        for (int line = 0; line < IRQ_LINES and raised != 0; line++)
        {
            uint32_t const bit = 1u << line;
            if ((raised & bit) != 0)
            {
                raised &= ~bit;
                kickos_isr_irq(line);
            }
        }
        return;
    }
    rv64_unexpected_interrupt(scause);
}

// --- Fault isolation --------------------------------------------------------
// sstatus.SPP IS READ FROM THE REGISTER, so no word of the frame is trusted. .Lecall runs
// syscall dispatch in S-mode on the thread's kernel block, so a fault there is a kernel bug.
//
// The second test checks the frame is the one the U-mode entry built on that block; every
// other frame fails closed to the panic dump.
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

// F_SP must be set too: without it the stub would run privileged on the sp the faulting thread
// chose.
void arch_fault_redirect_to_exit(void* frame)
{
    uint64_t scause = 0;
    uint64_t sepc = 0;
    uint64_t stval = 0;
    __asm volatile("csrr %0, scause" : "=r"(scause));
    __asm volatile("csrr %0, sepc" : "=r"(sepc));
    __asm volatile("csrr %0, stval" : "=r"(stval));
    // For an illegal instruction stval holds the instruction bits, not an address.
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

void arch_irq_window(void)
{
    __asm volatile("csrsi sstatus, 2\n\tnop\n\tcsrci sstatus, 2" ::: "memory");
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Unhandled supervisor trap (switch.S .Lfault) ---------------------------
// True means .Lfault must sret off the frame instead of dumping: fault isolation claimed the
// fault and re-pointed the frame at kickos_thread_fault_exit.
//
// Every CSR is read once at the top, before anything below can trap and overwrite them.
// sstatus comes from the frame, the value .Lrestore will consume.
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

    kickos_fault_report_begin();
    kpanic_enter();

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

// A U-mode trap from a thread whose ctx.kernel_sp is 0 (switch.S .Ltrap_nokstack).
[[noreturn]] void kickos_rv64_no_kernel_stack(void)
{
    kpanic_enter();
    ::kickos::kprintf("\n=== RISC-V S-TRAP (no kernel block seated) ===\n");
    kfault_terminate();
}

#if KICKOS_NUM_CORES > 1
// --- Core identity ----------------------------------------------------------
// sscratch is seated on every hart by the machine-mode prologue and holds this hart's block
// address outside the trap entry's two-instruction prologue.
struct rv64_percpu_block* rv64_percpu(void)
{
    uintptr_t blk = 0;
    __asm volatile("csrr %0, sscratch" : "=r"(blk));
    return reinterpret_cast<struct rv64_percpu_block*>(blk);
}

// mhartid IS NOT THE INDEX: it is integrator-chosen. The dense index is derived here from the
// row sscratch names, and a pointer naming no row is refused.
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

// The boot hart's sstatus as kickos_rv64_init read it back after clearing SUM.
static uint64_t g_sum_readback = 0;
static bool g_sum_recorded = false;

char const* arch_uaccess_guard(void)
{
    return ::kickos::rv64::sum_posture(g_sum_readback);
}

#if defined(KICKOS_WITNESS_UACCESS) && KICKOS_WITNESS_UACCESS
// No flag the interrupted context controls lifts this guard.
int arch_witness_irq_lift_flag(void)
{
    return -1;
}
#endif

// --- One-time core bring-up ------------------------------------------------
void kickos_rv64_init(void)
{
    // Direct mode (low 2 bits 00).
    uintptr_t const tv = reinterpret_cast<uintptr_t>(&kickos_rv64_stvec);
    __asm volatile("csrw stvec, %0" ::"r"(tv) : "memory");

    // First: everything below indexes per-hart state with it.
    struct rv64_percpu_block* const blk = rv64_percpu_seat();

    // The entry swaps sp with sscratch, so it must hold the trusted top before the first trap.
    // The block's own address is that top.
    __asm volatile("csrw sscratch, %0" ::"r"(blk) : "memory");

    // Read by switch.S's .Ltrap_reentry to tell a fault inside the reporter from a descent that
    // ran off the row: a store past the bottom lands in ordinary .bss and takes no trap.
    *reinterpret_cast<uint64_t*>(kickos_rv64_percpu[arch_cpu_id()].trap_stack) =
        KICKOS_RV64_TRAP_CANARY;

    // Zero means the boot table never took.
    uint64_t boot_satp = 0;
    __asm volatile("csrr %0, satp" : "=r"(boot_satp));
    if (boot_satp == 0)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (no translation root) ===\n");
        kfault_terminate();
    }

    // Confirms the drop startup.S performs; the probe needs the vector installed above.
    if (kickos_rv64_privilege_probe() == 0)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (hart is not in supervisor mode) ===\n");
        kfault_terminate();
    }

    // After the probe, whose trap leg rewrites sstatus. UXL is WARL and may be read-only; a
    // hart keeping the RV32 encoding truncates every U-mode address. Written whole: an
    // intermediate 0 or 3 in the field is a reserved value.
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

    // Whatever firmware left: the kernel reaches process memory only through the kaccess seam,
    // and with SUM clear a dereference of a low-half pointer faults.
    __asm volatile("csrc sstatus, %0" ::"r"(static_cast<uint64_t>(KICKOS_RV64_SSTATUS_SUM))
                   : "memory");
    uint64_t sum_seen = 0;
    __asm volatile("csrr %0, sstatus" : "=r"(sum_seen));
    char const* const sum_why = ::kickos::rv64::sum_refusal(sum_seen);
    if (sum_why != nullptr)
    {
        ::kickos::kpanic(sum_why);
    }
    if (not g_sum_recorded)
    {
        g_sum_readback = sum_seen;
        g_sum_recorded = true;
    }

    // After the probe, whose trap leg clears sstatus.SIE and never srets it back, so it must
    // run before any source can fire.
    uint64_t const sie = SIE_STIE | SIE_SSIE;
    __asm volatile("csrw sie, %0" ::"r"(sie) : "memory");
}

}
