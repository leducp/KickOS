// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-generic ARM Cortex-M arch backend: the parts of the arch.h seam whose
// implementation is IDENTICAL on ARMv6-M and ARMv7-M (deferred PendSV switch,
// SysTick one-shot timer, NVIC mask/unmask/inject, the idle wait, and the
// SysTick ISR entry). Compiled into BOTH kickos_arch_armv6m
// and kickos_arch_armv7m.
//
// The arch-profile-specific edges stay in arch_armv{6,7}m.cc: context init (the
// v7-M frame carries an EXC_RETURN word), the critical section (PRIMASK vs
// BASEPRI), NVIC priority programming, the monotonic/trace clock source, and
// per-arch bring-up + the distinctly-named default-IRQ entry.

#include <kickos/arch/arch.h>

#include <kickos/units.h> // _s literal (== 1e9 ns) for the ns/cycle conversions

#include "regs.h"
#include "mpu.h"

#include <stddef.h>

namespace
{
    using namespace kickos::arm;
    using namespace kickos::units; // _s == 1e9 ns

}

extern "C"
{

    // CMSIS convention: the core clock in Hz, defined + maintained by the chip.
    extern uint32_t SystemCoreClock;

    // Shared with switch.S (the PendSV/arch_start switch targets). volatile:
    // written by C (arch_switch) and by asm (PendSV); the compiler must not
    // cache or elide the accesses it can't see across the asm seam.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;
}

// ===========================================================================
extern "C"
{

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
    return (ipsr & 0x1FF) != 0;
}

uint32_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}

// --- One-shot timer (SysTick). Clock (arch_clock_now) is per-arch: chip-
// provided on v6-M (no DWT), a weak DWT default on v7-M. -----------------------
// Absolute deadline the running SysTick was last programmed for (UINT64_MAX ==
// disarmed / fired). ktime_rearm() calls arch_timer_arm on EVERY reschedule with
// the same pending deadline; blindly reloading SYST_CVR each time resets the
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
namespace
{
    // Max per-thread regions the deferred-commit stash carries (must be >= the kernel's
    // KICKOS_MPU_MAX_REGIONS; both are 8). Hoisted here so the fixed-region init can
    // bound-check against it. Sizes g_pend_regions below.
    constexpr size_t kMaxPendRegions = 8; // == KICKOS_MPU_MAX_REGIONS (kernel config)

    // Count of chip fixed regions occupying the LOW MPU slots [0, g_fixed_count).
    // Set once by kickos_arm_mpu_fixed_init; per-thread grants are programmed ABOVE it.
    // 0 for every chip without a fixed-region hook -> those chips are byte-identical.
    size_t g_fixed_count = 0;

    // Encode one region into an MPU_RASR value. attr is the UNPRIVILEGED access
    // (supervisor comes from the PRIVDEFENA background region): a code region is
    // R+X (RO, executable); data / stack / device is RW + execute-never. Device
    // memory gets the ordered device type. `size` is a power of two >= 32, and the
    // region base is naturally aligned to it (arch_ram_region_size / the linker).
    uint32_t mpu_rasr(size_t size, uint32_t attr)
    {
        using namespace kickos::arm;
        uint32_t const size_field =
            static_cast<uint32_t>(__builtin_ctz(static_cast<unsigned>(size))) - 1u;
        uint32_t rasr = MPU_RASR_ENABLE | (size_field << 1);
        if (attr & ARCH_MPU_X)
        {
            rasr |= MPU_RASR_AP_RO | MPU_RASR_MEM_NORMAL; // code: RO + executable
        }
        else if (attr & ARCH_MPU_DEV)
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_DEVICE; // MMIO
        }
        else
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_NORMAL; // data/stack
        }
        return rasr;
    }
}

