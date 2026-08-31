// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS porting interface. Every target implements exactly this seam.
//
// ISA-neutral: it names concepts (switch, crit-section, timer, mpu, syscall) and never the
// mechanism a backend implements one with. A new arch fits it with no signature changes.

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

// Per-arch definition of `struct arch_context` (opaque to the kernel; sized by the arch).
// Resolved to arch/<arch>/include/kickos/arch/context.h.
#include <kickos/arch/context.h>

// Per-arch definition of `struct arch_mpu_encoded`, the descriptor words a switch programs.
// Resolved to arch/<arch>/include/kickos/arch/mpu_encoded.h, and shipped only by an arch some
// board enforces on: elsewhere the type stays incomplete and every use of it is a pointer.
#if KICKOS_HAVE_MPU
#include <kickos/arch/mpu_encoded.h>
#else
struct arch_mpu_encoded;
#endif

// C++ only: the extern "C" below is unguarded, so a C includer breaks here.
extern "C"
{

// --- One-time backend bring-up ---------------------------------------------
// Called exactly once, before the kernel runs.
void arch_init(void);

// C-runtime data init driven by the linker's copy/zero range tables (init .data, zero .bss, for
// as many ranges as the chip declares, e.g. a separate pow2 app-data block under MPU
// enforcement). Call from the reset entry before the static ctors and arch_init; a chip whose
// linker script emits no tables need not call it. Runs before any global is live, so it must
// touch none of its own.
void kickos_ranges_init(void);

// Terminate the whole system with the given process/exit status. On the sim this ends the host
// process; on MCUs it halts.
void arch_shutdown(int status) __attribute__((noreturn));

// Reboot into the chip's bootloader (firmware-download mode). Returns -KOS_ENOSYS on a chip with
// no such entry; success never returns. The backend masks interrupts itself before handing over.
int arch_reboot(void);

// --- Core identity ----------------------------------------------------------
// The 0-based index of the core executing this code, in [0, KICKOS_NUM_CORES).
//
// At one core a function-like macro, so a single-core image carries no symbol for it;
// tests/static/check_cpu_id_fold.sh enforces that shape. The multi-core arm is a
// declaration, so a port raising KICKOS_NUM_CORES ships a definition or fails to link.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void);
#else
#define arch_cpu_id() 0u
#endif

// --- The cross-core doorbell ------------------------------------------------
// Poke the cores in `cores`, a bitmask of core indices, and wait until every core in it
// has answered. `cores` at 0 names nobody and both calls are then a no-op.
//
// The send is separate from the wait so an initiator can poke every core once and then wait
// once, which is what a rendezvous such as a TLB shootdown needs. A backend whose maintenance
// can invalidate and wait in its own instruction stream does that inside arch_aspace_map and
// arch_aspace_unmap and must NOT route it through the doorbell.
//
// The far side takes NO kernel lock, and the lock's own acquire loop services a pending
// doorbell: otherwise an initiator holding the lock waits on a core spinning to acquire it.
//
// At one core both are empty macros: the argument is consumed, never evaluated for effect.
#if KICKOS_NUM_CORES > 1
void arch_ipi_send(uint32_t cores);
void arch_ipi_wait(uint32_t cores);
#else
#define arch_ipi_send(cores) ((void)(cores))
#define arch_ipi_wait(cores) ((void)(cores))
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

// Discard every frame `ctx` holds and rebuild it so the thread resumes at `entry`, privileged,
// in thread mode, at the top of [stack_base, stack_base + stack_size). `ctx` must NOT be the
// running context: the fault path's arch_fault_redirect_to_exit is what rewrites a live one,
// and it also reads and clears sticky status registers that a scheduler-driven redirect must
// not touch.
//
// Idempotent in its values: `entry` and the stack top are absolute, so applying it twice before
// the thread resumes changes nothing.
//
// TOTAL: every backend answers, and the signature carries no failure return.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size);

// Switch the running context from `from` to `to`. MAY be deferred: a backend may book the swap
// for an exception return (the ARM backend does) or perform it now (the sim does). The
// scheduler must not assume the switch has completed when this returns.
//
// MUST be called either with interrupts masked or from ISR context. A backend is NOT required
// to apply the switch atomically, so an interrupt taken inside it may be delivered against a
// partially applied one. A backend may serve the ISR posture by booking the switch for the
// interrupt leg's exit.
void arch_switch(struct arch_context* from, struct arch_context* to);

// Enter the first thread from the boot context. `boot` is an optional save slot for the boot
// context: a backend MAY populate it (the sim does, so a later switch back unwinds to the host
// caller) or MAY ignore it and abandon the boot stack (the ARM backend does; the system always
// terminates via arch_shutdown, never by unwinding to boot). Callers MUST NOT switch back to
// `boot`.
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

// Running core clock in Hz (the CMSIS SystemCoreClock the chip tracks at PLL bring-up). 0 where
// the backend has no silicon core clock (the host sim).
uint32_t arch_cpu_clock_hz(void);

// Read-only branch-clock oracle: the peripheral branch clock in Hz feeding the register block
// whose base is `base`. `base` is the peripheral register-BLOCK base (e.g. a K64F UART at
// 0x4006A000); a backend MAY range-match within a block, but the contract only promises
// correctness for the block base itself. Returns 0 when this chip does not know the block's
// clock, never the core clock as a guess. The fallback TU returns 0 for every block. Read-only
// and cascade-free, so a rate change reaches a driver only when it asks again.
uint32_t arch_periph_clock_hz(uintptr_t base);

// Ungate the clock and drop the bus-side supervisor-protect for the register block at `base`.
// `base` is the peripheral register-block base and must match a per-chip table exactly;
// backends never range-match. Both bits are derived from `base`, so a caller cannot name a
// shared block's register or bit. Idempotent. Returns 0, -KOS_EINVAL (no entry for that base),
// or -KOS_ENOSYS (the fallback TU, no backend). A base has an entry only where the bus gate
// cannot open a kernel-reserved register, so a block whose gate reaches one is refused.
int arch_periph_enable(uintptr_t base);

// Write `value` to the register at `base + offset` on the caller's behalf, PRIVILEGED, for a
// register whose bus classifies its WRITE side supervisor-only (XMC4800 USIC FDR/BRG/CCR, RM
// Table 18-20 PV).
//
// `base` must match a per-chip allowlist entry exactly and `offset` must be one that entry
// names. A backend never range-matches and never admits a whole block.
//
// An entry's block MUST be clocked, powered and out of reset whenever the syscall can reach it,
// and the caller carries that: the seam validates alignment, wrap and possession. The store runs
// in the kernel's frame, so a fault on an unready block ends the system.
//
// Returns 0, -KOS_EINVAL (not on the allowlist, or `value` has a bit outside the entry's mask;
// the store is skipped, never masked) or -KOS_ENOSYS (no backend). The default declines and
// lives alone in arch/common/arch_periph_reg_write_default.cc.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// One-shot init-time pin-function config: point (port, pin) at the raw chip function code
// `func` (the PC/PCR encoding, opaque to the caller). Returns 0, -KOS_EINVAL (out of range), or
// -KOS_EBUSY (a kernel-owned pin the backend refuses). The fallback TU returns -KOS_ENOSYS so a
// non-empty board pin-map fails loud on a chip with no PORT/IOCR backend; a chip that owns its
// mux block (XMC4800, K64F) strong-overrides this.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func);

// Retune the core/bus clock to a P-state and return the actually landed core Hz, always the
// truth about where the clock now sits:
//   - a retune that fully succeeds returns the requested point's Hz;
//   - a retune that fails and parks on a safe fallback returns that fallback Hz: non-zero,
//     the clock did move, so the caller MUST run the coherence tail;
//   - 0 says this chip cannot change its clock at all (the fallback TU, unsupported backend).
// The backend performs the flash-wait-state / voltage step and the arch_clock_now re-anchor
// INTERNALLY, bracketing the exact PLL/divider write. MUST be called from privileged thread
// context with interrupts already masked by the caller and NOT from ISR context.
// Weak default returns 0.
//
// `target` carries a kos_pstate_t (sys/abi.h) as a plain u32; a backend that opts in includes
// sys/abi.h itself to name the KOS_PSTATE_* points.
uint32_t arch_cpu_clock_set(uint32_t target);

// Console coherence hooks (both no-op fallbacks):
//   arch_console_flush_sync: block until the TX shift register is fully idle
//     (transmission-complete, NOT merely buffer-empty). Entered under the caller's
//     IrqLock, and on a retune BEFORE the rate change, so no in-flight byte is still
//     clocking out at the OLD baud when the peripheral clock moves. On the shutdown
//     path it is what keeps arch_shutdown from stopping the core with a byte still in the
//     FIFO. EVERY chip whose console can outrun a shutdown needs a body here. A console
//     that hands each byte to its host inside the write call (semihosting SYS_WRITEC) has
//     nothing in flight, and the no-op fallback is the body there.
//     Must be bounded: it is on the panic and shutdown paths, where a wedged UART must
//     cost a dropped tail.
//   arch_console_retune: re-derive + reprogram the console baud from the CURRENT
//     SystemCoreClock, AFTER the clock has landed. Valid only where the clock actually
//     moved (achieved != previous).
void arch_console_flush_sync(void);
void arch_console_retune(void);

// --- Trace clock (telemetry timestamp seam) --------------------------------
// A dedicated high-resolution monotonic counter for telemetry timestamps, finer than
// arch_clock_now's nanoseconds. u32, so it WRAPS; the decoder reconstructs absolute time from
// the session anchors. The unit is the backend's own. A target with no such source leaves
// KICKOS_HAVE_TRACE_CLOCK undefined, and enabling telemetry there is a build-time FATAL.
uint32_t arch_trace_now(void);

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// Stamp the owning thread's trace id into a saved context, so the arch context-switch path
// emits {from,to} tids read from the physically-swapped contexts, NEVER by re-reading shared
// scheduler state, which an ISR can rewrite between the switch decision and the physical swap.
// Telemetry-only: the seam and the id field are both elided otherwise. Once per thread.
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

// Read at grant admission (kernel/grant), never on a commit path: a commit backend drops a
// region it cannot encode silently. Answers for the enforcement posture actually built and for
// the band the arena lives in, so a linker script that moves the arena into a differently cached
// band must revisit its chip's answer.
int arch_mpu_nocache_support(void);

struct arch_mpu_region
{
    uintptr_t base;
    size_t size;
    uint32_t attr; // OR of the ARCH_MPU_* bits
};

// Encode `n` regions into the descriptor words this backend programs, and report which of them
// got a descriptor as a bitmask (bit i for regions[i]). A region that fails
// arch_mpu_region_encodable gets none: the image never rounds a base or a size, so a misaligned
// request is refused here. Slots past `n` are written inactive, so an image encoded from a
// zero-length set grants nothing.
//
// Called at mutation, never on a switch path. Defined only where KICKOS_HAVE_MPU.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out);

// Load the running thread's regions on switch-in (replaces the whole active set). sim: mprotect
// over the user-RAM arena, granting the listed regions and no-access everywhere else. Regions
// are non-overlapping; attr is the unprivileged access (supervisor comes from the background
// region / SYSMPU RGD0).
//
// `image` is what the hardware is programmed from and is `regions` already encoded; the raw set
// travels beside it for the backends that need the addresses themselves (sim mprotect, RX
// same-set skip). It is null only where KICKOS_HAVE_MPU is 0.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image);

// Program the hardware from what arch_mpu_apply last recorded. On a backend whose context
// switch is DEFERRED, arch_mpu_apply only STASHES and this must run after the physical swap.
//
// Do NOT call it to make another thread's set live. The one sound direct use is the running
// thread's own region set, just widened and needing to be effective before the syscall
// returns (KOS_SYS_MEM_SELF_GRANT).
//
// Always resolves on every arch and both enforcement postures; an empty no-op where apply
// already programs the hardware (the sim) or where there is no MPU.
void kickos_arch_mpu_commit(void);

// This seam is a flat, NON-TRANSLATING protection-region set; a translating port gets the
// parallel arch_aspace_* family declared below.

// The smallest region this arch's MPU can enforce: ARM PMSA 32 bytes, RISC-V PMP NAPOT 8, one
// host page on the sim. A return of 0 means this arch has NO enforceable MPU (classic ESP32
// LX6, nRF51 M0) and allocations stay byte-granular. MUST return 0 or a power of two:
// arch_ram_region_size masks with min - 1.
size_t arch_mpu_min_region(void);

// Which of the two enforcing region-encoding modes this backend uses. Read only where
// arch_mpu_min_region() != 0.
// 1: size a power of two, base naturally aligned to it.
// 0: base and limit, so every arch_mpu_min_region() multiple is nameable.
// cmake/boot_arena.cmake scrapes this definition textually, so the body must stay a
// plain `return <integer>;` with no closing brace in any comment inside it.
int arch_mpu_region_pow2(void);

// True iff (base,size) is coverable exactly by one MPU descriptor with no rounding: the MMIO
// grant test, where a rounded window would over-grant the neighbouring registers. The sim
// returns false, mprotect mapping no real MMIO.
bool arch_mpu_region_encodable(uintptr_t base, size_t size);

// Round `want` up to the region size a backend can describe with one descriptor. arch_ram_alloc
// reserves this many bytes and the kernel sizes each thread/domain descriptor with the SAME
// call, so the descriptor matches the backing block exactly. This is the single point coupling
// allocation size to descriptor geometry; do NOT add a caller that treats the rounded value as
// usable capacity.
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

// Round up to a power of two.
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

// Natural alignment the block must sit on. In pow2 mode this is the region size itself, that
// mode snapping the base to it.
//
// Under KICKOS_TLS every block is strided by a power of two, whatever the descriptor geometry
// asks for: where the thread pointer is derived by masking SP down to the thread's own block, a
// block not starting on a multiple of that stride masks into its neighbour's.
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

// True iff a RAM block at (base,size) is nameable by one descriptor. The RAM test;
// arch_mpu_region_encodable is the MMIO one.
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

// The MPU-governed user-RAM pool. Domain data + unprivileged-thread stacks are placed here so
// per-domain isolation is enforceable. sim: an mmap arena; MCU: a linker-defined region.
// arch_ram_alloc reserves a block sized by arch_ram_region_size() and aligned to
// arch_ram_region_align(), so exactly one MPU region covers it. Returns null on exhaustion or
// size 0.
uintptr_t arch_ram_base(void);
size_t arch_ram_size(void);
void* arch_ram_alloc(size_t size);

// The shared, app-wide regions every UNPRIVILEGED thread needs just to run under enforcement:
// its code (RX) and its static data/.bss (RW, no-execute). An unprivileged thread gets no
// background-region default, so it faults fetching its own instructions and reading its own
// globals unless these are explicit regions. Fills up to `max` descriptors from the chip's
// linker-defined sections and returns the count, 0 where the arch models none of them. The
// kernel prepends these to an unprivileged thread's set (thread.cc), then the domain regions
// and stack.
size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max);

