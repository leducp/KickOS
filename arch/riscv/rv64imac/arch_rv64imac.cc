// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV64IMAC arch backend: the ISA-generic half of the arch.h seam. switch.S holds the
// trap vector, the save frame and the entries; trap.S the supervisor-mode confirmation.
//
// This port runs in supervisor mode, so every CSR here is an s-prefixed one and nothing is
// shared with the machine-mode rv32imac backend beside it.
//
// satp belongs to the map editor in aspace_rv64imac.cc: one root serves both privilege levels,
// so it moves only when the space does.
//
// The interrupt controller is pure software, there being no PLIC and no external interrupt in
// this image. mask/unmask/clear_pending are a bitmask, and one raise reaches the ISR path
// through sip.SSIP, which S-mode may write itself.

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/rv64_doorbell.h>
#include <kickos/arch/rv64_frame.h>
#include <kickos/diag.h>
#include <kickos/sys/atomic.h>

#if KICKOS_KERNEL_CORES > 1
#include <kickos/arch/doorbell_part.h>
#endif

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

#if KICKOS_KERNEL_CORES > 1
    static_assert(KICKOS_MULTICORE_MODEL_SHARED,
                  "the kernel's core index names the hart only under one shared kernel");

    using kickos::Atomic;
    using kickos::Order;
    using RowWord = Atomic<uint32_t, Order::RELAXED>;

    // The software controller's state, one row per hart, every word written by its own hart
    // alone. A line's state lives in the row of the hart it is routed to: arch_irq_line_core
    // names that hart, so kernel/irq/irq_route.cc performs every mask, unmask and clear there,
    // and every raise lands there. A row's thread-context and ISR-context writers are then one
    // hart, which arch_irq_save excludes from each other, so no word needs an atomic
    // read-modify-write and no local handshake needs a fence.
    //
    // A row keeps a line unmasked only while it is that line's route: the release masks a line
    // before it drops the route. A pending bit left in a row the line has left is discarded by
    // the next claim's first arm.
    //
    // Unmasked rather than masked, so that zero is the arch.h reset contract.
    struct alignas(KICKOS_DOORBELL_LINE) IrqRow
    {
        RowWord unmasked;
        RowWord pending;
        // A raise this hart's dispatch has not taken yet. A SET, so two raises landing between
        // dispatches both survive.
        RowWord raised;
    };
    IrqRow g_irq_row[KICKOS_NUM_CORES] = {};

    // The hart arch_irq_route named for a line, biased by one so that the unrouted state is
    // zero. Written under the kernel lock. An unrouted line is hart zero's.
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

    // Store->load, the one order RVWMO does not preserve (RISC-V Unprivileged ISA 18.1.3);
    // `rw,rw` is the form the specification's mapping guidelines use for full ordering
    // (Appendix A.5). FENCE.TSO IS NOT A SUBSTITUTE; it omits exactly the store->load edge.
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

    // sip.SSIP is the one cause every raise on this hart arrives on.
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

    // The fence is half of a handshake with take_posts, which writes its acked word and then
    // reads what the raise publishes; this reads that acked word after the caller's stores.
    // With no fence on both sides both reads may be stale at once: this finds the line still
    // owed and coalesces, the owner's handler reads the caller's state from before the raise,
    // and the driver sleeps for good.
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
    // Inlined into each caller: check_rv64_irq_fence.sh reads the acked release out of every
    // body that takes.
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
                // The other half of post_to's: this store before any load the handler makes.
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
    // The software controller's state, image-wide and not per hart. Nothing on this board
    // implements an interrupt controller, so these mirror no per-hart registers: a line is one
    // logical resource, and the kernel's IRQ layer masks it on whichever core services it and
    // unmasks it on whichever core its driver runs on. Every caller holds the kernel lock
    // (kernel/irq/irq.cc and the syscall entries), which is the exclusion that covers them.
    //
    // Unmasked rather than masked, so that zero is the arch.h reset contract and no
    // initialiser is owed.
    uint32_t g_irq_unmasked = 0;
    uint32_t g_irq_pending = 0;

    // Bit set = this line has a raise no dispatch has taken yet. A set, not one line identity:
    // above one core there are as many producers as cores, and a single identity is overwritten
    // by the next producer before the first core's dispatch has consumed it, which loses the
    // raise outright and leaves its driver asleep for good.
    //
    // This word owes no fence, and not because its accesses are single instructions: one
    // instruction buys atomicity, never visibility. What buys visibility is that raise_line
    // sets sip.SSIP on the calling hart, so the consumer that must not miss a bit is the hart
    // that set it: that hart's own dispatch, and its own doorbell poll. Same-hart accesses to
    // one address are ordered by the load value axiom (RISC-V Unprivileged ISA, Appendix
    // A.3.2). A peer's poll may read this word stale; that costs nothing in either direction,
    // the producing hart's sip.SSIP still standing.
    uint32_t g_irq_raised = 0;

    // The three words above are touched from ISR context and from thread context on any core,
    // and the ISR path holds no kernel lock: kickos_isr_irq brackets with an epoch above one
    // core, and irq_event_isr masks the line from inside it. So a plain read-modify-write here
    // would let a mask on one core clobber a rearm's unmask on another, which leaves the line
    // masked with nothing left to unmask it. Every mutation below is one instruction, and none
    // of them orders anything: the "memory" clobber constrains GCC and not the hardware, RVWMO
    // preserves no order between a store and a later load to a different address (RISC-V
    // Unprivileged ISA 18.1.3), and an AMO with .aq and .rl both clear adds none (13.1). A
    // caller that publishes to one of these words and then reads another owes the fence below.
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

    // Clears the bit and answers whether THIS caller is the one that cleared it, which is what
    // makes a redelivery happen exactly once when two cores race for it.
    bool take_bit(uint32_t* w, uint32_t bit)
    {
        uint32_t old = 0;
        __asm volatile("amoand.w %0, %1, (%2)" : "=&r"(old) : "r"(~bit), "r"(w) : "memory");
        return (old & bit) != 0;
    }

    // Store->load, the one order RVWMO does not preserve; `rw,rw` is the form the
    // specification's mapping guidelines use for full ordering (RISC-V Unprivileged ISA,
    // Appendix A.5). FENCE.TSO IS NOT A SUBSTITUTE; it omits exactly the store->load edge.
    // FENCE is base-ISA (2.7), which this board's march string needs it to be: Zifencei is
    // absent, so FENCE.I does not assemble here.
    void fence_store_load(void)
    {
        __asm volatile("fence rw, rw" ::: "memory");
    }

    // sip.SSIP is the one cause every raise on this hart arrives on.
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
#if defined(KICKOS_TLS) && KICKOS_TLS
static_assert(offsetof(struct arch_context, tls_base) == KICKOS_RV64_CTX_OFF_TLS_BASE,
              "the frame header disagrees with the struct on tls_base");
