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

// Per-arch definition of `struct arch_mpu_encoded`, the descriptor words a switch
// programs. Resolved to arch/<arch>/include/kickos/arch/mpu_encoded.h, and shipped only
// by an arch some board enforces on: elsewhere the type stays incomplete and every use
// of it is a pointer.
#if KICKOS_HAVE_MPU
#include <kickos/arch/mpu_encoded.h>
#else
struct arch_mpu_encoded;
#endif

// C++ ONLY: the extern "C" below is UNGUARDED, so a C includer breaks here.
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
// At one core a MACRO, so the value is folded at every use: -Os out-lines an
// always_inline candidate in system/include/kickos/sys/atomic.h. The multi-core arm is a
// declaration only, so a port that raises KICKOS_NUM_CORES and ships no definition is a
// LINK error rather than a kernel that believes it is on core 0.
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
// TOTAL: every backend answers, and the signature carries no failure return. A backend
// able to host a thread can express a privileged thread-mode resume at a given stack top,
// and a decline here would downgrade a slay to a cooperative kill without saying so.
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
// not know the block's clock. The fallback TU returns 0 for every block, and a wrong
// branch clock silently garbles the wire, so 0 rather than the core clock is the safe
// unknown: the driver then falls back to its own explicit constant. Read-only and
// cascade-free, so a rate change reaches a driver only when it asks again.
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
// An entry's block MUST be CLOCKED, POWERED and out of RESET whenever the syscall can
// reach it, and the caller carries that: the seam validates alignment, wrap and
// possession, which is the whole of what it reads. The store runs in the kernel's frame,
// so a fault on an unready block reaches kfault_terminate and ends the system. XMC4800's
// USIC0 qualifies because kickos_xmc_usic_init() ungates it from arch_init; a U1C0/U2C0
// entry behind CGATCLR1 would not.
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
//         the FIFO. EVERY chip whose console can outrun a shutdown needs a body here,
//         not only a chip that retunes; without one the terminal path truncates.
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
    ARCH_MPU_DEV = 1u << 3,    // device / MMIO
    ARCH_MPU_NOCACHE = 1u << 4 // Normal, outer+inner non-cacheable
};

// How this chip satisfies the ARCH_MPU_NOCACHE region attribute.
enum arch_mpu_nocache
{
    // A data cache sits in the path and the region descriptor carries no memory type.
    ARCH_MPU_NOCACHE_REFUSED = 0,
    // The region descriptor carries the memory type.
    ARCH_MPU_NOCACHE_PROGRAMMED = 1,
    // No data cache reaches the grantable memory, so the attribute costs nothing to honour.
    ARCH_MPU_NOCACHE_ALREADY = 2
};

// Read at grant ADMISSION (kernel/grant), never on a commit path: a commit backend drops
// a region it cannot encode SILENTLY. Answers for the enforcement posture actually built,
// not only for the silicon: with KICKOS_HAVE_MPU=0 no region is programmed at all.
//
// ONE answer for the whole chip, while the property is per REGION: a Cortex-M7 caches OCRAM
// but not TCM. Every backend answers for the band its arena lives in, so a linker script
// that moves the arena into a differently cached band must revisit its chip's answer.
int arch_mpu_nocache_support(void);

struct arch_mpu_region
{
    uintptr_t base;
    size_t size;
    uint32_t attr; // OR of the ARCH_MPU_* bits
};

// Encode `n` regions into the descriptor words this backend programs, and report which
// of them got a descriptor as a bitmask (bit i for regions[i]). A region that fails
// arch_mpu_region_encodable gets none: the image never rounds a base or a size, so a
// misaligned request is refused here rather than widened in hardware. Slots past `n`
// are written inactive, so an image encoded from a zero-length set grants nothing.
//
// Called at MUTATION, never on a switch path. Defined only where KICKOS_HAVE_MPU.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out);