// True iff [ptr, ptr+len) is app code/rodata/.data the backend recognizes as caller-readable
// but does NOT describe as one of the running thread's MPU regions. The confused-deputy floor
// (syscall_dispatch) reads a user buffer/name privileged; it first checks the granted regions
// and, only if that misses, this hook, so the two together cover exactly what the UNPRIVILEGED
// caller could itself reach.
//   enforcing MPU backend: the region check already admits code/rodata/.data as real regions
//     (arch_domain_static_regions), so this answers false.
//   non-enforcing backend: the kernel dereferences the range PRIVILEGED, so it must be MAPPED.
//     Admit the chip's linker-defined memories, code/rodata extent and static RAM up to the
//     arena base, and leave an arena range to the region check.
//   host sim: admit a range wholly inside the host image and clear of the arena.
bool arch_user_text_readable(uintptr_t ptr, size_t len);

// The WRITE twin of the hook above: true iff [ptr, ptr+len) is app static data the backend
// recognizes as caller-writable but does NOT describe as one of the running thread's MPU
// regions. user_writable_ok checks the granted regions first and falls back here, exactly as
// user_readable_ok does.
//   enforcing MPU backend: the region check already admits .appdata/.appbss as a real region,
//     so this answers false.
//   non-enforcing backend: the kernel stores privileged, so admit the chip's static-RAM extent,
//     RAM origin up to the arena base, and leave an arena range to the region check.
//   host sim: admit a range wholly inside the host image and clear of the arena.
bool arch_user_data_writable(uintptr_t ptr, size_t len);

