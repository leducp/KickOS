// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-generic ARM Cortex-M arch backend: the parts of the arch.h seam whose
// implementation is IDENTICAL on ARMv6-M and ARMv7-M (deferred PendSV switch,
// SysTick one-shot timer, the whole NVIC line set, the default-IRQ entry, the
// wild-PSP and no-kernel-block reports, the idle wait, and the SysTick ISR
// entry). Compiled into BOTH kickos_arch_armv6m and kickos_arch_armv7m.
//
// What differs per profile arrives through <arm_isa.h>, one copy per ISA
// directory and one on the include path per archive.
//
// The arch-profile-specific edges stay in arch_armv{6,7}m.cc: context init (the
// v7-M frame carries an EXC_RETURN word), the critical section (PRIMASK vs
// BASEPRI), the fault reporter (v6-M has no fault-status registers at all), the
// monotonic/trace clock source, and per-arch bring-up.

#include <kickos/arch/arch.h>

#include <kickos/sys/atomic.h>


#include <kickos/units.h> // _s literal (== 1e9 ns) for the ns/cycle conversions

#include <arm_isa.h> // per-profile: the panic banners and the device-priority write

#include "regs.h"
#include "mpu.h"

#include <stddef.h>

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

namespace
{
    using namespace kickos::arm;
    using namespace kickos::units; // _s == 1e9 ns

    using kickos::Atomic;
    using kickos::Order;
}

// kfault_terminate is the shared panic/fault dead-end (kernel.h).
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

extern "C"
{

    // CMSIS convention: the core clock in Hz, defined + maintained by the chip.
    extern uint32_t SystemCoreClock;

    // Shared with switch.S (the PendSV/arch_start switch targets): written by C
    // (arch_switch) and by asm (PendSV).
    Atomic<struct arch_context*, Order::RELAXED> g_arch_current = nullptr;
    Atomic<struct arch_context*, Order::RELAXED> g_arch_next = nullptr;

    // switch.S loads each as a plain word at offset 0. Nothing else enforces the layout.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");
}

// ===========================================================================
extern "C"
{

// Anything file-local below is `static`, never an anonymous namespace: C language
// linkage overrides the namespace, so an anonymous namespace nested in this block
// emits an unmangled GLOBAL symbol.

// A trap prologue refused the running thread's live SP. Contain the thread instead of ending
// the system: the kernel slays it and hands back the context to run instead, which this seats
// for the restore half the caller in switch.S branches to. Returns 1 when contained, 0 when
// the caller must terminate.
int kickos_arm_contain_wild_sp(void)
{
    // g_arch_current, not the scheduler's current: this one tracks the PHYSICAL switch, so it
    // names the thread whose pointer was refused even when a booked switch has already
    // published another one as current.
    struct arch_context* const next = kickos_thread_contain_wild_stack(g_arch_current, nullptr);
    if (next == nullptr)
    {
        return 0;
    }
    // g_arch_current is written by the restore half itself, so only the target is set here.
    g_arch_next = next;
    return 1;
}

// --- Switch: always deferred to PendSV (the outgoing ctx is g_arch_current) --
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // PendSV saves g_arch_current; `from` is always that thread
    g_arch_next = to;
    reg32(SCB_ICSR) = ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

int arch_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    // 9-bit exception-number mask (v7-M IPSR width). Correct on v6-M too: a
    // v6-M IPSR never exceeds 0x3F, so the wider mask yields the same result.
    uint32_t const exc = ipsr & 0x1FFu;
    // 11 is SVCall. This predicate is asked whether a switch requested here would DEFER,
    // and inside the syscall trap it would not: the trap runs on behalf of the thread that
    // issued the SVC, and the IPC fastpath blocks and performs the switch there. Ungated by
    // the fastpath because no backend runs any C at all in SVCall without one, so no reader
    // can observe the difference, and a predicate that answered differently per build knob
    // would be a second truth.
    if (exc == 11u)
    {
        return 0;
    }
    return exc != 0;
}

uint64_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}

// --- One-shot timer (SysTick). Clock (arch_clock_now) is a per-CHIP contract on
// every ARM arch; there is no arch-level fallback. --------------------------
// Absolute deadline the running SysTick was last programmed for (UINT64_MAX ==
// disarmed / fired). arch_timer_arm is entered on EVERY reschedule with the
// same pending deadline; blindly reloading SYST_CVR each time resets the
// countdown, so a far deadline reached only while lower-prio threads switch
// faster than it can expire (e.g. a bench reporter's 0.5 s sleep behind two
// CPU-bound players) starves forever. Guard: if the same deadline is already
// counting (SysTick still enabled), leave it running. The one-shot ISR disables
// SysTick before it re-arms, so its own re-arm (this exact deadline, remainder of
// a clamped wait) is never skipped. Touched only under the kernel IrqLock.
static uint64_t g_armed_deadline_ns = ~0ull;

