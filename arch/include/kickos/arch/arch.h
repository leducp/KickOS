// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// THE KickOS porting interface. Every target implements exactly this seam.
//
// ISA-neutral by design: it names *concepts* (switch, crit-section, timer,
// mpu, syscall), never *mechanisms*. PendSV/SVC/BASEPRI live inside arch/arm,
// never here. Litmus test: a non-ARM port (Renesas RX72M) must fit this seam
// with no signature changes.

#ifndef KICKOS_ARCH_ARCH_H
#define KICKOS_ARCH_ARCH_H

#include <stddef.h>
#include <stdint.h>

// Supplies KICKOS_NUM_CORES; a standalone TU has no board config and falls back to 1.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

#ifndef KICKOS_NUM_CORES
#define KICKOS_NUM_CORES 1
#endif

// Per-arch definition of `struct arch_context` (opaque to the kernel; sized by
// the arch). Resolved to arch/<arch>/include/kickos/arch/context.h.
#include <kickos/arch/context.h>

// C++ ONLY: the extern "C" below is deliberately UNGUARDED, so a C includer breaks here.
extern "C"
{

// --- One-time backend bring-up ---------------------------------------------
// sim: install signal handlers, create the interval timer, map the RAM arena.
void arch_init(void);

// C-runtime data init driven by the linker's copy/zero range tables (init .data,
// zero .bss, for as many ranges as the chip declares, e.g. a separate pow2 app-data
// block under MPU enforcement). Called from a chip's Reset_Handler before the
// static ctors and arch_init; a chip whose linker script emits no tables need not
// call it. Runs before any global is live, so it must touch none of its own.
void kickos_ranges_init(void);

// Terminate the whole system with the given process/exit status. On the sim
// this ends the host process; on MCUs it halts.
void arch_shutdown(int status) __attribute__((noreturn));

// Reboot into the chip's bootloader (firmware-download mode). NOT noreturn: a chip
// with no such entry declines with -KOS_ENOSYS instead. Success never returns. The
// backend masks interrupts itself before handing over.
int arch_reboot(void);

// --- Core identity ----------------------------------------------------------
// The 0-based index of the core executing this code, in [0, KICKOS_NUM_CORES).
//
// At one core a MACRO, not an inline the optimiser folds: -Os has been measured
// out-lining an always_inline candidate in system/include/kickos/sys/atomic.h.
// The multi-core arm has NO fallback TU, so a port that raises KICKOS_NUM_CORES and
// ships no definition is a LINK error rather than a kernel that believes it is on core 0.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void);
#else
#define arch_cpu_id() 0u
#endif

// --- Context / switching ---------------------------------------------------
// Build an initial frame in `ctx` so the first switch-in "returns" into
// entry(arg) on [stack_base, stack_base+stack_size). `privileged` selects the
// kernel (privileged) vs user (unprivileged) posture. When `entry` returns the
// arch calls back into kernel thread-exit (kickos_thread_return()).
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged);

// Discard every frame `ctx` holds and rebuild it so the thread resumes at `entry`,
// PRIVILEGED, in thread mode, at the top of [stack_base, stack_base + stack_size).
// `ctx` must NOT be the running context: this writes a saved context, and the fault
// path's arch_fault_redirect_to_exit is what rewrites a live one (the two share no
// code, because a live frame cannot be rebuilt, and the fault seam additionally reads and
// clears sticky status registers that a scheduler-driven redirect must not touch).
//
// Idempotent in its values: `entry` and the stack top are absolute, so applying it
// twice before the thread resumes changes nothing.
//
// Total, deliberately: there is no failure return. A backend that cannot express a
// privileged thread-mode resume at a given stack top cannot host a thread either, and
// a silent decline here would downgrade a slay to a cooperative kill without saying so.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size);

// Switch the running context from `from` to `to`. MAY be deferred: on ARM this
// pends PendSV and the register swap happens on exception return; on the sim it
// happens now, or on interrupt-exit when called from ISR context. The scheduler
// must not assume the switch has completed when this returns.
void arch_switch(struct arch_context* from, struct arch_context* to);

// Enter the first thread from the boot context. `boot` is an optional save slot
// for the boot context: a backend MAY populate it (the sim does, so a later
// switch back unwinds to the host caller) or MAY ignore it and abandon the boot
// stack (the ARM backend does; the system always terminates via arch_shutdown,
// never by unwinding to boot). Callers MUST NOT switch back to `boot`.
void arch_start(struct arch_context* boot, struct arch_context* first);

// --- Critical section (RAII-wrapped by kernel IrqLock) ---------------------
typedef uintptr_t arch_irq_state_t;
arch_irq_state_t arch_irq_save(void);
void arch_irq_restore(arch_irq_state_t state);