// An address that faults on unprivileged access (sim: a reserved arena page no domain owns).
// Used by the isolation self-test.
uintptr_t arch_mpu_probe_addr(void);

// --- Address space: a translating memory backend ----------------------------
// The parallel family beside the region calls. A chip selects region descriptors or
// translation, and no backend implements both.
//
// Opaque: the kernel names a space by pointer and never sizes or embeds one. Translation tags
// are the backend's, allocated and invalidated inside destroy and activate.
struct arch_aspace;

// The access a mapping grants the UNPRIVILEGED level. At least one bit is required; a guard page
// is an unmapped page.
enum
{
    ARCH_MAP_R = 1u << 0,
    ARCH_MAP_W = 1u << 1,
    ARCH_MAP_X = 1u << 2
};

// A physical address, sized independently of uintptr_t because the physical width does not
// track the virtual one in either direction: a regime may output more bits than a pointer
// holds, and may output fewer, so neither width bounds the other.
typedef uint64_t arch_phys_addr_t;

// A memory type, never the bits a backend encodes it to.
enum arch_map_memtype
{
    ARCH_MAP_NORMAL = 0,  // cacheable, write-back
    ARCH_MAP_NOCACHE = 1, // Normal, outer and inner non-cacheable
    ARCH_MAP_DEVICE = 2   // device / MMIO
};

