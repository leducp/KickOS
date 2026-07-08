// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV32IMAC arch backend: the ISA-generic half of the arch.h seam. The
// context switch + trap entry + syscall trampoline + first-thread entry assembly
// lives in switch.S; the chip layer (arch/riscv/chip/{virt,esp32c6}) supplies the
// hardware edges -- arch_init, arch_console_write, arch_shutdown, the clock/timer
// (arch_clock_now / arch_timer_arm / arch_timer_disarm), the CLINT base
// (g_clint_msip, for the deferred-switch software interrupt), and the linker
// script + startup vectors.
//
// Model (matches the RX72M single-frame SWINT port, adapted to RISC-V):
//   * ONE deferred switcher. arch_switch records g_arch_next and pends the machine
//     software interrupt (CLINT msip); the physical swap always happens in the
//     msip trap (switch.S), so there is ONE saved-frame format for both a voluntary
//     block (thread context) and a preemptive wake (ISR context). Held off while an
//     IrqLock masks mstatus.MIE -- the PendSV/SWINT deferral.
//   * ecall syscall trap -> a trampoline running M-mode (privileged) on the calling
//     thread's own stack (switch.S svc_trampoline), so a blocking syscall blocks by
//     an ordinary arch_switch whose continuation is per-thread (arch.h contract).
//   * mstatus.MIE critical section; g_isr_depth the IPSR!=0 analog; wfi idle.
//   * PMP is real on this core but MPU enforcement is M2 (like the ARM ports); no-op
//     here. RV32IMAC has no F/D extension -> soft-float, so the switch banks no FP.

#include <kickos/arch/arch.h>

#include <stdint.h>

// switch.S hard-codes the save-frame layout AND ctx.sp @0 / trace_tid @4. The frame
// (on the thread's own stack, ctx.sp = its base, low->high) is 32 words / 128 bytes:
//   [0 mepc][1 mstatus][2 ra][3 t0][4 t1][5 t2][6 s0][7 s1][8 a0]..[15 a7]
//   [16 s2]..[25 s11][26 t3][27 t4][28 t5][29 t6][30,31 pad]
// gp/tp are NOT saved: they are link-time-constant across all threads (set once in
// _start), so the switch leaves them untouched. A silent reorder here or in
// arch_context_init would corrupt the switch.
namespace
{
    enum : uint32_t
    {
        F_MEPC = 0, F_MSTATUS = 1, F_RA = 2, F_A0 = 8, // word indices
        FRAME_WORDS = 32
    };

    // mstatus bits (RISC-V Privileged ISA v1.10).
    constexpr uint32_t MSTATUS_MIE  = 1u << 3;
    constexpr uint32_t MSTATUS_MPIE = 1u << 7;
    constexpr uint32_t MSTATUS_MPP_M = 3u << 11; // MPP = machine (U = 0)

    // User-RAM bump allocator (chip linker script defines the bounds).
    volatile uint32_t g_ram_used = 0;
}

extern "C"
{
    // Shared with switch.S. volatile: written by C and by asm across a seam the
    // compiler cannot see. No leading underscore (RISC-V ELF symbol convention).
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;

    // In-ISR depth (the IPSR!=0 analog): bumped by the timer/external trap paths
    // (switch.S), NOT by the ecall or msip-switch paths -- so arch_in_isr() reads
    // false throughout syscall_dispatch (arch.h contract).
    volatile uint32_t g_isr_depth = 0;

    // CLINT machine-software-interrupt-pending register for this hart, set by the
    // chip's arch_init. arch_switch writes 1 to pend the deferred switch; the msip
    // switcher (switch.S) writes 0 to clear it. Chip-provided because the CLINT base
    // differs per chip (qemu-virt 0x0200_0000; the C6 exposes its own).
    volatile uint32_t* g_clint_msip = nullptr;

    // switch.S entry points + the kernel/user thread-return trampolines.
    void trap_entry(void);
    void kickos_rv_mtvec(void); // the vectored mtvec table (switch.S)
    void kickos_thread_return(void);
    void kickos_user_thread_return(void);

    // Linker-provided user-RAM bounds (chip linker script).
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];
}