// Nonzero while executing in interrupt/ISR context.
int arch_in_isr(void);

// --- Tickless clock + one-shot next-event timer ----------------------------
uint64_t arch_clock_now(void); // monotonic nanoseconds
void arch_timer_arm(uint64_t deadline_ns);
void arch_timer_disarm(void);

// Running core clock in Hz (the CMSIS SystemCoreClock the chip tracks at PLL
// bring-up). 0 where the backend has no silicon core clock (the host sim).
uint32_t arch_cpu_clock_hz(void);

// Read-only branch-clock oracle: the peripheral branch clock in Hz feeding the
// register block whose base is `base`, so a userspace driver derives its own
// baud/prescaler. `base` is the peripheral register-BLOCK base (e.g. a K64F UART
// at 0x4006A000); a backend MAY range-match within a block, but the contract only
// promises correctness for the block base itself. Returns 0 when this chip does
// not know the block's clock. The fallback TU returns 0 for every block, and a
// wrong branch clock silently garbles the wire, so 0 (not the core clock) is the
// safe unknown: the driver then falls back to its own explicit constant. Read-only
// and cascade-free; the DVFS rate-change notify is deferred.
uint32_t arch_periph_clock_hz(uintptr_t base);

// Ungate the clock and drop the bus-side supervisor-protect for the register block
// at `base`. `base` is the peripheral register-BLOCK base and must match a per-chip
// table EXACTLY; backends never range-match. Both bits are derived from `base`, so a
// caller cannot name a shared block's register or bit. Idempotent. Returns 0,
// -KOS_EINVAL (no entry for that base), or -KOS_ENOSYS (the fallback TU, no backend).
// A base has an entry only where the bus gate cannot open a kernel-reserved register:
// either the gate's granularity is contained by the block (K64F UART0), or the
// uncontained remainder is itself an arch_reserved_blocks entry (RT1062 USB1, whose AIPS
// slot also holds OTG2 and USBNC). The K64F PIT slot carries the kernel's own time base,
// so that base is refused.
int arch_periph_enable(uintptr_t base);

// Write `value` to the register at `base + offset` on the caller's behalf, PRIVILEGED.
// Some buses classify a block's WRITE side supervisor-only per REGISTER, so a granted
// window silently discards an unprivileged store to those (XMC4800 USIC FDR/BRG/CCR,
// RM Table 18-20 Write = PV).
//
// `base` must match a per-chip ALLOWLIST entry EXACTLY and `offset` must be one that
// entry names. A backend never range-matches and never admits a whole block.
//
// An entry's block MUST be CLOCKED whenever the syscall can reach it: the store runs in
// the kernel's frame, so a fault on a gated block reaches kfault_terminate and ends the
// system. XMC4800's USIC0 qualifies only because kickos_xmc_usic_init() ungates it from
// arch_init; a U1C0/U2C0 entry behind CGATCLR1 would not.
//
// Returns 0, -KOS_EINVAL (not on the allowlist, or `value` has a bit outside the entry's
// mask; the store is skipped, never masked) or -KOS_ENOSYS (no backend). The default
// declines and lives alone in arch/common/arch_periph_reg_write_default.cc.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// One-shot init-time pin-function config: point (port, pin) at the raw chip
// function code `func` (the PC/PCR encoding, opaque to the caller). Returns 0,
// -KOS_EINVAL (out of range), or -KOS_EBUSY (a kernel-owned pin the backend
// refuses). The fallback TU returns -KOS_ENOSYS so a non-empty board pin-map
// fails LOUD on a chip with no PORT/IOCR backend; a chip that owns its mux block
// (XMC4800, K64F) strong-overrides this.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func);

// Retune the core/bus clock to a P-state and return the ACTUALLY-LANDED core Hz.
// ALWAYS the truth about where the clock now sits, never a status:
//   - a retune that fully succeeds returns the requested point's Hz;
//   - a retune that FAILS and parks on a safe fallback (e.g. K64F fail_to_fei ->
//     ~20.97 MHz) returns THAT fallback Hz: non-zero, the clock DID move, so the
//     caller MUST run the coherence tail;
//   - 0 is returned ONLY when this chip cannot change its clock at all (the fallback TU
//     / unsupported backend). 0 NEVER means "failed but moved".
// The backend performs the flash-wait-state / voltage step and the arch_clock_now
// re-anchor INTERNALLY, bracketing the exact PLL/divider write (the re-anchor is the
// SOLE writer of the arch_clock_now mult on a re-anchor chip). MUST be called from
// privileged thread context with interrupts already masked by the caller and NOT from
// ISR context (see the coherence sequence, docs/design-m3-clock-select.md sec 2.3).
// Weak default returns 0 (this chip cannot change its clock).
//
// `target` carries a kos_pstate_t (sys/abi.h) as a plain u32; a backend that opts in
// includes sys/abi.h itself to name the KOS_PSTATE_* points. The achievable set is small
// and chip-specific; the truthful landed Hz is the RETURN value, not this selector.
uint32_t arch_cpu_clock_set(uint32_t target);