// Whether this backend can honour `type`. Read at grant admission, never on a map path: a
// quietly downgraded type hands a driver a cacheable view of a DMA buffer. The answer may come
// from the board.
bool arch_aspace_memtype_support(enum arch_map_memtype type);

// TOTAL OR FAIL: on anything but ARCH_ASPACE_OK no partial mapping is installed. A failed map
// leaves its range unmapped, and a frame under a pre-existing leaf the rollback clears is
// leaked, no leaf being left to name it at destroy.
enum arch_aspace_result
{
    ARCH_ASPACE_OK = 0,
    // No frame for a table the backend needed. Freeing frames and retrying can succeed.
    ARCH_ASPACE_ENOMEM = 1,
    // The backend's structure cannot hold the mapping with frames still available, so
    // retrying is futile.
    ARCH_ASPACE_ECAPACITY = 2,
    // Not expressible: a misaligned or empty range, a right or type this backend refuses,
    // or an unmap of a range not wholly mapped.
    ARCH_ASPACE_EINVAL = 3
};

// The mapping granule in bytes, a power of two, and the unit `va`, `pa` and `pages` are counted
// in. The frame allocator and the guard-page arithmetic read this one answer.
size_t arch_aspace_granule(void);

// A space with the kernel's fixed high range present and nothing else mapped, or null when a
// root cannot be allocated. How much of that range a backend copies into a new root is its own
// choice, but a later edit of the kernel range must reach every space.
struct arch_aspace* arch_aspace_create(void);

// Release the root, every table under it and every frame the space still holds, with the
// backend's invalidation ordered BEFORE the root or its tag can be reused. Null is a no-op.
// Activate another space before destroying the running one.
//
// Holds means mapped, so a space must not still map a frame it does not own
// when this runs: a borrower of a handoff unmaps its range first, or the
// frame is freed twice.
void arch_aspace_destroy(struct arch_aspace* space);

// Map `pages` granules at `va` onto the frames at `pa`, or remove that many at `va`.
//
// COHERENCE-COMPLETE: when either returns the change is visible to this core, whatever
// maintenance that took having happened inside. A map into a slot that was empty owes that
// maintenance too.
//
// At more than one core the initiator waits. An unmap whose frames are being FREED may defer
// the remote half, the boundary being reuse; an unmap that REVOKES may not, and the far-side
// handler takes no kernel lock.
//
// A map may target the space this core is RUNNING on: the self-grant widens it mid-syscall.
//
// A failed map leaves its range unmapped: the rollback clears every leaf from `va` up
// to the first page that has none, whichever call installed it. Do NOT map over a
// partially mapped range: this rollback owes a narrower form before that is sound.
enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages,
                                        uint32_t rights, enum arch_map_memtype type);
enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va,
                                          size_t pages);

// Make `space` the translation the unprivileged level runs under.
//
// TOTAL: every backend answers and the signature carries no failure return.
//
// Where a root and a translation tag change together the architecture documents the ORDER.
//
// The space is reached from the incoming thread's task; arch_context_init carries no memory
// parameter.
void arch_aspace_activate(struct arch_aspace* space);