// The MPU hardware-programming step (disable / reprogram descriptors / re-enable).
// Split out of arch_mpu_apply so a DEFERRED-switch arch (armv6m: the switch is a
// pended PendSV) can invoke it from the PendSV switch epilogue -- i.e. atomically
// with the PHYSICAL context switch -- instead of eagerly from switch_to. Eager apply
// on a deferred switch reprograms the MPU for the INCOMING thread while the OUTGOING
// thread is still physically running (PendSV not fired yet), so the outgoing thread
// executes with the incoming thread's shrunk region set and faults on its own stack.
// v7-M keeps the eager path (arch_mpu_apply below) unchanged.
extern "C" void kickos_arm_mpu_program(struct arch_mpu_region const* regions, size_t n)
{
    using namespace kickos::arm;
    // A domain switch / privilege change can only take effect atomically, so
    // disable the MPU while reprogramming, then re-enable with the PRIVDEFENA
    // background (privileged code keeps the default map; unprivileged code sees
    // only the programmed regions). MEMFAULTENA makes a violation a clean
    // MemManage rather than an escalated HardFault.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA;
    reg32(MPU_CTRL) = 0;
    __asm volatile("dsb" ::: "memory");
    // Chip fixed regions own the LOW slots [0, k) -- programmed once by
    // kickos_arm_mpu_fixed_init and NEVER touched here (disabling the MPU above does
    // not clear descriptors, so they survive the reprogram). Per-thread grants go in
    // [k, hw), so a grant sits ABOVE the fixed background and correctly overrides it
    // (PMSAv7: highest-numbered region wins). k == 0 on every chip without a fixed
    // hook, making the emitted sequence byte-identical to the pre-seam behavior.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    size_t const k = g_fixed_count;
    for (size_t i = k; i < hw_regions; i++)
    {
        size_t const j = i - k; // per-thread region index
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        // PMSA requires a power-of-two size with a naturally-aligned base. Encode
        // ONLY such regions; a non-pow2 region is fail-closed (descriptor disabled),
        // never silently mis-encoded (ctz would under-size it and the base would
        // snap). Unprivileged regions are pow2 by construction (arch_ram_region_*
        // + the pow2 linker code/data sections); a privileged thread's non-pow2
        // whole-arena grant is simply dropped here -- harmless, it runs on the
        // PRIVDEFENA background. (Linker contract: code/data regions must be pow2.)
        if (j < n and regions[j].size >= 32
            and (regions[j].size & (regions[j].size - 1)) == 0)
        {
            reg32(MPU_RBAR) = static_cast<uint32_t>(regions[j].base) & ~0x1Fu;
            reg32(MPU_RASR) = mpu_rasr(regions[j].size, regions[j].attr);
        }
        else
        {
            reg32(MPU_RASR) = 0; // unused / non-pow2: disable the descriptor
        }
    }
    __asm volatile("dsb" ::: "memory");
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

// Weak default: no chip fixed regions. A chip (i.MX RT1062) strong-overrides this.
size_t __attribute__((weak)) kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out)
{
    (void)out;
    return 0;
}

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
    if (k + kMaxPendRegions > hw_regions)
    {
        for (;;)
        {
            __asm volatile("wfi");
        }
    }
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA;
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

// --- Deferred MPU-commit seam (shared across every ARM backend) ---------------
// The switch is a PENDED PendSV on every ARM arch: switch_to() calls arch_mpu_apply
// on the OUTGOING thread, but the physical register/PSP swap only happens later in
// PendSV. Programming the hardware eagerly would run the outgoing thread under the
// INCOMING thread's regions until PendSV fires -> a fault on its own stack (proven on
// RP2040; docs/design-mpu-commit-deferred.md). So arch_mpu_apply only STASHES the
// region set here; kickos_arch_mpu_commit programs the hardware, called from each
// deferred arch's PendSV epilogue AFTER the physical swap. A private copy (not a
// pointer) means the commit never chases a TCB whose region set changed after the stash.
namespace
{
    arch_mpu_region g_pend_regions[kMaxPendRegions]; // kMaxPendRegions hoisted above
    size_t g_pend_count = 0;
}