// Console coherence hooks (both no-op fallbacks):
//   arch_console_flush_sync: block until the TX shift register is fully idle
//     (transmission-complete, NOT merely buffer-empty). TWO callers need exactly this one
//     act, so it is one seam rather than two names for it:
//       - a clock retune, so no in-flight byte is still clocking out at the OLD baud when
//         the peripheral clock moves (S6). Called under the caller's IrqLock, BEFORE the
//         rate change. Only a chip whose console clock moves with the core clock cares.
//       - kickos_terminate, so arch_shutdown does not stop the core with a byte still in
//         the FIFO. EVERY chip whose console can outrun a shutdown cares about this one,
//         which is why the retune-only framing this comment used to carry was wrong: it
//         read as "no retune, no body needed" and left the terminal path truncating.
//     Must be BOUNDED. It is on the panic and shutdown paths, where a wedged UART must
//     cost a dropped tail rather than a hang.
//   arch_console_retune: re-derive + reprogram the console baud from the CURRENT
//     SystemCoreClock, AFTER the clock has landed. Called only when the clock actually
//     moved (achieved != previous).
void arch_console_flush_sync(void);
void arch_console_retune(void);

// --- Trace clock (telemetry timestamp seam) --------------------------------
// A dedicated high-resolution monotonic counter for telemetry timestamps: the
// ns arch_clock_now is too coarse to time a context switch (~1-5 us). u32 by
// design (wraps; the decoder reconstructs absolute time from the SESSION
// anchors). Per-arch source: armv7m = DWT CYCCNT (cycles); armv6m/chip-provided
// (rp2040 = the 1 MHz TIMER low half; nrf51 = semihosting us); sim = clock_now
// scaled to us. A target that has no such source does NOT define
// KICKOS_HAVE_TRACE_CLOCK and cannot enable telemetry (build-time FATAL).
uint32_t arch_trace_now(void);

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// Stamp the owning thread's trace id into a saved context, so the arch context-
// switch path can emit {from,to} tids read from the PHYSICALLY-swapped contexts,
// NEVER by re-reading shared scheduler state (which an ISR can rewrite between
// the switch decision and the physical swap). Telemetry-only: this seam does not
// exist when telemetry is compiled out (the id field is elided too). Called once
// per thread in thread_create.
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id);
#endif

// --- MPU: per-task memory protection ---------------------------------------
enum
{
    ARCH_MPU_NONE = 0,
    ARCH_MPU_R = 1u << 0,
    ARCH_MPU_W = 1u << 1,
    ARCH_MPU_X = 1u << 2,
    ARCH_MPU_DEV = 1u << 3 // device / MMIO
};

struct arch_mpu_region
{
    uintptr_t base;
    size_t size;
    uint32_t attr; // OR of the ARCH_MPU_* bits
};

// Load the running thread's regions on switch-in (replaces the whole active
// set). sim: mprotect over the user-RAM arena, granting the listed regions and
// no-access everywhere else. Regions are non-overlapping; attr is the
// UNPRIVILEGED access (supervisor comes from the background region / SYSMPU RGD0).
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n);

// Program the hardware from what arch_mpu_apply last recorded. On every arch whose
// context switch is DEFERRED (ARM PendSV, RX/RISC-V software interrupt) arch_mpu_apply
// only STASHES; the switch epilogue calls this after the physical swap. See
// docs/design-mpu-commit-deferred.md.
//
// The kernel calls this DIRECTLY in exactly one situation: the RUNNING thread's own
// region set was just widened and must be effective before the syscall returns
// (KOS_SYS_MEM_SELF_GRANT). Sound because outgoing and incoming are the same thread.
// Do NOT call it to make another thread's set live.
//
// Always resolves on every arch and both enforcement postures (the switch assembly
// calls it unconditionally); an empty no-op where apply already programs the hardware
// (the sim) or where there is no MPU.
void kickos_arch_mpu_commit(void);