// Reach the frame backing one page of `space` through a kernel-usable pointer, and release it.
//
// ARCH_ASPACE_ACQUIRE_MIN are holdable at once per core, and a backend with a finite window
// pool sizes that pool for the figure and asserts against it. Holds are counted as outstanding
// calls: two acquires of one page are two holds unless the backend counts references.
//
// `va` need not be granule-aligned and the pointer addresses the same byte. Null when the page
// is not mapped in `space`. A range contiguous in virtual memory is not contiguous in physical
// memory, so a caller splits at granule boundaries and acquires each page.
//
// An address the space may legitimately name, which is the range map and unmap take, and null
// for anything else. Each backend states that range ONCE, in the predicate its own map and unmap
// already ask, and its shape differs per backend. A RELEASE outside the range is refused, and a
// backend must count it as neither a hold nor a mispairing: an address no space may map held no
// slot.
//
// A release pairs with an acquire that ANSWERED: a null answer took no hold, and releasing
// beside one surrenders somebody else's. A caller checks each acquire before it releases
// anything.
#define ARCH_ASPACE_ACQUIRE_MIN 6u
void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va);
void arch_aspace_release(struct arch_aspace* space, uintptr_t va);

// NAME the frame backing one page of `space`: the granule-aligned physical address the page
// holding `va` is mapped onto, and 0 when that page is not mapped. `va` is aligned down before
// the question, so an unaligned address is admitted and the offset inside the granule is dropped.
//
// An address the space may legitimately name, which is the range map and unmap take, and 0 for
// anything else, on the terms arch_aspace_acquire states above. A backend's own diagnostic probe
// takes the kernel half from a private walk.
//
// 0 means not mapped, so no low-half page may be mapped onto physical frame 0;
// kernel/mem/aspace.cc's aspace_frame_token and the arms in kernel/syscall/syscall_aspace.cc
// read 0 that way.
//
// Owed to the first backend that installs a block or superpage in the range a space may map: a
// level-aware walk that resolves the leaf at whatever level it stands and answers its output
// plus the granule-aligned offset of `va` inside that leaf's span.
//
// Spends no acquire hold and reads no frame's contents, so a caller may compare many pages.
//
// Subtracting two acquire pointers names a frame only where acquire is an addition; on a backend
// windowing a handful of slots that comparison succeeds wrongly.
arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va);

// SELFTEST ONLY: what the machine reports about the translation this port programs.
//   bits 0..7    ARCH_ASPACE_MODEL_* : one per figure of this port's that the machine bore out
//   bits 8..15   the address-space identifier width reported, in bits
//   bits 16..23  the physical address range reported, in bits
//   bits 24..31  one bit per granule reported supported, bit N naming the Nth granule the
//                ARCHITECTURE defines, smallest first (A64: 4 KiB, 16 KiB, 64 KiB)
#define ARCH_ASPACE_MODEL_GRANULE 0x01u /* the granule this port programs is supported */
#define ARCH_ASPACE_MODEL_ASID    0x02u /* the identifier is as wide as the port's record */
#define ARCH_ASPACE_MODEL_PA      0x04u /* the physical range covers what the port programs */
#define ARCH_ASPACE_MODEL_ALL     0x07u
#define ARCH_ASPACE_MODEL_ASID_SHIFT 8u
#define ARCH_ASPACE_MODEL_PA_SHIFT   16u
#define ARCH_ASPACE_MODEL_GRAN_SHIFT 24u
#define ARCH_ASPACE_MODEL_FIELD_MASK 0xFFu
uint64_t arch_aspace_model(void);

// The space the boot path installed, and the ONLY handle for it: its tables are link-time
// constants. The kernel installs it when the running process's space is destroyed under it, a
// freed root left in the translation control being a walk into the frame pool.
struct arch_aspace* arch_aspace_boot(void);

#if defined(KICKOS_ENABLE_SELFTEST)
// The backend's map-editor bookkeeping since boot, three fields in one word:
//   63..32  page-invalidation sequences ISSUED
//   31..8   the ones elided, saturating at the field width
//    7..0   window releases that named no hold and a frame outside the kernel window
// The low byte is a defect record: it must be 0, and a self-test arm is the only thing that
// says so.
uint64_t arch_aspace_tlbi_counts(void);
#endif

// --- Data-cache maintenance for an observer that does not snoop -------------
// Make this core's writes over [addr, addr + bytes) visible to an observer outside the
// coherency the CPU participates in, and make such an observer's writes over that range
// visible to this core.
//
// The backend reads its own line size and rounds the range out to it, so a caller never has a
// figure to keep in step.
//
// The invalidate cleans as it invalidates, a range whose ends fall inside a line sharing those
// lines with its neighbours.
//
// `addr` is a kernel-usable pointer, which is what arch_aspace_acquire answers. Splitting a user
// virtual range into pages belongs above this seam.
//
// There is no fallback definition: a port defines these, and a reference fails the LINK
// until it does. arch_dcache_flush leaves the lines valid, so this core keeps reading them
// from cache.
void arch_dcache_flush(void const* addr, size_t bytes);
void arch_dcache_invalidate(void* addr, size_t bytes);

// --- Rule 7: kernel-reserved MMIO blocks ------------------------------------
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

// Fill `out` (capacity `max`, the kernel passes KICKOS_MAX_RESERVED) with this chip's reserved
// blocks and return the count. KICKOS_RESERVED_NONE (0) is legal, the sim owning nothing
// MPU-governable. Every definition is per chip under #if KICKOS_MEMORY_ENFORCED, the fact that
// protection is live, which a translating chip sets too. A port that declares no set fails to
// link.
#define KICKOS_RESERVED_NONE 0u
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max);

