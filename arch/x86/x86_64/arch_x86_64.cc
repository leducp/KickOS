// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86_64 arch backend: the ISA-generic half of the arch.h seam.
//
// The interrupt controller is SOFTWARE, with the local APIC doorbell on each core. There is
// no I/O APIC, so the routed core, mask, latch and each core's delivery bits live in memory.

// BEFORE arch.h, and the position is load-bearing: an attribute binds only on a symbol's FIRST
// declaration, and the visibility attribute is what keeps the reference PC-relative
// (tools/check-x86_64-no-got.sh).
extern "C" __attribute__((visibility("hidden"))) void kickos_thread_fault_exit(void);

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/ring3.h>
#include <kickos/arch/trap.h>
#include <kickos/arch/x86_64_trap_stack.h>
#include <kickos/chip_limits.h>

#include <stddef.h>
#include <stdint.h>

// switch.S. HIDDEN is load-bearing on the trampoline whose ADDRESS is taken below: -fpie emits
// a global-offset-table load for the address of an external function, `ld -m i386pep` neither
// builds that table nor relaxes the form, and `&f` then comes out as the first eight bytes OF
// f. tools/check-x86_64-no-got.sh refuses the relocation.
#define KICKOS_X86_64_LOCAL __attribute__((visibility("hidden")))
extern "C" KICKOS_X86_64_LOCAL void kickos_x86_64_switch_now(struct arch_context* from,
                                                             struct arch_context* to);
extern "C" KICKOS_X86_64_LOCAL void kickos_x86_64_start(struct arch_context* first);
extern "C" KICKOS_X86_64_LOCAL void kickos_x86_64_thread_exit(void);

// The app's own return path (user/src/syscall_stubs.cc), which reaches the kernel exit through
// the syscall trap. An unprivileged thread's entry returns HERE and never into
// kickos_thread_return, which is kernel text it may not call.
extern "C" KICKOS_X86_64_LOCAL void kickos_user_thread_return(void);
#if KICKOS_KERNEL_CORES > 1
extern "C" void kickos_x86_64_doorbell_service(void);
#endif

namespace
{
    using kickos::x86_64::read_cr2;
    using kickos::x86_64::trap_frame;

    // switch.S spells these as literal displacements, so a field moved on one side alone is a
    // silent wrong offset.
    constexpr size_t X86_64_FRAME_SIZE = KICKOS_X86_64_TRAP_FRAME;
    static_assert(sizeof(trap_frame) == X86_64_FRAME_SIZE,
                  "switch.S reserves KOS_FRAME_SIZE bytes for one trap_frame");
    static_assert(offsetof(trap_frame, r15) == 0, "switch.S uses KOS_F_R15 0");
    static_assert(offsetof(trap_frame, rax) == 112, "switch.S uses KOS_F_RAX 112");
    static_assert(offsetof(trap_frame, rdi) == 72, "switch.S uses KOS_F_RDI 72");
    static_assert(offsetof(trap_frame, vector) == 120, "switch.S uses KOS_F_VECTOR 120");
    static_assert(offsetof(trap_frame, error) == 128, "switch.S uses KOS_F_ERROR 128");
    static_assert(offsetof(trap_frame, rip) == 136, "switch.S uses KOS_F_RIP 136");
    static_assert(offsetof(trap_frame, cs) == 144, "switch.S uses KOS_F_CS 144");
    static_assert(offsetof(trap_frame, rflags) == 152, "switch.S uses KOS_F_RFLAGS 152");
    static_assert(offsetof(trap_frame, rsp) == 160, "switch.S uses KOS_F_RSP 160");
    static_assert(offsetof(trap_frame, ss) == 168, "switch.S uses KOS_F_SS 168");
    static_assert(offsetof(struct arch_context, sp) == 0, "switch.S uses KOS_CTX_SP 0");

    // With no block seated every ring 3 entry would load a zero stack pointer. This fires if
    // the ARCH_KERNEL_STACKS_MANDATORY select is ever dropped.
    static_assert(KICKOS_KERNEL_STACKS != 0,
                  "x86_64's syscall entry loads the kernel stack out of ctx.kernel_sp");
    static_assert(KICKOS_KERNEL_STACK_SIZE % 16 == 0,
                  "a block's top must land on the psABI stack alignment the entry needs");