// MMU-era NOTE (concepts, never mechanisms): a future VMSA/paging port introduces
// a PARALLEL arch_aspace_* family (build/switch/map a page-table root), NOT an
// overload of arch_mpu_apply and NOT a reinterpretation of arch_mpu_region. The
// MPU seam stays a flat, non-translating protection-region set; do not try to
// cram "load a page table" into it. See docs/design-mmu-era-exploration.md.

// The smallest region this arch's MPU can enforce: ARM PMSA 32 bytes, RISC-V PMP
// NAPOT 8, one host page on the sim. A return of 0 means this arch has NO enforceable
// MPU (classic ESP32 LX6, nRF51 M0) and allocations stay byte-granular.
// MUST return 0 or a power of two: arch_ram_region_size masks with min - 1.
size_t arch_mpu_min_region(void);

// Which of the two enforcing region-encoding modes this backend uses. Read only where
// arch_mpu_min_region() != 0.
// 1: size a power of two, base naturally aligned to it. PMSAv7 RASR carries
//    ctz(size) - 1 and PMP NAPOT folds the size into the trailing address
//    bits, so no other size is expressible.
// 0: base+limit descriptors (PMSAv8 RBAR/RLAR, SYSMPU SRTADDR/ENDADDR, RX
//    RSPAGEn/REPAGEn), so every arch_mpu_min_region() multiple is nameable.
// cmake/boot_arena.cmake scrapes this definition textually, so the body must stay a
// plain `return <integer>;` with no closing brace in any comment inside it.
int arch_mpu_region_pow2(void);

// True iff (base,size) is coverable exactly by ONE MPU descriptor with NO rounding.
// The MMIO grant test: a rounded MMIO window over-grants the neighbouring registers,
// so unlike arch_ram_region_size this rejects rather than snaps.
// The sim returns false: mprotect cannot map real MMIO, so it fail-closes.
bool arch_mpu_region_encodable(uintptr_t base, size_t size);

// Round `want` up to the region SIZE a backend can describe with one descriptor.
// arch_ram_alloc reserves this many bytes and the kernel sizes each thread/domain
// descriptor with the SAME call, so the descriptor matches the backing block exactly.
// This is the single point coupling allocation size to descriptor geometry; do NOT add
// a caller that treats the rounded value as usable capacity.
static inline size_t arch_ram_region_size(size_t want)
{
    size_t min = arch_mpu_min_region();
    if (min == 0)
    {
        return (want + 15u) & ~static_cast<size_t>(15u); // no MPU: byte-granular
    }
    if (want < min)
    {
        want = min;
    }
    if (arch_mpu_region_pow2() == 0)
    {
        size_t const rounded = (want + (min - 1u)) & ~(min - 1u);
        if (rounded < want) // size_t overflow: unroundable, hand back the raw request
        {
            return want;
        }
        return rounded;
    }
    size_t p = 1;
    while (p < want)
    {
        size_t next = p << 1;
        if (next < p) // size_t overflow: unroundable, hand back the raw request
        {
            return want;
        }
        p = next;
    }
    return p;
}

// Natural ALIGNMENT the block must sit on. In pow2 mode this is the region size itself,
// since PMSA/NAPOT snap the base to it; only that mode pays an alignment gap.
static inline size_t arch_ram_region_align(size_t want)
{
    size_t const min = arch_mpu_min_region();
    if (min == 0)
    {
        return 16u;
    }
    if (arch_mpu_region_pow2() == 0)
    {
        return min;
    }
    return arch_ram_region_size(want);
}

// True iff a RAM block at (base,size) is nameable by ONE descriptor. The RAM test;
// arch_mpu_region_encodable is the MMIO one. They diverge on the sim, where mprotect
// can describe any page-granular range in the arena but no peripheral window at all.
static inline bool arch_ram_region_admissible(uintptr_t base, size_t size)
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
    if (size < min)
    {
        return false;
    }
    if (arch_mpu_region_pow2() == 0)
    {
        return (base & (min - 1u)) == 0 and (size & (min - 1u)) == 0;
    }
    if ((size & (size - 1u)) != 0)
    {
        return false;
    }
    return (base & (size - 1u)) == 0;
}

// The MPU-governed user-RAM pool. Domain data + unprivileged-thread stacks are
// placed here so per-domain isolation is enforceable. sim: an mmap arena; MCU: a
// linker-defined region. arch_ram_alloc reserves a block sized by
// arch_ram_region_size() and aligned to arch_ram_region_align(), so exactly one MPU
// region covers it. Returns null on exhaustion or size 0.
uintptr_t arch_ram_base(void);
size_t arch_ram_size(void);
void* arch_ram_alloc(size_t size);