// Load the running thread's regions on switch-in (replaces the whole active
// set). sim: mprotect over the user-RAM arena, granting the listed regions and
// no-access everywhere else. Regions are non-overlapping; attr is the
// UNPRIVILEGED access (supervisor comes from the background region / SYSMPU RGD0).
//
// `image` is what the hardware is programmed from and is `regions` already encoded;
// the raw set travels beside it for the backends that need the addresses themselves
// (sim mprotect, RX same-set skip). It is null only where KICKOS_HAVE_MPU is 0.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image);

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

// This seam is a flat, NON-TRANSLATING protection-region set. A VMSA/paging port gets the
// parallel arch_aspace_* family declared below, and no backend implements both. See
// docs/design-m6-mmu.md F6.

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

// Round up to a power of two. Separate from arch_ram_region_size because that one
// answers a DESCRIPTOR question and on a base+limit backend does not round to a power of
// two at all.
static inline size_t kickos_pow2_ceil(size_t want)
{
    size_t p = 1;
    while (p < want)
    {
        size_t const next = p << 1;
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
//
// UNDER KICKOS_TLS EVERY BLOCK IS STRIDED BY A POWER OF TWO, whatever the descriptor
// geometry asks for. Unprivileged ARM code has no register that differs per thread except
// SP, so __aeabi_read_tp is SP masked down to the thread's own block, and a block that
// does not start on a multiple of that stride masks into its neighbour's. The mask in
// arch_ram_alloc needs a power of two anyway, which is why this rounds rather than
// returning arch_ram_region_size.
static inline size_t arch_ram_region_align(size_t want)
{
    size_t const min = arch_mpu_min_region();
    size_t geometry = 16u;
    if (min != 0)
    {
        geometry = min;
        if (arch_mpu_region_pow2() != 0)
        {
            geometry = arch_ram_region_size(want);
        }
    }
#if defined(KICKOS_TLS) && KICKOS_TLS
    size_t const stride = kickos_pow2_ceil(want);
    if (stride > geometry)
    {
        return stride;
    }
#endif
    return geometry;
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

// --- Address space: a translating memory backend ----------------------------
// The parallel family beside the region calls. A chip selects region descriptors or
// translation on the axis mpu.cmake already uses, and no backend implements both.
// docs/design-m6-mmu.md F6.
//
// Opaque: the kernel names a space by pointer and never sizes or embeds one. Translation
// tags are the backend's, allocated and invalidated inside destroy and activate.
struct arch_aspace;

// The access a mapping grants the UNPRIVILEGED level; the kernel occupies a fixed high
// range of every space. At least one bit is required, a guard page being an unmapped
// page: R=W=X=0 marks a next-level pointer on RISC-V.
enum
{
    ARCH_MAP_R = 1u << 0,
    ARCH_MAP_W = 1u << 1,
    ARCH_MAP_X = 1u << 2
};

// A physical address, wider than uintptr_t because Sv39 pairs a 39-bit virtual range with
// a 56-bit physical one and Sv32 pairs 32 with 34.
typedef uint64_t arch_phys_addr_t;

// A memory type, never the bits a backend encodes it to: the entry format varies by
// ratified extension, by vendor, and by a machine-mode enable an S-mode kernel does not
// own.
enum arch_map_memtype
{
    ARCH_MAP_NORMAL = 0,  // cacheable, write-back
    ARCH_MAP_NOCACHE = 1, // Normal, outer and inner non-cacheable
    ARCH_MAP_DEVICE = 2   // device / MMIO
};

// Whether this backend can honour `type`. Read at grant ADMISSION, never on a map path: a
// quietly downgraded type hands a driver a cacheable view of a DMA buffer. The answer may
// come from the board rather than a register, Svpbmt carrying no identification bit.
bool arch_aspace_memtype_support(enum arch_map_memtype type);

// TOTAL OR FAIL: on anything but ARCH_ASPACE_OK the space is as it was, with no frame
// leaked and no partial mapping installed.
enum arch_aspace_result
{
    ARCH_ASPACE_OK = 0,
    // No frame for a table the backend needed. Freeing frames and retrying can succeed.
    ARCH_ASPACE_ENOMEM = 1,
    // The backend's structure cannot hold the mapping with frames still available, so
    // retrying is futile. A hashed page table must evict from a full bucket during a map.
    ARCH_ASPACE_ECAPACITY = 2,
    // Not expressible: a misaligned or empty range, a right or type this backend refuses,
    // or an unmap of a range not wholly mapped.
    ARCH_ASPACE_EINVAL = 3
};

// The mapping granule in bytes, a power of two, and the unit `va`, `pa` and `pages` are
// counted in. The frame allocator and the guard-page arithmetic read this one answer.
// Larger mappings are not a second granule.
size_t arch_aspace_granule(void);

// A space with the kernel's fixed high range present and nothing else mapped, or null
// when a root cannot be allocated. A backend with a SINGLE root register installs the
// kernel half here by copying the TOP-LEVEL entries, so every space shares the tables
// below them and a later kernel-half edit stays in step; copying one level deeper makes
// the kernel half per-space state that every edit must walk.
struct arch_aspace* arch_aspace_create(void);

// Release the root, every table under it and every frame the space still holds, with the
// backend's invalidation ordered BEFORE the root or its tag can be reused. Null is a
// no-op. Activate another space before destroying the running one.
//
// HOLDS MEANS MAPPED, so a space must not still map a frame it does not own when this
// runs. F10's handoff maps ONE block into two spaces, and destroying the borrower would
// return that block to the allocator while the donor's leaf still stands: the second
// destroy then frees a frame the pool has already handed to someone else, which the
// allocator's already-free guard cannot see. The borrower unmaps its range first.
void arch_aspace_destroy(struct arch_aspace* space);

// Map `pages` granules at `va` onto the frames at `pa`, or remove that many at `va`.
//
// COHERENCE-COMPLETE: when either returns the change is visible to this core, whatever
// maintenance that took having happened inside. A64 requires break-before-make when
// replacing a live entry, so the invalidate belongs between the two writes. A map into a
// slot that was empty needs one too, architectures caching negative translations.
// Batching, where it pays, is begin and commit on the space.
//
// At more than one core the initiator waits: A64's broadcast barrier blocks until every
// other PE has drained, while RV64 and x86_64 wait on far-side code. An unmap whose frames
// are being FREED may defer the remote half, the boundary being reuse; an unmap that
// REVOKES may not, and the far-side handler takes no kernel lock. docs/design-m6-mmu.md T9.
//
// Three callers against activate's two, none of them a root switch: the self-grant, which
// widens the RUNNING space mid-syscall, and the image and stacks a process is built from.
enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages,
                                        uint32_t rights, enum arch_map_memtype type);
enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va,
                                          size_t pages);

// Make `space` the translation the unprivileged level runs under. Two callers: the switch
// bookkeeper, which the IPC fastpath shares, and the first-thread start. The self-grant
// widens the running space and is a map.
//
// TOTAL: every backend answers and the signature carries no failure return, as
// arch_ctx_redirect's does not.
//
// Where a root and a translation tag change together the architecture documents the ORDER,
// and its shape depends on a translation-control field this seam does not expose.
//
// The space is reached from the incoming thread's TASK; arch_context_init takes an entry
// point, an argument, a stack and a privilege posture, and no memory parameter.
void arch_aspace_activate(struct arch_aspace* space);

// Reach the frame backing one page of `space` through a kernel-usable pointer, and release
// it, rather than adding an offset to a user address: a backend mapping all of physical
// memory in its high half inlines this to an addition, and one whose physical space is
// wider than its virtual has no offset to add.
//
// AT LEAST TWO are holdable at once per core: the endpoint copy holds a source page in one
// space and a destination page in another, so a backend with a finite window pool sizes it
// for that.
//
// `va` need not be granule-aligned and the pointer addresses the same byte. Null when the
// page is not mapped in `space`. A range contiguous in virtual memory is not contiguous in
// physical memory, so a caller splits at granule boundaries and acquires each page.
void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va);
void arch_aspace_release(struct arch_aspace* space, uintptr_t va);