    // The gate reads each of these as an immediate, so the sums are spelled out there.
    static_assert(KICKOS_X86_64_TRAP_FRAME_IRQ == KICKOS_X86_64_TRAP_FRAME + 8,
                  "a same-level delivery realigns rsp to 16 bytes before the frame");
    static_assert(KICKOS_X86_64_TRAP_NEST
                      == KICKOS_X86_64_TRAP_FRAME_IRQ + KICKOS_X86_64_TRAP_DEPTH_IRQ,
                  "KICKOS_X86_64_TRAP_NEST is an interrupt's frame plus its dispatch");
    // The lowest word of a block is the overflow canary (kernel/thread/thread.cc).
    static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                      >= KICKOS_X86_64_TRAP_FRAME + KICKOS_X86_64_TRAP_DEPTH_SYSK,
                  "the kernel block cannot hold the ring 3 syscall plus its canary word");
    static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                      >= KICKOS_X86_64_TRAP_FRAME + KICKOS_X86_64_TRAP_DEPTH_IRQ,
                  "the kernel block cannot hold a ring 3 interrupt plus its canary word");
    static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                      >= KICKOS_X86_64_TRAP_NEST + KICKOS_X86_64_TRAP_DEPTH_EXITK,
                  "the kernel block cannot hold the relocated death path plus its canary word");
    static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                      >= KICKOS_X86_64_TRAP_DEPTH_EXITKSW,
                  "the kernel block cannot hold the relocated death path's switch");
    static_assert(KICKOS_MIN_STACK_SIZE
                      >= KICKOS_X86_64_TRAP_NEST + KICKOS_X86_64_TRAP_DEPTH_SYSPRIV,
                  "the spawn floor cannot hold a privileged caller's syscall");
    static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_X86_64_TRAP_DEPTH_SYSPRIVSW,
                  "the spawn floor cannot hold a privileged caller's syscall through the switch");
    static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_X86_64_TRAP_NEST + KICKOS_X86_64_TRAP_DEPTH_RET,
                  "the spawn floor cannot hold a privileged thread's entry return");
    static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_X86_64_TRAP_DEPTH_RETSW,
                  "the spawn floor cannot hold a privileged entry return through the switch");
    static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_X86_64_TRAP_NEST + KICKOS_X86_64_TRAP_DEPTH_IDLE,
                  "a ring 0 interrupt does not fit this board's idle stack");
    static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_X86_64_PANIC_FRAME + KICKOS_X86_64_PANIC_DEPTH,
                  "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

    constexpr uint64_t RFLAGS_IF = KICKOS_X86_64_RFLAGS_IF;
    // Bit 1 reads as one on every x86 processor; a resumed frame with it clear is one nothing
    // built.
    constexpr uint64_t RFLAGS_RESERVED_ONE = 1ull << 1;

    // The entry runs with interrupts masked by the gate itself. A physical
    // context and a deferred destination belong to that processor alone.
    struct alignas(64) CpuState
    {
        uint32_t isr_depth;
        struct arch_context* ctx_current;
        struct arch_context* switch_to;
    };
    CpuState g_cpu_state[KICKOS_KERNEL_CORES] = {};

    CpuState& local_state(void)
    {
        return g_cpu_state[arch_cpu_id()];
    }

    // The software controller. Bit set = masked, and every line starts masked, which is arch.h's
    // reset contract. Bounded by the CHIP's line count: a line past it would be tracked here and
    // then handed to kickos_isr_irq, whose dispatch table the chip sized.
    constexpr int IRQ_LINES = KICKOS_MAX_IRQ;
    static_assert(IRQ_LINES > 0 and IRQ_LINES <= 32,
                  "the mask and latch bitmaps are one uint32_t each");
    uint32_t g_irq_masked = 0xffffffffu;
    // Bit set = a raise landed while the line was masked, latched one-deep and redelivered at
    // unmask.
    uint32_t g_irq_pending = 0;
    // Zero means no owner; otherwise the assigned core plus one. The kernel's
    // claim and release protocol changes this under its cross-core lock.
    uint32_t g_irq_route_plus1[IRQ_LINES] = {};
    // The lines the doorbell is carrying. A bitmap the handler drains, so every line rung
    // before a delivery is dispatched by it; arch.h's floor is one shared cell.
    alignas(64) uint32_t g_doorbell_lines[KICKOS_KERNEL_CORES] = {};

    uint32_t line_core(int line)
    {
        uint32_t const route = __atomic_load_n(&g_irq_route_plus1[line], __ATOMIC_ACQUIRE);
        if (route == 0)
        {
            return arch_cpu_id();
        }
        return route - 1u;
    }

    void ring_line(int line, uint32_t core)
    {
        __atomic_fetch_or(&g_doorbell_lines[core], 1u << line, __ATOMIC_RELEASE);
        if (core == arch_cpu_id())
        {
            kickos::x86_64::apic_doorbell();
        }
        else
        {
            kickos::x86_64::apic_doorbell_send(1u << core);
        }
    }

    // The incoming block goes to TWO places, the task-state segment and the per-core block, so
    // both are written together or one entry class loads a stale pointer. A blockless context
    // publishes ZERO: idle and the boot context are privileged, so a zero is never loaded, and
    // one that was faults immediately.
    void publish_current(struct arch_context* to)
    {
        local_state().ctx_current = to;
        kickos::x86_64::tss_set_rsp0(to->kernel_sp);
        kickos::x86_64::cpu_set_kernel_sp(to->kernel_sp);
    }

    // One pass: a line rung by a handler inside this loop rings the doorbell again, so a bit
    // set behind the index is delivered by that ring.
    void dispatch_doorbell(void)
    {
        uint32_t* const carry = &g_doorbell_lines[arch_cpu_id()];
        for (int line = 0; line < IRQ_LINES; line++)
        {
            uint32_t const bit = 1u << line;
            if ((__atomic_fetch_and(carry, ~bit, __ATOMIC_ACQ_REL) & bit) == 0)
            {
                continue;
            }
            kickos_isr_irq(line);
        }
    }
}