// The shared, app-wide regions every UNPRIVILEGED thread needs just to run under
// enforcement: its code (RX) and its static data/.bss (RW, no-execute). Unlike a
// privileged thread, an unprivileged one gets NO background-region default, so it
// faults fetching its own instructions / reading its own globals unless these are
// explicit regions. Fills up to `max` descriptors from the chip's linker-defined
// sections and returns the count; 0 when the arch does not model them (no-MPU
// backends, and the host sim, whose code/data are ungoverned). The kernel prepends
// these to an unprivileged thread's set (thread.cc), then the domain regions + stack.
size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max);

// True iff [ptr, ptr+len) is app code/rodata/.data the backend recognizes as
// caller-readable but does NOT describe as one of the running thread's MPU regions.
// The confused-deputy floor (syscall_dispatch) reads a user buffer/name privileged;
// it first checks the granted regions and, only if that misses, this hook, so the
// two together cover exactly what the UNPRIVILEGED caller could itself reach.
//   enforcing MPU backend: code/rodata/.data ARE real regions (see
//     arch_domain_static_regions), so the region check already admits them and this
//     returns false; any address outside the set is genuinely unreachable.
//   non-enforcing backend: no per-domain isolation exists to breach, but the kernel
//     dereferences the range PRIVILEGED, so it must be MAPPED: only the chip's
//     linker-defined memories are admitted (code/rodata extent, and static RAM up to
//     the arena base). An arena range falls through to the region check.
//   host sim: app + kernel share one binary, sections are not MPU regions, so a range
//     wholly inside the host image (and not the arena) is admitted; a wild pointer
//     outside both is rejected. It cannot separate app rodata from kernel statics;
//     the security boundary it DOES enforce (cross-domain arena reads) stays closed.
bool arch_user_text_readable(uintptr_t ptr, size_t len);

// The WRITE twin of the hook above: true iff [ptr, ptr+len) is app static data the
// backend recognizes as caller-writable but does NOT describe as one of the running
// thread's MPU regions. user_writable_ok checks the granted regions first and falls
// back here, exactly as user_readable_ok does.
//   enforcing MPU backend: .appdata/.appbss IS a real region (see
//     arch_domain_static_regions), so the region check already admits it and this
//     returns false; an address outside the set is genuinely unreachable.
//   non-enforcing backend: the kernel stores privileged, so only the chip's static-RAM
//     extent (RAM origin up to the arena base) is admitted. An arena range still falls
//     through to the region check, so a later enforcing build of the same backend stays
//     sound.
//   host sim: app and kernel share one host image whose sections are not MPU regions,
//     so a range wholly inside the image and clear of the arena is admitted.
bool arch_user_data_writable(uintptr_t ptr, size_t len);

// An address that faults on unprivileged access (sim: a reserved arena page no
// domain owns). Used by the isolation self-test.
uintptr_t arch_mpu_probe_addr(void);

// --- Rule 7: kernel-reserved MMIO blocks (docs/design-m4-driver-model.md sec.7) --
// The owns-for-life peripherals a grant must NEVER hand to userspace: the timebase
// block, the IRQ controller, every access-permission controller (the MPU/PMP twin
// AND any bus-side gate such as the K64F AIPS bridge PACRs or the ESP32-C6
// HP_APM/HP_TEE), and the clock/reset gate registers. The grant path (kernel/grant)
// refuses any region overlapping one of these.
struct arch_reserved_block
{
    uintptr_t base;
    size_t size;
};

// Fill `out` (capacity `max`, the kernel passes KICKOS_MAX_RESERVED) with this
// chip's reserved blocks and return the count. KICKOS_RESERVED_NONE (0) is legal
// (the sim owns nothing MPU-governable). NO fallback TU on purpose: an enforcing
// port that forgets to declare its set is a LINK error, not a silent open hole
// (affirmative fail-closed). Defined per enforcing chip under #if KICKOS_HAVE_MPU.
#define KICKOS_RESERVED_NONE 0u
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max);

// Nonzero on a core with the Cortex-M bit-band peripheral/SRAM alias (M3/M4): a
// reserved peripheral block is then ALSO reachable through its word-per-bit alias
// image, and a device grant touching either alias window is refused (kernel/grant).
// Fallback TU answers 0 (no alias: M0+/M7/RISC-V/RX); the bit-band M4 chips
// (mk64f, stm32f411, xmc4800) strong-override to 1.
int arch_bitband_present(void);