// ===========================================================================
extern "C"
{

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
    ctx->sp = reinterpret_cast<uint32_t>(f);
}

// --- Switch: record the target + pend the msip switcher (never swaps inline) --
// Always deferred (the RX SWINT / ARM PendSV model): the physical swap happens in
// the msip trap. Called under the kernel IrqLock (mstatus.MIE=0), so the pended
// msip fires only once the lock releases (thread context) or the current trap
// returns (ISR context). No in-ISR branch is needed -- deferral is inherent.
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
    // csrs only SETS bits: state is 0 or MSTATUS_MIE, so this re-enables MIE iff it
    // was enabled at the paired save (and is a no-op otherwise) -- nesting-safe.
    __asm volatile("csrs mstatus, %0" ::"r"(state) : "memory");
}

int arch_in_isr(void)
{
    return g_isr_depth != 0;
}

// --- Trace clock: the cycle CSR (rdcycle), always present, 32-bit raw ---------
// The ideal cycle-accurate trace source: reads its own low 32 bits (wraps; the
// host reconstructs absolute time from the SESSION clock_hz anchors). No ns
// conversion, no wrap-extend, no crit section -- safe on the switch path.
// KICKOS_HAVE_TRACE_CLOCK is set for rv32imac. (mcounteren.CY is enabled in
// kickos_rv32_init so a U-mode thread can read it too.)
uint32_t arch_trace_now(void)
{
    uint32_t v;
    __asm volatile("rdcycle %0" : "=r"(v));
    return v;
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

// --- MPU: PMP exists on this core but enforcement is M2 (matches the ARM ports)
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
    // M2: program pmpaddr/pmpcfg on switch-in. No enforcement now -- privilege +
    // syscall only, exactly the ARM/RX M1 posture.
}

uintptr_t arch_ram_base(void)
{
    return reinterpret_cast<uintptr_t>(__kickos_ram_start);
}

size_t arch_ram_size(void)
{
    return static_cast<size_t>(__kickos_ram_end - __kickos_ram_start);
}

void* arch_ram_alloc(size_t size)
{
    size_t total = arch_ram_size();
    size_t need = (size + 15u) & ~static_cast<size_t>(15u); // 16-byte aligned
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    if (need != 0 and need <= total - g_ram_used)
    {
        p = __kickos_ram_start + g_ram_used;
        g_ram_used = g_ram_used + static_cast<uint32_t>(need);
    }
    arch_irq_restore(s);
    return p;
}

uintptr_t arch_mpu_probe_addr(void)
{
    return 0; // no enforced MPU on M1.x -> no probe address (real on M2)
}

// --- Interrupt controller (software-injected test scaffolding) ---------------
// arch_irq_inject fakes a device firing -- test/bench scaffolding (arch.h). RISC-V
// gives NO way to software-raise a real external (PLIC) interrupt: the PLIC, like
// the Cortex-A GIC, has no software-generated-interrupt, and its pending bits are
// driven by hardware gateways (QEMU models this faithfully -- a software pending-
// write does not stick). So injection uses the SUPERVISOR SOFTWARE interrupt
// (mip.SSIP, a software-writable bit, mcause=1) as a private channel -- the exact
// analog of the host sim's raise(SIGUSR1). Masking is a software bitmask (the sim's
// irq_masked twin); a raise on a masked line is dropped. A real device-interrupt
// receive path (a PLIC over meip) is a separate, driver-era concern (none yet).
//
// SSIP needs S-mode, present on the QEMU virt CPU (the run target). The ESP32-C6 is
// M/U-only (no SSIP); when it is brought up on silicon, inject routes to an
// interrupt-matrix "from-CPU" line instead -- the C6 is build-only now and runs no
// injected-IRQ tests, so the SSIP path is exercised only where it exists.
namespace
{
    constexpr uint32_t MIP_SSIP = 1u << 1;
    volatile uint32_t g_irq_masked = 0;  // bit set = line masked (default: unmasked)
    volatile int g_inject_line = -1;     // the pending software-injected line
}