void arch_timer_arm(uint64_t deadline_ns)
{
    if (deadline_ns == g_armed_deadline_ns and (reg32(SYST_CSR) & SYST_CSR_ENABLE) != 0)
    {
        return;
    }
    g_armed_deadline_ns = deadline_ns;
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    // Clamp the delta to the one-shot range BEFORE converting, so a far-future
    // (or saturated UINT64_MAX) deadline can't overflow the ns*freq product. A
    // clamped deadline fires early and the kernel re-arms the remainder from
    // kickos_isr_timer (a harmless extra wake).
    // max_delta_ns depends only on the clock; cache it so a variable 64-bit divide
    // does not run on every arm (recomputed only when the clock changes at boot).
    uint64_t f = SystemCoreClock;
    static uint64_t cached_f = 0;
    static uint64_t cached_max_delta = ~0ull;
    if (f != cached_f)
    {
        if (f != 0)
        {
            cached_max_delta = (static_cast<uint64_t>(SYST_RVR_MAX) * 1_s) / f;
        }
        else
        {
            cached_max_delta = ~0ull;
        }
        cached_f = f;
    }
    uint64_t max_delta_ns = cached_max_delta;
    uint64_t cyc;
    if (delta_ns >= max_delta_ns)
    {
        cyc = SYST_RVR_MAX;
    }
    else
    {
        cyc = (delta_ns * f) / 1_s;
    }
    if (cyc == 0)
    {
        cyc = 1; // never program 0 (fires immediately / not at all)
    }
    reg32(SYST_CSR) = 0;                        // disable while reprogramming
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;           // drop any pend latched while masked
    reg32(SYST_RVR) = static_cast<uint32_t>(cyc);
    reg32(SYST_CVR) = 0;                        // clear -> reload on next tick
    reg32(SYST_CSR) = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void arch_timer_disarm(void)
{
    g_armed_deadline_ns = ~0ull;
    reg32(SYST_CSR) = 0;
    // "disarm" must mean no callback: clear a SysTick that pended while the line
    // was masked, else it fires once on lock release after we disarmed.
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
}

// --- MPU: ARM PMSA backend (v6-M/v7-M share the register map) ----------------
#if KICKOS_HAVE_MPU
// Descriptor slots a per-thread image can occupy, above the chip's fixed rows. The
// fixed-region init bound-checks the silicon against it.
static constexpr size_t MAX_PEND_REGIONS = ARCH_MPU_ENCODED_SLOTS;

// Count of chip fixed regions occupying the LOW MPU slots [0, g_fixed_count).
// Set once by kickos_arm_mpu_fixed_init; per-thread grants are programmed ABOVE it.
// 0 for every chip without a fixed-region hook -> those chips are byte-identical.
static size_t g_fixed_count = 0;

// The MPU hardware-programming step (disable / reprogram descriptors / re-enable).
// Split out of arch_mpu_apply so the PendSV epilogue can run it atomically with the
// physical context switch. An eager apply reprograms the MPU for the incoming thread
// while the outgoing thread is still running (PendSV not fired yet), faulting it on
// its own stack.
//
// PMSAv7 only: a chip whose MPU is the crossbar SYSMPU or a v8-M PMSAv8 programs a
// different descriptor from its own commit, and its image has different words.
#if KICKOS_ARM_MPU == KICKOS_ARM_MPU_PMSAV7
extern "C" void kickos_arm_mpu_program(struct arch_mpu_encoded const* img)
{
    using namespace kickos::arm;
    if (img == nullptr)
    {
        return;
    }
    // MEMFAULTENA and BUSFAULTENA keep an isolation violation and a bus abort as MemManage and
    // BusFault instead of letting either escalate to HardFault.
    //
    // MPU_CTRL IS DELIBERATELY NOT ZEROED HERE. Disabling the MPU also stops the CHIP FIXED
    // rows applying, and on imxrt1062 those carry the ERR011573 anti-speculation wrap over
    // the FlexSPI band this code is itself executing from. Each descriptor is instead
    // disabled individually just before it is rewritten, which leaves [0, k) in force
    // throughout.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA;
    __asm volatile("dmb" ::: "memory");
    // Chip fixed regions own the LOW slots [0, k), programmed once by
    // kickos_arm_mpu_fixed_init and NEVER touched here. Per-thread grants go in
    // [k, hw), so a grant sits ABOVE the fixed background and correctly overrides it
    // (PMSAv7: highest-numbered region wins). k == 0 on every chip without a fixed hook.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    size_t const k = g_fixed_count;
    for (size_t i = k; i < hw_regions; i++)
    {
        size_t const j = i - k; // per-thread region index
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        // Disable THIS descriptor before its base moves, or it would briefly pair the new
        // base with the old size and attributes.
        reg32(MPU_RASR) = 0;
        if (j < ARCH_MPU_ENCODED_SLOTS)
        {
            reg32(MPU_RBAR) = img->rbar[j];
            reg32(MPU_RASR) = img->rasr[j];
        }
        // No else: a slot past the image keeps the RASR = 0 written above.
    }
    __asm volatile("dsb" ::: "memory");
    // Do not drop this because kickos_arm_mpu_fixed_init also enables the MPU: only imxrt1062
    // calls that, so on every other PMSAv7 chip this is the ONLY write that enables it. It
    // enables, so unlike a leading MPU_CTRL = 0 it cannot stop the chip fixed rows applying.
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

#endif // KICKOS_ARM_MPU_PMSAV7

// One-time: program the chip's fixed regions into the LOW slots [0, k), cache k, and
// enable the MPU (with the PRIVDEFENA background). Call from the chip arch_init BEFORE
// enabling caches and before the scheduler starts. Idempotent-safe to call once.
void kickos_arm_mpu_fixed_init(void)
{
    using namespace kickos::arm;
    struct kickos_arm_mpu_fixed_region const* fixed = nullptr;
    size_t const k = kickos_arm_mpu_fixed(&fixed);
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    // The fixed set plus a full per-thread set must fit the hardware descriptors, or a
    // per-thread grant would silently fall off the top. Fail loud (a chip-config bug
    // caught at boot), never truncate. No kernel assert on the arch path -> spin.
    if (k + MAX_PEND_REGIONS > hw_regions)
    {
        while (true)
        {
            __asm volatile("wfi");
        }
    }
    // Zeroing MPU_CTRL is correct HERE and only here: this runs at boot, before the caches and
    // before any thread, so there is no fixed row yet to lose.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA | SHCSR_BUSFAULTENA;
    reg32(MPU_CTRL) = 0;
    __asm volatile("dsb" ::: "memory");
    for (size_t i = 0; i < k; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        reg32(MPU_RBAR) = fixed[i].base & ~0x1Fu;
        reg32(MPU_RASR) = fixed[i].rasr;
    }
    g_fixed_count = k;
    __asm volatile("dsb" ::: "memory");
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

// --- Deferred MPU-commit seam (shared across every ARM backend) --------------- The
// switch is a PENDED PendSV on every ARM arch: arch_mpu_apply runs on the OUTGOING
// thread, but the physical register/PSP swap only happens later in PendSV. Programming
// the hardware eagerly would run the outgoing thread under the INCOMING thread's
// regions until PendSV fires -> a fault on its own stack (proven on RP2040). So
// arch_mpu_apply only STASHES the region set here; kickos_arch_mpu_commit programs the
// hardware AFTER the physical swap.
//
// A POINTER into the incoming thread's TCB, not a copy. The image is re-encoded by its
// owner alone, and that owner is the thread the pended switch will land on, so a
// re-encode between the stash and the commit programs the set that thread actually has.
// Thread slots come from a static pool and are never returned to an allocator, so a
// pointer left over from an earlier switch still addresses valid storage; two switch_book
// calls under one lock leave the second, which is the thread the switch lands on.
static struct arch_mpu_encoded const* g_pend_image = nullptr;

// Read the pending stash. Lets a chip whose MPU is NOT PMSAv7 (K64F SYSMPU, PMSAv8)
// program its own hardware from the SAME stash by defining only the commit.
struct arch_mpu_encoded const* kickos_arm_mpu_pending(void)
{
    return g_pend_image;
}

// STASH-ONLY apply: record the incoming image, no hardware write. Shared by every ARM
// backend, PMSAv7 (v6-M/v7-M) and K64F SYSMPU and PMSAv8 alike; a chip replaces only the
// commit, never this, so this definition is not overridable.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    g_pend_image = image;
}

#else
// No enforcement on this board (KICKOS_HAVE_MPU=0): privilege + SVC only. The stash has
// no reader, but the apply symbol must still resolve.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}
#endif

// --- Interrupt controller (NVIC) --------------------------------------------
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    reg32(NVIC_ICER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // AHEAD OF THE ENABLE, never after: on a profile that bands device lines, a line enabled
    // at the controller's reset priority preempts an IrqLock-held section until this lands.
    kickos_arm_irq_line_prio(l);
    // A pending bit latched while the line was masked survives the enable and fires the
    // instant ISER is set. The dsb drains a preceding device-flag clear, whose W1C may
    // still sit in the write buffer (exception entry does not order device writes), so a
    // level source that is genuinely deasserted does not re-latch.
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ISER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Drain any pending device write before dropping the latched NVIC pending.
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ICPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Latch-and-coalesce: the NVIC holds ISPR pending independently of ISER, so a
    // raise on a masked (disabled) line latches and fires the instant the line is
    // enabled: write ISPR unconditionally, do not drop.
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Kernel-facing ISR entries ----------------------------------------------
// SysTick expiry: disarm (one-shot tickless model) then run the kernel handler,
// which re-arms the next deadline via arch_timer_arm.
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

// The exception number in IPSR is 16 + the external line. The mask is the 9-bit v7-M IPSR
// width and is correct on v6-M too, whose IPSR never exceeds 0x3F (arch_in_isr above reads
// it the same way).
void kickos_arm_default_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    int line = static_cast<int>(ipsr & 0x1FFu) - 16;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// --- Refusals raised by switch.S --------------------------------------------
// From switch.S (PendSV and the SVC entry), when the running thread's live PSP lacks room
// BELOW it for the callee block about to be pushed there. Runs in handler mode on the MSP.
//
// RETURNS TO RESUME ANOTHER THREAD when the offending one could be contained; only the thread
// that chose the pointer dies. CONTAINS BEFORE REPORTING because kpanic_enter is one-way: it
// masks this core's IRQs and never restores them, so a system resumed after it has no timer.
void kickos_arm_bad_psp(uint32_t psp, uint32_t need, uint32_t lo, uint32_t hi)
{
#if KICKOS_KERNEL_STACKS
    int const contained = kickos_arm_contain_wild_sp();
#else
    // NO BLOCK TO REBUILD ON, so this board cannot contain: arch_ctx_redirect falls back to
    // fabricating the privileged exit frame on the thread's OWN USER STACK, which is the
    // write the guard above exists to refuse. A whole-board property (armv7m is the one arch
    // that resolves this knob to 0, on f302nucleo and bluepill-c8), so it is decided here and
    // not per thread, and the CONTAINED spelling below is compiled out with it.
    int const contained = 0;
#endif
    if (contained == 0)
    {
        kpanic_enter();
    }
#if KICKOS_PANIC_DUMP
    // Re-derived rather than passed: one guard serves all three legs.
    // THE NOUN IS THE DISCRIMINATOR. tests/lib/panic.ere matches "=== <ARCH> EXCEPTION", so a
    // contained refusal spelled that way is indistinguishable from one that ended the system
    // and assert_no_panic stops meaning anything on exactly the arms that now survive.
    char const* why = "no room below";
    if (psp < lo)
    {
        why = "under stack_lo";
    }
    else if (psp >= hi)
    {
        why = "at or above stack_hi";
    }
    // Which guarded push refused: nothing in the arguments separates the sites, so it comes
    // from ICSR.VECTACTIVE.
    uint32_t const vect = reg32(SCB_ICSR) & 0x1FFu;
    char const* site = "handler";
    if (vect == 11u)
    {
        site = "SVCall";
    }
    else if (vect == 14u)
    {
        site = "PendSV";
    }
#if KICKOS_KERNEL_STACKS
    if (contained == 0)
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY, why);
    }
    else
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY_CONTAINED, why);
    }