// --- Syscall trap (user -> kernel) -----------------------------------------
// Issued by the userspace syscall stubs; returns the syscall result.
//
// CONTRACT (portability-critical): the arch MUST run syscall_dispatch() in
// privileged THREAD context on the calling thread's own continuation, NOT in
// ISR/handler context. A blocking syscall (sem_wait, sleep, ...) blocks by an
// ordinary synchronous context switch (arch_switch completes, resuming the
// dispatch inline when the thread is next scheduled), exactly as if the kernel
// routine were called directly. The kernel's blocking primitives depend on
// this; arch_in_isr() must read false during dispatch.
//   sim: arch_syscall is a plain call, so dispatch already runs in thread
//        context and arch_switch swaps synchronously.
//   ARM (later): SVC raises privilege and continues dispatch in privileged
//        thread mode (so a blocking switch/PendSV saves the mid-dispatch
//        continuation and resumes it), rather than running dispatch in the SVC
//        handler where a switch could only be deferred.
// 64-bit ARGUMENTS are split into uintptr_t halves (see sys/abi.h), so no
// arch-specific argument-marshalling seam is needed.
//
// The result comes back at the ABI's 64-bit return width: arch_syscall takes the low
// half, arch_syscall64 both. They are the SAME trap, the pair being whatever the psABI
// uses for a long long return (r0:r1 on ARM, a0:a1 on RISC-V, R1:R2 on RX), so a
// backend that preserves the pair serves both.
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

// --- Register-carrying IPC trap (KOS_SYS_CALL_REG) -------------------------
// Declared only under KICKOS_ARCH_HAS_IPC_FASTPATH, so a backend without the
// trap-handler fastpath cannot be selected by mistake.
//
// `io` is KOS_CALL_REG_WORDS + 3 words, in and out over the SAME storage:
//   in  io[0]=nr io[1]=ep_cap io[2]=packed lens io[3..] = request payload
//   out io[1..] = reply payload; the return value is the call's result
// The trap must preserve `io` itself across the ecall/svc.
//
// KOS_CALL_REG_FALLBACK is not an error: the fastpath declined and the caller must
// re-issue through KOS_SYS_CALL.
#if KICKOS_ARCH_HAS_IPC_FASTPATH
int32_t arch_syscall_reg(uint32_t* io);

// Store a syscall result into a SAVED context, for a thread the fastpath parked with no
// kernel continuation to return through. The arch writes it where its own restore path
// reloads the syscall's return register from. Called only from the switch that resumes
// that thread, so the context is not live in any register file.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result);
#endif

// --- Interrupt controller (thin abstraction: mask / unmask / raise) --------
// Deliberately minimal: no priority grouping, pending-vs-active, edge-vs-level,
// or tail-chaining; those are earned per-chip at M1 against real silicon. On ARM
// this backs onto the NVIC; on the sim, signal-driven injection.
//
// mask/unmask gate delivery of a line. The generic first-level ISR masks the
// line before waking its driver (thread context), which unmasks via irq_ack once
// serviced, so the line cannot re-fire while it is being handled. A raise that
// lands on a MASKED line is LATCHED one-deep (coalesced), not dropped: it is
// redelivered through the normal ISR path the instant the line is unmasked.
//
// RESET CONTRACT (uniform across every arch): all lines start MASKED at reset. A
// driver unmasks its line (arch_irq_unmask, or irq_claim which arms it) before
// use; nothing may assume a line is deliverable until it has been unmasked.
//
// LOCKING CONTRACT: all four of mask/unmask/inject/clear_pending are SELF-BRACKETED
// (each does its own interrupt-masked critical section over its state RMW), so a
// caller need not hold IrqLock; the test-scaffolding syscalls (irq_inject,
// irq_unmask) call them bare.
//
// SINGLE-DOORBELL CONTRACT (software backends: sim, rv32imac, xtensa, rxv3-soft):
// a coalesced redelivery is carried through ONE shared cell + ONE physical doorbell
// and the per-line pending bit is cleared as it is rung. So AT MOST ONE unmask with
// a pending redelivery may occur per IrqLock/interrupts-masked region; a second
// would clobber the first's identity and lose an event. Holds today (irq_claim/
// wait/ack each unmask exactly one line per lock section); a future bulk-rearm path
// needs the identity-free dispatcher (see TODO M4).
void arch_irq_mask(int line);

// Enable delivery of a line, PRESERVING any raise latched while it was masked: a
// latched raise fires through the normal first-level ISR path (kickos_isr_irq)
// after enable, never a direct notification from unmask itself.
void arch_irq_unmask(int line);

// Discard any raise latched on a line (best-effort: a controller that cannot drop
// a native pending, e.g. the PLIC for a real device line, no-ops there). The
// explicit discard primitive: called at first-arm (irq_claim / console_tx /
// bench) to drop pre-registration garbage, and reserved for the M4 level-trigger
// rearm path.
void arch_irq_clear_pending(int line);