// Nonzero on a core with the Cortex-M bit-band peripheral/SRAM alias (M3/M4): a reserved
// peripheral block is then ALSO reachable through its word-per-bit alias image, and a device
// grant touching either alias window is refused (kernel/grant). Fallback TU answers 0; a chip
// with the alias strong-overrides to 1.
int arch_bitband_present(void);

// --- Syscall trap (user -> kernel) -----------------------------------------
// Issued by the userspace syscall stubs; returns the syscall result.
//
// CONTRACT (portability-critical): the arch MUST run syscall_dispatch() in privileged THREAD
// context on the calling thread's own continuation, NOT in ISR/handler context. A blocking
// syscall (sem_wait, sleep, ...) blocks by an ordinary synchronous context switch: arch_switch
// completes and the dispatch resumes inline when the thread is next scheduled. The kernel's
// blocking primitives depend on this, and arch_in_isr() must read false during dispatch. 64-bit
// ARGUMENTS are split into uintptr_t halves (see sys/abi.h).
//
// The result comes back at the ABI's 64-bit return width: arch_syscall takes the low half,
// arch_syscall64 both. They are the SAME trap, carried in whatever register pair the psABI uses
// for a long long return, so a backend that preserves that pair serves both.
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

// The same trap, spelled from kernel text. Where a translating backend splits the image, the
// trap above links into the app's half and kernel text may not call it; the backend assembles
// a second copy under these names, in its own half, which every address space maps by
// construction. Spell these in kernel code and the trap is correct on every backend; where
// the image is not split the names alias the ones above.
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
// Declared only under KICKOS_ARCH_HAS_IPC_FASTPATH.
//
// `io` is KOS_CALL_REG_WORDS + 3 words, in and out over the SAME storage:
//   in  io[0]=nr io[1]=ep_cap io[2]=packed lens io[3..] = request payload
//   out io[1..] = reply payload; the return value is the call's result
// The trap must preserve `io` itself across the trap instruction.
//
// KOS_CALL_REG_FALLBACK says the fastpath declined and the caller must re-issue through
// KOS_SYS_CALL.
#if KICKOS_ARCH_HAS_IPC_FASTPATH
int32_t arch_syscall_reg(uint32_t* io);

// Store a syscall result into a SAVED context, for a thread the fastpath parked with no kernel
// continuation to return through. The arch writes it where its own restore path reloads the
// syscall's return register from. The context must not be live in any register file when
// this runs.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result);
#endif

// --- Interrupt controller (thin abstraction: mask / unmask / raise) --------
// The whole seam is delivery gating: mask, unmask, inject, clear_pending.
//
// mask/unmask gate delivery of a line. The generic first-level ISR masks the line before waking
// its driver, which unmasks via irq_ack once serviced. A raise that lands on a MASKED line is
// latched one-deep (coalesced), never dropped, and is redelivered through the normal ISR path
// the instant the line is unmasked.
//
// RESET CONTRACT: all lines start MASKED at reset, and nothing may assume a line is deliverable
// until it has been unmasked (arch_irq_unmask, or irq_claim which arms it).
//
// LOCKING CONTRACT: mask, unmask, inject and clear_pending are SELF-BRACKETED, each doing its
// own interrupt-masked critical section over its state RMW, so a caller need not hold IrqLock.
//
// SINGLE-DOORBELL CONTRACT. A backend with no hardware pending state for a logical line carries
// a coalesced redelivery through ONE physical doorbell, clearing the per-line pending bit as it
// rings. Where the identity travels in one shared cell, at most one unmask with a pending
// redelivery may occur per IrqLock region: a second clobbers the first's identity and loses an
// event. irq_claim/wait/ack each unmask exactly one line per lock section, and portable code
// obeys that whichever backend it is on. One shared cell: sim, rv32imac, rv64imac, xtensa,
// rxv3-soft. A pending bitmap the doorbell handler drains, holding every line rung inside one
// masked region: x86_64. The lists are hand-maintained, and portable code assumes the shared
// cell on a backend that appears in neither.
void arch_irq_mask(int line);

// Enable delivery of a line, preserving any raise latched while it was masked: a latched raise
// fires through the normal first-level ISR path (kickos_isr_irq) after enable, never a direct
// notification from unmask itself.
void arch_irq_unmask(int line);

// Discard any raise latched on a line (best-effort: a controller that cannot drop a native
// pending, e.g. the PLIC for a real device line, no-ops there). The explicit discard
// primitive, for dropping pre-registration garbage at first arm, and reserved for the M4
// level-trigger rearm path.
void arch_irq_clear_pending(int line);

// Raise device line `irq` (the controller's "raise"), so the ISR runs in interrupt context.
// Drives scheduler trigger #4. A real driver never raises; it register/wait/acks. Raising is
// fake-a-device-firing test scaffolding, privilege-gated.
void arch_irq_inject(int irq);