#if KICKOS_NUM_CORES > 1
namespace kickos::x86_64
{
uint32_t boot_apic_id(void)
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    uint32_t d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1u), "c"(0u));
    return b >> 24;
}
}
#endif

extern "C"
{

#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    uint32_t id = 0;
    __asm__ volatile("movl %%gs:%c1, %0" : "=r"(id) : "i"(KICKOS_X86_64_CPU_CORE_ID));
    return id;
}
#endif

// --- Context / switching ----------------------------------------------------
// Builds the frame kickos_x86_64_resume pops. An iretq loads no return address, so the
// thread-exit trampoline's address is placed where the entry's own `ret` will find it.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    ctx->stack_lo = reinterpret_cast<uintptr_t>(stack_base);
    ctx->stack_hi = ctx->stack_lo + stack_size;
    // ctx->kernel_sp is READ here, never written: thread_create seats the block before this
    // call and owns the zero that means none is seated.

    // SysV wants the stack 16-byte aligned at a call site, so the entry's first instruction
    // must see rsp 8 modulo 16, which the return-address slot below produces. It sits at the
    // top of the thread's OWN stack at either level, where the entry function's `ret` looks.
    uintptr_t const top = ctx->stack_hi & ~static_cast<uintptr_t>(15);
    uintptr_t const return_slot = top - 8;

    // Where this frame sits IS the privilege boundary: it carries cs, ss and rflags, so whoever
    // can write it chooses the privilege level of the one iretq that starts the thread. An
    // unprivileged thread's frame therefore goes on its KERNEL block; a privileged thread
    // resumes at ring 0 on this sp, so its frame stays on the stack it was handed.
    //
    // The iretq pops the frame and loads rsp from it, so a thread executing at ring 3 holds
    // nothing on its block (switch.S).
    uintptr_t frame_top = return_slot;
    uint64_t code_selector = kickos::x86_64::sel_kernel_code;
    uint64_t stack_selector = kickos::x86_64::sel_kernel_data;
    uint64_t returns_to = reinterpret_cast<uint64_t>(&kickos_x86_64_thread_exit);
    if (privileged == 0)
    {
        // Never zero here: the one TCB outside the pool is the privileged idle, and
        // thread_create asserts it.
        frame_top = ctx->kernel_sp;
        code_selector = kickos::x86_64::sel_user_code;
        stack_selector = kickos::x86_64::sel_user_data;
        returns_to = reinterpret_cast<uint64_t>(&kickos_user_thread_return);
    }
    *reinterpret_cast<uint64_t*>(return_slot) = returns_to;

    uintptr_t const base = frame_top - X86_64_FRAME_SIZE;
    trap_frame* const f = reinterpret_cast<trap_frame*>(base);
    uint64_t* const words = reinterpret_cast<uint64_t*>(base);
    for (size_t i = 0; i < X86_64_FRAME_SIZE / sizeof(uint64_t); i++)
    {
        words[i] = 0;
    }
    f->rdi = reinterpret_cast<uint64_t>(arg);
    f->rip = reinterpret_cast<uint64_t>(entry);
    f->cs = code_selector;
    f->ss = stack_selector;
    f->rsp = return_slot;
    // Interrupts LIVE, and this is the system's FIRST enable: arch_irq_restore clears only
    // what its own paired save set, so nothing earlier can perform it.
    f->rflags = RFLAGS_IF | RFLAGS_RESERVED_ONE;
    ctx->sp = base;
}