// Raise device line `irq` (the controller's "raise"). sim: delivers an async
// signal so the ISR runs in interrupt context; ARM: pends the NVIC line. Drives
// scheduler trigger #4. A real driver never raises; it register/wait/acks. Raising
// is fake-a-device-firing test scaffolding, privilege-gated.
void arch_irq_inject(int irq);

// --- Minimal debug console (bottom edge of the in-kernel console driver) ---
// Write-only. Two edges:
//   arch_console_write:      normal path. A chip with a buffered console makes
//                              this enqueue + prime the TX IRQ (see console_tx.h);
//                              otherwise it is the polled writer.
//   arch_console_write_sync: polled, bounded; safe with the scheduler/IRQs down.
//                              Panic / fault / assert / pre-arm output uses this.
//                              Defaults to arch_console_write via a lone-TU fallback,
//                              not a weak symbol; a chip with a buffered console
//                              defines its own polled writer instead.
void arch_console_write(char const* buf, size_t n);
void arch_console_write_sync(char const* buf, size_t n);

// Force the UART back to a known polled-ready channel on the panic path after a
// userspace console driver may have left its granted register window garbled (D6).
// No-op fallback TU (boards that never hand over need nothing); a chip
// that supports handover overrides it with an idempotent full-window register rewrite.
void arch_console_reclaim(void);

// The register window arch_console_reclaim writes, i.e. the device a userspace console
// driver is granted on this chip. *size == 0 (the fallback TU) means "no window": either
// the console is not a memory-mapped device, or this chip has no handover.
//
// The window is the ARCH's answer, never an address userspace supplied: the reclaim is
// about to reprogram exactly these registers.
void arch_console_reclaim_window(uintptr_t* base, size_t* size);

// --- Single on-board kernel diagnostic LED (optional) ----------------------
// The board's one diagnostic LED, the raw bottom edge of the kernel diag LED
// (kdiag_led_*): a last-resort self-debug facility that must work UART-less, in a
// fault, before drivers exist. NOT a general device driver.
// arch_diag_led_init() configures the pin once at boot; arch_diag_led_set() drives
// it (on != 0). Both have a no-op fallback-TU default (kernel/init/led.cc): a board
// with no known LED (or the sim) does nothing; a chip backend with one provides
// strong overrides. Raw set, no toggle: the kernel side tracks the state.
void arch_diag_led_init(void);
void arch_diag_led_set(int on);

// --- Chip-specific fault decode (optional) ----------------------------------
// Called by the core fault reporter after it dumps the CPU frame + fault-status
// registers, so a chip whose isolation trap does NOT surface in the core CFSR
// (K64F SYSMPU -> a bus error, not a MemManage) can add its own capture. Fallback-TU
// no-op default (per-arch); a chip backend with an external MPU strong-overrides.
void arch_fault_report_extra(void);

// --- Fault isolation: kill the faulting thread instead of the system --------
// `frame` is the backend's own fault frame (whatever its handler already holds),
// opaque to the core and never dereferenced outside these two.
//
// Did this fault happen in UNPRIVILEGED THREAD context, i.e. is it the running
// thread's own fault rather than a kernel bug? Syscall dispatch runs PRIVILEGED on
// the thread's own stack, so the thread's identity is not the answer; the CPU's
// privilege at fault time is. Fallback-TU default: false, so a backend that has not
// opted in panics.
//
// A backend whose fault frame lives in MEMORY on the faulting thread's own stack must
// gate on kickos_fault_frame_trusted BEFORE it reads a single word of that frame: a
// wild SP hands the handler a frame the thread never legitimately produced. Facts read
// out of a REGISTER (privilege, cause, fault-status) are always valid and need no gate.
bool arch_fault_is_user_thread(void* frame);

// Rewrite `frame` so the exception return lands in kickos_thread_fault_exit,
// privileged, in thread mode, at the TOP of the faulting thread's own stack
// (kickos_fault_stack_top), and hand the fault facts to kickos_fault_record for the stub
// to print in thread context. The thread is dying, so its register values need not be
// preserved. Called ONLY when arch_fault_is_user_thread returned true; fallback-TU
// default: empty.
void arch_fault_redirect_to_exit(void* frame);

// --- Idle -------------------------------------------------------------------
// Block until the next interrupt (ARM WFI; sim sigsuspend).
void arch_idle_wait(void);