// --- Minimal debug console (bottom edge of the in-kernel console driver) ---
// Write-only. Two edges:
//   arch_console_write:      normal path. A chip with a buffered console makes
//                              this enqueue + prime the TX IRQ (see console_tx.h);
//                              otherwise it is the polled writer.
//   arch_console_write_sync: polled, bounded; safe with the scheduler/IRQs down.
//                              Panic / fault / assert / pre-arm output uses this.
//                              Defaults to arch_console_write through a lone-TU fallback;
//                              a chip with a buffered console defines its own polled writer.
void arch_console_write(char const* buf, size_t n);
void arch_console_write_sync(char const* buf, size_t n);

// Force the UART back to a known polled-ready channel on the panic path after a userspace
// console driver may have left its granted register window garbled (D6). A chip that supports
// handover overrides this with an idempotent full-window register rewrite; the fallback TU is a
// no-op.
void arch_console_reclaim(void);

// The register window arch_console_reclaim writes, i.e. the device a userspace console driver is
// granted on this chip. *size == 0 (the fallback TU) says this chip names no window.
//
// The window is the ARCH's answer, never an address userspace supplied: the reclaim is about to
// reprogram exactly these registers.
void arch_console_reclaim_window(uintptr_t* base, size_t* size);

// --- Single on-board kernel diagnostic LED (optional) ----------------------
// The board's one diagnostic LED, the raw bottom edge of kdiag_led_*: it must work UART-less, in
// a fault, before drivers exist. arch_diag_led_init() configures the pin once at boot;
// arch_diag_led_set() drives it (on != 0). Raw set, no toggle: the kernel side tracks the state.
// Both have a no-op fallback-TU default (kernel/init/led.cc).
void arch_diag_led_init(void);
void arch_diag_led_set(int on);

// --- Chip-specific fault decode (optional) ----------------------------------
// Runs after the core fault reporter has dumped the CPU frame and fault-status registers, so
// a chip whose isolation trap surfaces elsewhere adds its own capture. Fallback-TU no-op
// default.
void arch_fault_report_extra(void);

// --- Fault isolation: the faulting thread dies and the system runs on ------
// `frame` is the backend's own fault frame (whatever its handler already holds),
// opaque to the core and never dereferenced outside these two.
//
// Did this fault happen in UNPRIVILEGED THREAD context? The answer is the CPU's privilege at
// fault time, syscall dispatch running privileged on the thread's own stack. Fallback-TU
// default: false, so a backend that has not opted in panics.
//
// A backend whose fault frame lives in MEMORY on the faulting thread's own stack must gate on
// kickos_fault_frame_trusted BEFORE it reads a single word of that frame: a wild SP hands the
// handler a frame the thread never produced. Facts read out of a REGISTER (privilege, cause,
// fault-status) are valid whatever the SP holds.
bool arch_fault_is_user_thread(void* frame);

// Rewrite `frame` so the exception return lands in kickos_thread_fault_exit, privileged, in
// thread mode, at the TOP of the faulting thread's own stack (kickos_fault_stack_top), and hand
// the fault facts to kickos_fault_record for the stub to print in thread context. The thread is
// dying, so its register values need not be preserved. Valid ONLY where
// arch_fault_is_user_thread returned true; fallback-TU default: empty.
void arch_fault_redirect_to_exit(void* frame);

// --- Idle -------------------------------------------------------------------
// Block until the next interrupt (ARM WFI; sim sigsuspend).
void arch_idle_wait(void);

// --- Provided by the kernel for the arch backend ---------------------------
// Next-event timer expired (tickless deadline or, if enabled, periodic tick).
void kickos_isr_timer(void);
// Device interrupt line `irq` fired (sim: injected event; ARM: NVIC line).
void kickos_isr_irq(int irq);
// A thread's entry function returned; the arch trampoline routes here.
void kickos_thread_return(void) __attribute__((noreturn));
// The arch-independent syscall table dispatch. Sign rule: an errno arm returns a NEGATED int
// and sign-extends, so it stays negative read at any width; every other arm zero-extends.
// The low half is what a 32-bit target always saw.
uint64_t syscall_dispatch(uintptr_t nr,
                          uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
// A memory-protection violation was caught (sim: SIGSEGV over the arena).
void kickos_isr_fault(uintptr_t addr, int is_write);

// One frame for a translating backend's own tables, and its release. One allocator exists and
// the kernel owns it: a backend keeps no pool of its own.
//
// Physical, destroy reading what it frees out of a descriptor. The frame is granule-sized and
// granule-aligned; its contents are undefined, so a backend that needs zeroes writes them. 0 is
// exhaustion, which the caller maps to ARCH_ASPACE_ENOMEM. Compiled where a translating backend
// is (KICKOS_HAVE_ASPACE).
arch_phys_addr_t kickos_frame_alloc(void);
void kickos_frame_free(arch_phys_addr_t frame);

// Fault isolation. Call from the arch fault handler BEFORE it starts its dump. Applies the
// kill rule and, when it holds, calls arch_fault_redirect_to_exit. Returns true when the
// handler must simply RETURN: the exception return then lands in kickos_thread_fault_exit.
// False means the fault panics.
bool kickos_fault_kill_thread(void* frame);

// Does [frame, frame + bytes) lie inside the RUNNING thread's own stack? The arch-neutral
// frame-validity guard: a frame written anywhere else came from an SP the thread had no business
// holding, so nothing in it may be believed and the fault must panic. Fails closed on no current
// thread, on idle, and on a thread with no recorded stack. Call it even where a cheaper
// arch-specific early-out already fired; neither test subsumes the other.
bool kickos_fault_frame_trusted(void const* frame, size_t bytes);

#if KICKOS_KERNEL_STACKS
// Does [frame, frame + bytes) lie inside the RUNNING thread's own KERNEL stack block? The same
// guard for a backend whose trap entry builds its frames there: a frame outside the block came
// from some other sp and nothing in it may be believed. Fails closed on no current thread, on
// idle, and on a thread with no block seated (ctx.kernel_sp 0). Compiled only where the blocks
// exist.
bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes);
#endif