#endif
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_RV64_CTX_OFF_KERNEL_SP,
              "the U-mode entry loads ctx.kernel_sp at CTX_KERNEL_SP");
#if KICKOS_REENT_PER_THREAD
static_assert(offsetof(struct arch_context, reent_tp) == KICKOS_RV64_CTX_OFF_REENT_TP,
              "the restore loads ctx.reent_tp at CTX_REENT_TP");
#endif

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
static_assert(KICKOS_RV64_TRAP_FRAME == KICKOS_RV64_FRAME,
              "rv64_trap_stack.h prices the frame every entry builds");
// The gate reads KICKOS_RV64_TRAP_NEST as an immediate, so the sum is spelled out there.
static_assert(KICKOS_RV64_TRAP_NEST == KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_TRAP_DEPTH_IRQ,
              "KICKOS_RV64_TRAP_NEST is an interrupt's frame plus its dispatch");
// The lowest word of a block is the overflow canary (kernel/thread/thread.cc).
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV64_TRAP_FRAME + KICKOS_RV64_TRAP_DEPTH_SYSK,
              "the kernel block cannot hold the U-mode entry plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_RV64_TRAP_NEST + KICKOS_RV64_TRAP_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_RV64_TRAP_DEPTH_EXITKSW,
              "the kernel block cannot hold the relocated death path's switch");
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RV64_TRAP_NEST + KICKOS_RV64_TRAP_DEPTH_RET,
              "the spawn floor cannot hold a privileged thread's entry return");
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RV64_TRAP_DEPTH_RETSW,
              "the spawn floor cannot hold a privileged thread's entry return through the switch");
static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_RV64_TRAP_NEST + KICKOS_RV64_TRAP_DEPTH_IDLE,
              "an S-mode interrupt does not fit this board's idle stack");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_RV64_PANIC_FRAME + KICKOS_RV64_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

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
#if KICKOS_REENT_PER_THREAD
    ctx->reent_tp = 0;