#else
    ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY, why);
#endif
    ::kickos::kprintf("  in %s PSP=0x%x need=%u stack=[0x%x,0x%x)\n", site,
                      static_cast<unsigned>(psp), static_cast<unsigned>(need),
                      static_cast<unsigned>(lo), static_cast<unsigned>(hi));
#else
    (void)psp;
    (void)need;
    (void)lo;
    (void)hi;
#if KICKOS_KERNEL_STACKS
    if (contained == 0)
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP);
    }
    else
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_CONTAINED);
    }
#else
    ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP);
#endif
#endif
    if (contained == 0)
    {
        kfault_terminate();
    }
}

#if KICKOS_KERNEL_STACKS
// From svc_trampoline, when the calling thread has no kernel block seated. Runs privileged
// in THREAD mode ON THE MSP, .Lsvc_nokstack having cleared CONTROL.SPSEL before the branch;
// `psp` is the thread's own, computed before that clear.
void kickos_arm_no_kernel_stack(uint32_t psp)
{
    kpanic_enter();
    ::kickos::kprintf(KICKOS_ARM_BANNER_NO_KERNEL_STACK);
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  in svc_trampoline PSP=0x%x\n", static_cast<unsigned>(psp));
#else
    (void)psp;
#endif
    kfault_terminate();
}
#endif

}