// Resumes the thread at `entry`, privileged, on its own KERNEL block: the death path must not
// run on memory an unprivileged thread can write.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp survives the rebuild and is put back explicitly. The stub is privileged, so
    // the rebuild below places its frame from the block it is HANDED.
    uintptr_t const kernel_sp = ctx->kernel_sp;
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
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    CpuState& cpu = local_state();
    if (cpu.isr_depth != 0)
    {
        // Deferred, which arch.h permits: the interrupted thread's state is already in the
        // frame the entry built, so the swap is two stores at the exception exit. `from` is
        // dropped here; see g_ctx_current.
        cpu.switch_to = to;
        return;
    }
    // Thread context under the kernel IrqLock, so no interrupt observes the cell between this
    // write and the frame it describes being the one on the CPU.
    publish_current(to);
    kickos_x86_64_switch_now(from, to);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    (void)boot; // abandoned, as arch.h permits
    publish_current(first);
    kickos_x86_64_start(first);

    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

// The interrupt entry alone bumps it, so it reads FALSE inside syscall dispatch as arch.h
// requires: the kernel's blocking primitives depend on that.
int arch_in_isr(void)
{
    return local_state().isr_depth != 0;
}

// --- Clocks -----------------------------------------------------------------
uint64_t arch_clock_now(void)
{
    return kickos::x86_64::clock_now();
}

void arch_timer_arm(uint64_t deadline_ns)
{
    kickos::x86_64::timer_arm(deadline_ns);
}

void arch_timer_disarm(void)
{
    kickos::x86_64::timer_disarm();
}

// The MEASURED timestamp-counter rate (apic_init), which is this processor's only reported
// core clock: no CPUID leaf on this model answers it.
uint64_t arch_cpu_clock_hz(void)
{
    return kickos::x86_64::apic_tsc_hz();
}

// --- Region descriptors: none on this arch ----------------------------------
// arch_mpu_min_region returning 0 makes arch_ram_region_size 16-byte granular. cmake/
// boot_arena.cmake SCRAPES both bodies textually, so each must stay a plain integer return.
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

// Rule 7 (arch.h): x86 has no bit-band alias.
int arch_bitband_present(void)
{
    return 0;
}

// Zero because of the KNOB: the chip selects neither region descriptors nor an address space,
// so KICKOS_MEMORY_ENFORCED is 0 and the isolation self-test this feeds is not registered.
uintptr_t arch_mpu_probe_addr(void)
{
    return 0;
}