#endif
    // ctx->kernel_sp is read, not written, here: thread_create seats the block before this
    // call and owns the zero that means none is seated.

    // Where this frame sits is the privilege boundary: it carries sstatus and sepc, so whoever
    // can write it chooses the level and the PC of the sret that starts the thread, and a
    // thread's own stack is writable by its task. An unprivileged thread's first frame goes on
    // its kernel block. A privileged thread resumes at S-mode on this sp and would then run its
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

    // SPIE: the sret sets SIE from it, and this is the system's first enable. SIE stays 0 in
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
#if KICKOS_REENT_PER_THREAD
    uintptr_t const reent_tp = ctx->reent_tp;
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
#if KICKOS_REENT_PER_THREAD
        ctx->reent_tp = reent_tp;
#endif
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
#if defined(KICKOS_TLS) && KICKOS_TLS
    ctx->tls_base = tls_base;
#endif
#if KICKOS_REENT_PER_THREAD
    ctx->reent_tp = reent_tp;
#endif
}

#if KICKOS_REENT_PER_THREAD
void arch_context_seat_reent(struct arch_context* ctx, void* state)
{
    ctx->reent_tp = reinterpret_cast<uintptr_t>(state);
}
#endif

// Synchronous in thread context and deferred from an ISR, which arch.h permits.
//
// The deferred arm rests on an invariant the entry maintains: every interrupt frame is a
// resumable thread context standing on a stack that outlives the trap. A U-mode interrupt puts
// it on the thread's own kernel block, an S-mode one on the interrupted thread's own stack, and
// syscall dispatch runs with SIE masked so no interrupt lands on the trap stack.
//
// The thread-context arm requires the caller to have interrupts masked. The publish below and
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

// The interrupt leg of the entry alone bumps it, so it reads FALSE inside syscall dispatch as
// arch.h requires: the kernel's blocking primitives depend on that.
int arch_in_isr(void)
{
    return rv64_percpu()->isr_depth != 0;
}

// --- Clocks -----------------------------------------------------------------
uint64_t arch_cpu_clock_hz(void)
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

// Nothing is deferred on this backend, so the set is already live when apply returns.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    arch_mpu_apply(regions, n, image);
}

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
#if KICKOS_KERNEL_CORES > 1
// No hardware line exists on this board, so mask/unmask/clear_pending are the calling hart's
// row, which irq_route.cc makes the line's route, and a raise reaches the ISR path through ONE
// doorbell, sip.SSIP. Each body is self-bracketed as arch.h requires, and reads its hart index
// inside the bracket.
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

// Discards every raise of the line this hart holds: the latch, a raise it has not dispatched,
// and a peer's post it has not taken, which the take here turns into one of the other two.
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

// The raise lands on the hart the line is routed to, through the doorbell when that is a peer.
void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return;
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
}

// Called with the line masked in its old route's row, so no row but the new route's can arm it.
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

// The hart whose row holds the line, which is the hart the line is routed to.
int arch_irq_line_core(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return KICKOS_IRQ_LINE_CORE_NONE;
    }
    return static_cast<int>(line_hart(line));
}

// Whether this hart owes a dispatch a line: its own row's set, or a peer's post not yet taken.
// The doorbell poll reads it to know whether the raise it absorbed carried something it did not
// service.
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
// No hardware line exists on this board, so mask/unmask/clear_pending are the three words above
// and a raise reaches the ISR path through ONE doorbell, sip.SSIP. Each body is self-bracketed
// as arch.h requires.
//
// Every raise is a bit in a set rather than one line identity, so a second raise arriving before
// the first is dispatched costs nothing: both are taken by whichever dispatch runs next.
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
    // A raise taken while the line was masked redelivers now through the doorbell: sip.SSIP is
    // set with SIE clear, so it fires at arch_irq_restore on the normal ISR path.
    //
    // The fence and not the take is what makes the handshake hold. Both sides publish to their
    // own word and then read the peer's: this one writes g_irq_unmasked then reads
    // g_irq_pending and arch_irq_inject does the reverse, so with no fence on both sides both
    // writes may sit behind both reads and neither side raises: the bit pending, unmasked,
    // and the driver asleep for good. The take settles only which side delivers once each
    // write is visible. arch_irq_save brackets this body and masks this hart alone, which is
    // not exclusion against a peer.
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