// Top (exclusive) of the RUNNING thread's own stack, or 0 when there is none to name (no
// current thread, idle, or no recorded stack). Where a backend's redirect puts the SP, so the
// stub runs with the whole stack under it: exit_current, cap_teardown and the fault print all
// need headroom there. A backend must still
// establish that these fields describe the stack the stub will run on
// (kickos_fault_frame_trusted on the fault's own SP) before it uses this value.
uintptr_t kickos_fault_stack_top(void);

// Did the faulting access land in the guard band immediately below the running thread's stack,
// i.e. did the thread run off the bottom of its own stack? Where an exception cancels the
// faulting instruction and restores SP (RXv3), the faulting address is the only evidence of an
// overflow that kickos_fault_frame_trusted can see. Fails closed (true) when there is no
// recorded stack to compare against. KICKOS_FAULT_STACK_GUARD_BAND is the width.
bool kickos_fault_below_stack(uintptr_t addr);

// The facts arch_fault_redirect_to_exit captured, printed later by kickos_thread_fault_exit in
// thread context. NOTHING may print from the handler: printing there forces kpanic_enter, which
// masks interrupts and reclaims the console permanently. `status_name` names the arch
// fault-status word for the reader (armv7m: "CFSR").
// `addr` is read only when `addr_valid`, since a fault-address register holds stale contents
// otherwise.
//
// `status` is 64-bit because a status register is that wide on the arches that have one;
// narrower backends promote and pay nothing.
void kickos_fault_record(char const* status_name, uint64_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid);

#if defined(KICKOS_ENABLE_SELFTEST)
// Trap-stack regression: names kickos_trapstack_witness if an unprivileged thread's trap frame
// reached that kernel word. Call from a backend's PANIC path, after kpanic_enter, so the
// faultsurvive `kwrite' gate can prove the trap prologue did NOT store through a U-mode sp into
// kernel memory. Silent when intact.
void kickos_trapstack_witness_report(void);

// NESTED-TRAP regression: which stack the kernel picked for an interrupt taken while the kernel
// was ALREADY running. The claim is that a frame built there is never the interrupted THREAD's
// own stack. Call once per such trap with the frame the backend built and the interrupted
// thread's stack bounds (`lo` 0 when there is no current thread). KOS_SYS_NEST_WITNESS reads the
// counters, a kprintf on the shutdown path putting the console's varargs route inside the
// syscall descent it stands beside.
void kickos_nestwitness_note(uintptr_t frame, uintptr_t lo, uintptr_t hi);
uint32_t kickos_nestwitness_count(int which);
#endif

// Where arch_fault_redirect_to_exit points the faulting thread. Runs privileged, in thread
// mode, on that thread's own stack.
void kickos_thread_fault_exit(void) __attribute__((noreturn));

// Contain the thread whose live stack pointer a trap prologue just refused. Raises it to
// CANCEL_SLAY and hands back the context to resume, and the system runs on.
//
// `offender` names the thread physically executing, and the caller must pass what the arch
// layer switched TO and not the scheduler's own current: a switch booked before the trap has
// already published the incoming thread there, so current names the wrong thread on every
// asynchronous refusal.
//
// The offending thread may well be the one resumed: switch_book rebuilds its context onto the
// exit stub at the top of a stack the TCB names, and its registers are discarded.
//
// `slain_name`, when not null, receives the slain thread's name on success, and is untouched
// when this returns null. Read it on the caller's own trap or interrupt stack, never in the exit
// stub, whose descent is bound against every thread's stack floor by the RET class.
//
// Returns null wherever the caller must still terminate: no thread owns `offender` (which is
// what refuses idle, outside the pool), it is privileged, the slay could not claim it, or
// nothing can run. Takes the kernel lock itself; the trap prologues that reach it run unmasked.
struct arch_context* kickos_thread_contain_wild_stack(struct arch_context* offender,
                                                      char const** slain_name);

// Where arch_ctx_redirect points a SLAIN thread. Runs privileged, in thread mode, at the top of
// that thread's own stack, and prints nothing. `arg` is always null and exists only to match the
// entry signature arch_ctx_redirect fabricates against.
void kickos_thread_slay_exit(void* arg) __attribute__((noreturn));

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// A context switch physically completed: emit a SWITCH record {from_tid, to_tid}. Call at the
// real register swap, with tids read out of the two contexts swapped, NEVER by re-reading
// shared scheduler state. from_tid == 0xFFFF on the very first switch. RESCAN group.
void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid);

// Emit the closing SESSION record (final records_attempted plus a second clock anchor), from
// arch_shutdown and before the backend drains its ring, so the decoder gets its two-anchor
// resync span.
void kickos_trace_final_session(void);

// Print a one-line telemetry health report (attempted vs dropped) to the console at shutdown; a
// CI gate reads it to verify the drop accounting.
void kickos_trace_report_counters(void);
#endif

} // extern "C"

#endif