// Read the pending stash. Lets a chip whose MPU is NOT PMSAv7 (K64F SYSMPU) program
// its own hardware from the SAME stash by strong-overriding only the commit below.
size_t kickos_arm_mpu_pending(struct arch_mpu_region const** out)
{
    *out = g_pend_regions;
    return g_pend_count;
}

// STASH-ONLY apply (weak): record the incoming set, no hardware write. Shared by
// every ARM backend -- PMSAv7 (v6-M/v7-M) and K64F SYSMPU alike; a chip overrides
// only the commit, never this.
void __attribute__((weak)) arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    if (n > kMaxPendRegions)
    {
        n = kMaxPendRegions;
    }
    for (size_t i = 0; i < n; i++)
    {
        g_pend_regions[i] = regions[i];
    }
    g_pend_count = n;
}

// Commit the stash to the PMSAv7 hardware (weak default: F411/XMC on v7-M, RP2040/
// microbit on v6-M). cpsid brackets the disable/reprogram/re-enable so a preempting
// IRQ cannot observe a half-programmed MPU -- valid asm on both v6-M and v7-M. A chip
// with a different MPU (K64F SYSMPU) strong-overrides this; the arch's switch.S calls
// it by this fixed name after the physical swap.
void __attribute__((weak)) kickos_arch_mpu_commit(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    kickos_arm_mpu_program(g_pend_regions, g_pend_count);
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}
#else
// No enforcement on this board (KICKOS_HAVE_MPU=0): privilege + SVC only. apply is a
// no-op; the commit is an empty weak default (each deferred arch's PendSV epilogue
// calls the commit symbol unconditionally, so it must always resolve).
void __attribute__((weak)) arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
}
void __attribute__((weak)) kickos_arch_mpu_commit(void) {}
#endif

size_t __attribute__((weak)) arch_mpu_min_region(void)
{
    return 32u; // ARMv6-M / v7-M PMSA min region; a no-MPU chip (nRF51) overrides to 0
}

// PMSA needs a power-of-two size >= 32 with the base naturally aligned to it (the
// RBAR base masking in arch_mpu_apply assumes exactly that). K64F (SYSMPU, byte-
// granular) overrides this strongly; a no-MPU ARM chip (min 0) falls to the
// 16-byte-granular branch.
bool __attribute__((weak)) arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size == 0)
    {
        return false;
    }
    size_t const min = arch_mpu_min_region();
    if (min == 0)
    {
        return (base & 15u) == 0 and (size & 15u) == 0;
    }
    if (size < min or (size & (size - 1)) != 0)
    {
        return false;
    }
    return (base & (size - 1)) == 0;
}

// Rule 7 bit-band flag (arch.h): 0 by default -- M0+/M7 have no bit-band alias. The
// bit-band M4 chips (mk64f, stm32f411, xmc4800) strong-override to 1 so the grant
// path also refuses a reserved block's alias image. Defined unconditionally (unused
// where the grant module is not linked, i.e. KICKOS_HAVE_MPU=0).
int __attribute__((weak)) arch_bitband_present(void)
{
    return 0;
}


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

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Latch-and-coalesce: the NVIC holds ISPR pending independently of ISER, so a
    // raise on a masked (disabled) line latches and fires the instant the line is
    // enabled -- write ISPR unconditionally, do not drop.
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Idle / halt ------------------------------------------------------------
// weak: real silicon halts (WFI) to save power. A QEMU semihosting-clock chip
// (mps2, nrf51) overrides this to SPIN: QEMU <= 10 freezes the semihosting
// SYS_CLOCK -- our monotonic clock there -- while the core is in WFI, so a timed
// sleep with every thread idle would never wake (the clock stops). QEMU 11 fixed
// it; spinning keeps the clock advancing on the older QEMU the CI runner ships.
void __attribute__((weak)) arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Kernel-facing ISR entries ----------------------------------------------
// SysTick expiry: disarm (one-shot tickless model) then run the kernel handler,
// which re-arms the next deadline via arch_timer_arm.
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

}