// The permissions come from the PE32+ SECTION TABLE, walked at runtime from the base the image
// was LOADED at (ring3_x86_64.cc): the image is built -ffunction-sections -fdata-sections, so
// the writable set is forty-odd sections interleaved with read-only ones.
//
// The arena is not admitted here: a thread's own stack is a region the kernel composes
// (thread.cc), and admitting the whole conventional-memory range would hand every caller every
// other thread's stack.
bool arch_user_text_readable(uintptr_t ptr, size_t len)
{
    return kickos::x86_64::image_range_mapped(ptr, len, false);
}

bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
    return kickos::x86_64::image_range_mapped(ptr, len, true);
}

// --- Data cache -------------------------------------------------------------
// x86 keeps its caches coherent with bus masters in hardware.
void arch_dcache_flush(void const* addr, size_t bytes)
{
    (void)addr;
    (void)bytes;
}

void arch_dcache_invalidate(void* addr, size_t bytes)
{
    (void)addr;
    (void)bytes;
}

// --- Interrupt controller ---------------------------------------------------
// The software controller's mask and pending cells are atomic, so callers may update them
// without holding IrqLock. A pending raise is redelivered when unmask wins the race.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    __atomic_fetch_or(&g_irq_masked, 1u << line, __ATOMIC_SEQ_CST);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    uint32_t const bit = 1u << line;
    __atomic_fetch_and(&g_irq_masked, ~bit, __ATOMIC_SEQ_CST);
    // A raise taken while the line was masked redelivers now through the doorbell, which is
    // rung with interrupts masked and so fires at arch_irq_restore.
    if ((__atomic_fetch_and(&g_irq_pending, ~bit, __ATOMIC_SEQ_CST) & bit) != 0)
    {
        ring_line(line, line_core(line));
    }
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    __atomic_fetch_and(&g_irq_pending, ~(1u << line), __ATOMIC_SEQ_CST);
}

void arch_irq_route(int line, uint32_t core)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return;
    }
    uint32_t route = 0;
    if (core < KICKOS_KERNEL_CORES)
    {
        route = core + 1u;
    }
    __atomic_store_n(&g_irq_route_plus1[line], route, __ATOMIC_RELEASE);
}

int arch_irq_line_core(int line)
{
    if (line < 0 or line >= IRQ_LINES)
    {
        return KICKOS_IRQ_LINE_CORE_NONE;
    }
    uint32_t const route = __atomic_load_n(&g_irq_route_plus1[line], __ATOMIC_ACQUIRE);
    if (route == 0)
    {
        return KICKOS_IRQ_LINE_CORE_NONE;
    }
    return static_cast<int>(route - 1u);
}

// Test scaffolding (arch.h).
void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= IRQ_LINES)
    {
        return;
    }
    uint32_t const bit = 1u << irq;
    if ((__atomic_load_n(&g_irq_masked, __ATOMIC_SEQ_CST) & bit) != 0)
    {
        __atomic_fetch_or(&g_irq_pending, bit, __ATOMIC_SEQ_CST);
        // If unmask won the race before the latch, deliver this raise now.
        if ((__atomic_load_n(&g_irq_masked, __ATOMIC_SEQ_CST) & bit) == 0
            and (__atomic_fetch_and(&g_irq_pending, ~bit, __ATOMIC_SEQ_CST) & bit) != 0)
        {
            ring_line(irq, line_core(irq));
        }
    }
    else
    {
        ring_line(irq, line_core(irq));
    }
}

// --- Fault isolation --------------------------------------------------------
// No register records the interrupted privilege level here; the only record is the cs the
// hardware PUSHED, which is memory. The block test comes FIRST and is what makes that word
// believable, the frame of a ring 3 fault being placed by the hardware at the top of the
// running thread's own kernel block. The cs test is needed as well: a kernel bug taken during a
// syscall dispatch builds its frame on that same block, and its pushed cs names the kernel
// selector.
bool arch_fault_is_user_thread(void* frame)
{
    if (not kickos_fault_frame_on_kernel_stack(frame, sizeof(trap_frame)))
    {
        return false;
    }
    trap_frame const* const f = static_cast<trap_frame const*>(frame);
    // The vector is read ahead of the cs: a vector at or above 32 is an external delivery,
    // asynchronous to the thread it interrupted, so its pushed cs names whoever happened to be
    // running.
    if (f->vector >= 32)
    {
        return false;
    }
    return (f->cs & 3u) == 3;
}