// The space the boot path installed, and the ONLY handle for it: its tables are link-time
// constants rather than something this seam created. It is what the kernel installs when
// the running process's space is DESTROYED under it, the last thread of a task being the
// one executing when its domain is released, and a freed root left in the translation
// control is a walk into the frame pool.
struct arch_aspace* arch_aspace_boot(void);

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

// Fill `out` (capacity `max`, the kernel passes KICKOS_MAX_RESERVED) with this chip's
// reserved blocks and return the count. KICKOS_RESERVED_NONE (0) is legal, the sim owning
// nothing MPU-governable. Every definition is per chip under #if KICKOS_MEMORY_ENFORCED,
// the fact that protection is LIVE, which a translating chip sets while carrying no region
// descriptors. A port that forgets to declare its set is therefore a LINK error rather than
// a silent open hole (affirmative fail-closed).
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

// THE SAME TRAP, SPELLED FROM KERNEL TEXT. Where a translating backend splits the image,
// the leaf above links into the app's half: an EL0 thread executes it, so it carries
// privileged-execute-never and kernel text may not call it (docs/design-m6-mmu.md, T5b.1).
// The backend therefore assembles a second copy under these names, in its own half, which
// every address space maps by construction. Spell these in kernel code and the trap is
// correct on every backend; where the image is not split there is nothing to duplicate and
// the names alias the ones above.
#if KICKOS_HAVE_ASPACE
uintptr_t karch_syscall(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uint64_t karch_syscall64(uintptr_t nr,
                         uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
#else
#define karch_syscall arch_syscall
#define karch_syscall64 arch_syscall64
#endif

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
// The whole seam is delivery gating: mask, unmask, inject, clear_pending. Priority
// grouping, pending-vs-active, edge-vs-level and tail-chaining are earned per-chip
// against real silicon. On ARM this backs onto the NVIC; on the sim, signal-driven
// injection.
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
// would clobber the first's identity and lose an event. irq_claim/wait/ack each unmask
// exactly one line per lock section, which is what holds it; a bulk-rearm path would
// need the identity-free dispatcher (see TODO M4).
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
// A chip that supports handover overrides this with an idempotent full-window register
// rewrite; the fallback TU is a no-op, which is what a board that keeps the console needs.
void arch_console_reclaim(void);

// The register window arch_console_reclaim writes, i.e. the device a userspace console
// driver is granted on this chip. *size == 0 (the fallback TU) says this chip names no
// window, the console being either not memory-mapped or not handed over.
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

// One frame for a translating backend's own tables, and its release. ONE allocator exists
// and the kernel owns it, so a backend that kept a pool of its own would make destroy's
// accounting unverifiable against the kernel's counters.
//
// PHYSICAL, because destroy reads what it frees out of a descriptor, where an output
// address is all there is. The frame is granule-sized and granule-aligned; its CONTENTS are
// undefined, so a backend that needs zeroes writes them. 0 is exhaustion, which maps to
// ARCH_ASPACE_ENOMEM rather than to a panic. Compiled where a translating backend is
// (KICKOS_HAVE_ASPACE).
arch_phys_addr_t kickos_frame_alloc(void);
void kickos_frame_free(arch_phys_addr_t frame);

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

#if KICKOS_KERNEL_STACKS
// Does [frame, frame + bytes) lie inside the RUNNING thread's own KERNEL stack block? The
// same guard for a backend whose trap entry builds its frames there instead of on the
// interrupted thread stack: the sp a U-mode thread chose is never written through, so a
// frame that is NOT in the block was produced by some other sp and nothing in it may be
// believed. Fails closed on no current thread, on idle, and on a thread with no block
// seated (ctx.kernel_sp 0). Compiled only where the blocks exist.
bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes);
#endif

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
//
// `status` is 64-bit because a status REGISTER is, on the arches that have one that wide:
// AArch64's ESR_EL1, and RV64's mcause, whose interrupt bit is bit 63. Narrower backends
// promote and pay nothing. The alternative was to truncate and argue per arch that nothing
// above bit 31 is ever set, which is an argument about each CALLER's filtering rather than
// about the register, and it would have to be re-made for every 64-bit backend.
void kickos_fault_record(char const* status_name, uint64_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid);

#if defined(KICKOS_ENABLE_SELFTEST)
// Trap-stack regression: names kickos_trapstack_witness if an unprivileged thread's trap
// frame reached that kernel word. A backend calls it on its PANIC path, after
// kpanic_enter, so the faultsurvive `kwrite' gate can prove the trap prologue did NOT
// store through a U-mode sp into kernel memory. Silent when intact.
void kickos_trapstack_witness_report(void);

// NESTED-TRAP regression, and a different claim from the one above: not where a wild sp
// pointed, but which stack the kernel picked for an interrupt taken while the kernel was
// ALREADY running. Such a trap arrives with no bound applied to the sp it interrupts, so a
// frame built there lands wherever the interrupted code had descended to; the claim is that
// this is never the interrupted THREAD's own stack, which on a backend whose entry has moved
// to per-thread kernel stacks holds because the dispatch it interrupts is not there either. A
// backend calls this once per such trap with the frame it built and the interrupted thread's
// stack bounds (`lo` 0 when there is no current thread). The counters are kernel-side so one
// syscall serves every backend, and KOS_SYS_NEST_WITNESS reads them. A COUNTER READ and not a
// print: a kprintf on the shutdown path would put the console's varargs route inside the
// syscall descent it stands beside.
void kickos_nestwitness_note(uintptr_t frame, uintptr_t lo, uintptr_t hi);
uint32_t kickos_nestwitness_count(int which);
#endif

// Where arch_fault_redirect_to_exit points the faulting thread. Runs privileged, in
// thread mode, on that thread's own stack.
void kickos_thread_fault_exit(void) __attribute__((noreturn));

// Contain the thread whose live stack pointer a trap prologue just refused, instead of
// ending the system for it. Raises it to CANCEL_SLAY and hands back the context to resume.
//
// `offender` NAMES THE THREAD PHYSICALLY EXECUTING, and the caller must pass what the arch
// layer switched TO and not the scheduler's own current: a switch booked before the trap has
// already published the incoming thread there, so current names the wrong thread on every
// asynchronous refusal.
//
// THE OFFENDING THREAD MAY WELL BE THE ONE RESUMED, and what makes that safe is the REBUILD
// and not any promise to run something else. pick_next returns it whenever it is still the
// highest-priority runnable thread, which on a synchronous refusal is the ordinary case;
// switch_book then rebuilds its context onto the exit stub at the top of a stack the TCB
// names. Its registers were never saved and are discarded, so the frame the guard refused to
// build is never needed, and nothing about the refused pointer is trusted at any point.
//
// `slain_name`, when not null, receives the slain thread's name on success, so a reporter can
// credit the refusal to a thread. Read on the caller's own trap or interrupt stack; the exit
// stub is the wrong place for it, the RET class binding that descent against every thread's
// stack floor. Untouched when this returns null.
//
// Returns null wherever the caller must still terminate: no thread owns `offender` (which is
// what refuses idle, outside the pool), it is privileged, the slay could not claim it, or
// nothing can run. Takes the kernel lock itself; the trap prologues that reach it run
// unmasked.
struct arch_context* kickos_thread_contain_wild_stack(struct arch_context* offender,
                                                      char const** slain_name);

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