// --- Provided by the kernel, called back by the arch backend ---------------
// Next-event timer expired (tickless deadline or, if enabled, periodic tick).
void kickos_isr_timer(void);
// Device interrupt line `irq` fired (sim: injected event; ARM: NVIC line).
void kickos_isr_irq(int irq);
// A thread's entry function returned; the arch trampoline routes here.
void kickos_thread_return(void) __attribute__((noreturn));
// The arch-independent syscall table dispatch (called by arch_syscall / SVC).
// Sign rule: an errno arm returns a NEGATED int and sign-extends, so it stays negative
// read at any width; every other arm zero-extends. The low half is what a 32-bit target
// always saw.
uint64_t syscall_dispatch(uintptr_t nr,
                          uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
// A memory-protection violation was caught (sim: SIGSEGV over the arena).
void kickos_isr_fault(uintptr_t addr, int is_write);

// Fault isolation, called from the arch fault handler BEFORE it starts its dump.
// Applies the kill rule and, when it holds, calls arch_fault_redirect_to_exit.
// Returns true when the handler must simply RETURN: the exception return then lands
// in kickos_thread_fault_exit. False means the fault panics.
bool kickos_fault_kill_thread(void* frame);

// Does [frame, frame + bytes) lie inside the RUNNING thread's own stack? The
// arch-neutral frame-validity guard: a fault frame that the hardware (or the trap
// prologue) wrote somewhere else was produced by an SP the thread had no business
// holding, so nothing in it may be believed and the fault must panic. Fails closed on
// no current thread, on idle, and on a thread with no recorded stack. Cheaper
// arch-specific early-outs (armv7m reads the CFSR stacking bits) do not replace it:
// neither test subsumes the other.
bool kickos_fault_frame_trusted(void const* frame, size_t bytes);

// Top (exclusive) of the RUNNING thread's own stack, or 0 when there is none to name (no
// current thread, idle, or no recorded stack). Where a backend's redirect puts the SP, so
// the stub runs with the whole stack under it rather than at the depth the thread had
// reached: exit_current, cap_teardown and the fault print all need headroom there. A
// backend must still establish that these fields describe the stack the stub will run on
// (kickos_fault_frame_trusted on the fault's own SP) before it uses this value.
uintptr_t kickos_fault_stack_top(void);

// Did the faulting access land in the GUARD BAND immediately below the running thread's
// stack, i.e. did the thread run off the bottom of its own stack? For a backend whose
// exception CANCELS the faulting instruction and restores SP (RXv3), the SP still reads
// in-bounds after an overflow and kickos_fault_frame_trusted cannot see one; the faulting
// ADDRESS is then the only evidence there is. What it buys is ATTRIBUTION, not safety: the
// stack reset is what keeps the stub off an exhausted stack, and an access far below the
// stack is a wild write that belongs to the thread alone. Fails closed (true) when there is
// no recorded stack to compare against. KICKOS_FAULT_STACK_GUARD_BAND is the width.
bool kickos_fault_below_stack(uintptr_t addr);

// The facts arch_fault_redirect_to_exit captured, printed later by
// kickos_thread_fault_exit in thread context. NOTHING may print from the handler:
// printing there forces kpanic_enter, which masks interrupts and reclaims the console
// permanently, and the system is meant to survive this fault. `status_name` names the
// arch fault-status word for the reader (armv7m: "CFSR"). `addr` is read only when
// `addr_valid`, since a fault-address register holds stale contents otherwise.
void kickos_fault_record(char const* status_name, uint32_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid);

// Where arch_fault_redirect_to_exit points the faulting thread. Runs privileged, in
// thread mode, on that thread's own stack.
void kickos_thread_fault_exit(void) __attribute__((noreturn));

// Where arch_ctx_redirect points a SLAIN thread. Runs privileged, in thread mode, at
// the top of that thread's own stack, and prints nothing: a slay was asked for by a
// thread that already knows. `arg` is always null and exists only to match the entry
// signature arch_ctx_redirect fabricates against.
void kickos_thread_slay_exit(void* arg) __attribute__((noreturn));

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// A context switch physically completed: emit a SWITCH record {from_tid, to_tid}.
// The arch switch path calls this at the REAL register swap (ARM: the PendSV
// tail; sim: each ucontext swap site), from tids read out of the two contexts it
// actually swapped. from_tid == 0xFFFF on the very first switch. RESCAN group.
void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid);

// Emit the closing SESSION record (final records_attempted + a second clock
// anchor). The sim backend calls this from arch_shutdown just before it drains
// the ch1 ring to a file, so the decoder gets its two-anchor resync span.
void kickos_trace_final_session(void);

// Print a one-line telemetry health report (attempted vs dropped) to the console
// at shutdown; a CI gate reads it to verify the drop accounting.
void kickos_trace_report_counters(void);
#endif

} // extern "C"

#endif