// iretq reloads the code selector, the stack selector, the flags and the stack pointer along
// with the instruction pointer, so this redirect has to decide all five; iretq refuses a return
// whose stack selector does not carry the privilege the code selector names.
void arch_fault_redirect_to_exit(void* frame)
{
    trap_frame* const f = static_cast<trap_frame*>(frame);
    uint64_t const cr2 = read_cr2();

    // The reporter carries a single 64-bit status. The vector goes in the high half so a
    // printed status reads as vector then error.
    uint64_t const status = (f->vector << 32) | (f->error & 0xffffffffull);
    // cr2 holds the address of the last PAGE FAULT and stale contents otherwise.
    int addr_valid = 0;
    if (f->vector == 14)
    {
        addr_valid = 1;
    }
    kickos_fault_record("vec:err", status, static_cast<uintptr_t>(f->rip),
                        static_cast<uintptr_t>(cr2), addr_valid);

    f->rip = reinterpret_cast<uint64_t>(&kickos_thread_fault_exit);
    f->cs = kickos::x86_64::sel_kernel_code;
    f->ss = kickos::x86_64::sel_kernel_data;
    f->rflags = RFLAGS_IF | RFLAGS_RESERVED_ONE;
    f->rsp = kickos_fault_stack_top();
}

// --- Idle -------------------------------------------------------------------
// HLT wakes ONLY with the interrupt flag set: a HLT under a mask parks the processor until
// reset. STI leaves a one-instruction shadow in which no interrupt is taken, which is what
// makes `sti; hlt` race-free.
void arch_idle_wait(void)
{
    uint64_t flags = 0;
    __asm__ volatile("pushfq\n\tpop %0" : "=r"(flags)::"memory");
    if ((flags & RFLAGS_IF) != 0)
    {
        __asm__ volatile("hlt" ::: "memory");
        return;
    }
    __asm__ volatile("sti\n\thlt\n\tcli" ::: "memory");
}

// --- The interrupt entry's C half (trap_x86_64.S) ---------------------------
// Returns the frame to resume from, or nullptr for a vector this backend does not own.
//
// The end-of-interrupt comes FIRST, so a re-arm inside the kernel's own handler is not blocked
// by this delivery still being in service; the gate cleared the interrupt flag, so no re-entry
// can follow from it.
kickos::x86_64::trap_frame* kickos_x86_64_isr(kickos::x86_64::trap_frame* frame)
{
    CpuState& cpu = local_state();
    uint64_t const vector = frame->vector;
    if (vector == kickos::x86_64::vector_spurious)
    {
        // No end-of-interrupt is owed for the spurious vector (Intel SDM Vol 3).
        return frame;
    }
    if (vector != kickos::x86_64::vector_timer and vector != kickos::x86_64::vector_doorbell)
    {
        // The local APIC holds the in-service bit for a vector at or above 32 until the
        // end-of-interrupt register is written, and the report this returns into ends the
        // image on a polled console.
        if (vector >= 32)
        {
            kickos::x86_64::apic_eoi();
        }
        return nullptr;
    }

    kickos::x86_64::apic_eoi();
    cpu.isr_depth++;
    if (vector == kickos::x86_64::vector_timer)
    {
        kickos_isr_timer();
    }
    else
    {
#if KICKOS_KERNEL_CORES > 1
        kickos_x86_64_doorbell_service();
        if (kickos_kernel_core_resched_take() != 0)
        {
            kickos_kernel_core_resched();
        }
#endif
        dispatch_doorbell();
    }
    cpu.isr_depth--;

    if (cpu.switch_to == nullptr)
    {
        return frame;
    }
    // The interrupted frame IS the outgoing context's saved state, so publishing it is one
    // store; the incoming context's own frame is what the epilogue then pops.
    struct arch_context* const to = cpu.switch_to;
    cpu.switch_to = nullptr;
    if (cpu.ctx_current != nullptr)
    {
        cpu.ctx_current->sp = reinterpret_cast<uintptr_t>(frame);
    }
    publish_current(to);
    return reinterpret_cast<kickos::x86_64::trap_frame*>(to->sp);
}

}