void arch_irq_mask(int line)
{
    if (line < 0 or line >= 32)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked = g_irq_masked | (1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= 32)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked = g_irq_masked & ~(1u << line);
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= 32)
    {
        return;
    }
    if ((g_irq_masked & (1u << irq)) != 0)
    {
        return; // masked line: drop the raise (matches the sim/ARM/RX semantics)
    }
    g_inject_line = irq; // set BEFORE pending, so the trap sees it
    __asm volatile("csrs mip, %0" ::"r"(MIP_SSIP) : "memory");
}

// SSIP dispatch (switch.S .Lssoft), ISR context. Clear the software interrupt, then
// run the injected line's first-level ISR (kickos_isr_irq masks the line + wakes its
// driver -- kernel/irq/irq.cc); the driver re-unmasks via irq_ack.
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

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Unhandled trap (switch.S .Lfault): a fault is a genuine bug at M1 --------
// Exit loudly with a distinct status so a QEMU/CTest run reports it rather than
// hanging. arch_shutdown is chip-provided (semihosting exit on virt).
void kickos_rv_unhandled_trap(void)
{
    arch_shutdown(139);
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
// The chip has already set g_clint_msip (+ its timer base). Install the single
// trap vector, enable the software (switch) + timer local interrupts, allow U-mode
// to read the cycle/time counters, and reset the software ISR-depth state.
void kickos_rv32_init(void)
{
    g_isr_depth = 0;

    // mtvec: VECTORED mode (low 2 bits = 01). Point at the 256B-aligned vector
    // table (switch.S); every slot jumps to trap_entry, which demuxes on mcause.
    // The ESP32-C6 core supports ONLY vectored mtvec; QEMU virt supports it too, so
    // the arch is uniform. (void(*)() decays to the table base address.)
    uintptr_t tv = reinterpret_cast<uintptr_t>(kickos_rv_mtvec) | 1u;
    __asm volatile("csrw mtvec, %0" ::"r"(tv) : "memory");
    (void)trap_entry; // referenced by the asm vector table, not directly here

    // Enable the machine software (msip, bit 3 = the deferred switch), machine timer
    // (mtip, bit 7 = the tickless clock), and supervisor software (ssip, bit 1 = the
    // injected-IRQ test channel) local interrupts. ssip only fires via arch_irq_inject
    // (and only where S-mode exists, e.g. qemu-virt); on an M/U-only core the bit is
    // read-only-zero, so enabling it is harmless.
    uint32_t mie = (1u << 3) | (1u << 7) | (1u << 1);
    __asm volatile("csrw mie, %0" ::"r"(mie) : "memory");

    // Let U-mode threads read cycle/time/instret (rdcycle in arch_trace_now, and a
    // userspace clock read) instead of trapping. mcounteren bits CY|TM|IR.
    __asm volatile("csrw mcounteren, %0" ::"r"(0x7u) : "memory");

    // Permissive bootstrap PMP: ONE NAPOT entry covering the whole address space,
    // R+W+X, U-accessible. RISC-V is fail-CLOSED -- once PMP is implemented (it is on
    // this core), a U-mode access with NO matching entry FAULTS (unlike ARM, where
    // unprivileged is unrestricted until the MPU clamps it). So an unprivileged
    // thread can't even fetch its first instruction without this. It gives U-mode
    // full access (no isolation) -- exactly the ARM/RX M1 posture; M2's arch_mpu_apply
    // refines per-thread PMP. pmpaddr0 all-ones = NAPOT match-everything; pmpcfg0
    // byte0 = A=NAPOT(0x18) | X(0x4) | W(0x2) | R(0x1) = 0x1F.
    __asm volatile("csrw pmpaddr0, %0" ::"r"(0xFFFFFFFFu) : "memory");
    __asm volatile("csrw pmpcfg0, %0" ::"r"(0x1Fu) : "memory");
}

}