void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return;
    }
    // An ISR reaching arch_irq_mask/unmask touches the same words.
    uint32_t const bit = 1u << irq;
    arch_irq_state_t s = arch_irq_save();
    if ((load_word(&g_irq_unmasked) & bit) != 0)
    {
        raise_line(irq);
    }
    else
    {
        set_bit(&g_irq_pending, bit);
        // The line may have been unmasked between the read above and the latch, by a rearm that
        // looked at a pending word this store had not reached. Re-read, and take the bit back
        // rather than assume: exactly one of the two sides takes it, and that side delivers.
        //
        // The fence is the other half of arch_irq_unmask's and is useless without it: the write
        // above must be visible to that hart's take before this re-read answers, or both sides
        // read stale and no raise is made at all.
        fence_store_load();
        if ((load_word(&g_irq_unmasked) & bit) != 0 and take_bit(&g_irq_pending, bit))
        {
            raise_line(irq);
        }
    }
    arch_irq_restore(s);
}

// Whether a device line is still latched in the controller above. The doorbell poll reads it
// to know whether the raise it absorbed carried something it did not service.
int kickos_rv64_inject_owed(void)
{
    uint32_t raised = 0;
    __asm volatile("lw %0, 0(%1)" : "=r"(raised) : "r"(&g_irq_raised) : "memory");
    return raised != 0u;
}
#endif

// The interrupt leg of the entry (switch.S .Lintr), ISR context with SIE clear. scause is
// architectural, so the demux is the arch's. `frame` is kept for a cause that has no handler.
//
// The timer's STIP must not be cleared here: Sstc drives it from `time >= stimecmp`, so
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
        // One cause, three sources, and the order is the contract. sip.SSIP carries a peer's
        // cross-hart doorbell, the reschedule that doorbell may stand for, and a device-line
        // raise, this hart's own or one a peer posted. Each arm is gated on its own state, so a
        // raise carrying two of them loses neither, and getting the order wrong drops a device
        // raise or a rendezvous and shows up as a hang under load rather than as a red gate.

        // First, and before any service: a raise landing during the work below stays pending
        // and is delivered again, rather than being cleared away underneath.
        __asm volatile("csrc sip, %0" ::"r"(SIP_SSIP) : "memory");

#if KICKOS_NUM_CORES > 1
        // Second, the doorbell's far side, which takes no kernel lock: an initiator may be
        // holding it while it waits here. The cell is the authority, not the raise.
        if (kickos_rv64_doorbell_pending() != 0)
        {
            kickos_rv64_doorbell_service();
        }
#endif
#if KICKOS_KERNEL_CORES > 1
        // Third, and outside the service body because it takes the kernel lock. The take is
        // what tells a reschedule from a rendezvous whose target owes no scheduler entry.
        if (kickos_kernel_core_resched_take() != 0)
        {
            kickos_kernel_core_resched();
        }
#endif
        // Fourth, the device lines the set carries, which share the cause with everything above.
        // Every line the set carries, not one: two raises can land between dispatches, and a
        // line left in the set with the cause already cleared is a driver that never wakes.
        // kickos_isr_irq masks the line and wakes its driver (kernel/irq/irq.cc); the driver
        // re-unmasks via irq_ack or on its next wait.
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
    // Direct mode (low 2 bits = 00): one entry point for every cause.
    uintptr_t const tv = reinterpret_cast<uintptr_t>(&kickos_rv64_stvec);
    __asm volatile("csrw stvec, %0" ::"r"(tv) : "memory");

    // The identity is seated first: everything below indexes per-hart state with it.
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

    // SUM stays clear: S-mode cannot load or store a page carrying U at all. The kernel reaches
    // memory a process owns only through the kaccess seam (kickos/aspace.h), whose acquire hands
    // back a kernel-half pointer to the frame the space's tables name. A kernel dereference of a
    // low-half pointer faults.

    // The drop startup.S performs is confirmed here and cannot be confirmed earlier: current
    // privilege is not readable on RISC-V, so the probe's refused read needs a vector to land
    // in.
    if (kickos_rv64_privilege_probe() == 0)
    {
        kpanic_enter();
        ::kickos::kprintf("\n=== RISC-V S-TRAP (hart is not in supervisor mode) ===\n");
        kfault_terminate();
    }

    // The width U-mode runs at, seated and read back before the first sret to it, and after the
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

    // STIE (the tickless deadline) and SSIE (the injected-IRQ doorbell), after the probe: its
    // trap leg clears sstatus.SIE and never srets it back, so it has to run before any source
    // can fire. sstatus.SIE is still 0 here; the first sret to a thread enables delivery.
    uint64_t const sie = SIE_STIE | SIE_SSIE;
    __asm volatile("csrw sie, %0" ::"r"(sie) : "memory");
}

}
