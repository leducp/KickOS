// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Architecture-neutral KickOS porting interface.

#ifndef KICKOS_ARCH_ARCH_H
#define KICKOS_ARCH_ARCH_H

#include <stddef.h>
#include <stdint.h>

// Core counts default to one when no board configuration is included.
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif

#ifndef KICKOS_NUM_CORES
#define KICKOS_NUM_CORES 1
#endif

// Cores scheduled by one kernel; KICKOS_NUM_CORES counts cores driven by the image.
#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

// Isolated cores run only threads whose explicit affinity mask includes them.
// Applies only to the shared-kernel model.
#ifndef KICKOS_ISOLATED_CORES
#define KICKOS_ISOLATED_CORES 0
#endif

// Mask of this kernel's cores. Avoid shifting by 32 for a 32-core kernel.
#define KICKOS_CORE_SET_ALL (~0u >> (32 - KICKOS_KERNEL_CORES))

// Whether one kernel spans every core. A model, independent of the core count.
#ifndef KICKOS_MULTICORE_MODEL_SHARED
#define KICKOS_MULTICORE_MODEL_SHARED 1
#endif

// Whether this image belongs to an AMP partition. Set by the board, not
// derived from core count: a single-core image can still have AMP peers.
#ifndef KICKOS_AMP_NODE
#define KICKOS_AMP_NODE 0
#endif

// Whether each AMP node has its own image.
#ifndef KICKOS_AMP_OWN_IMAGE
#define KICKOS_AMP_OWN_IMAGE 0
#endif

// Several AMP nodes share one image; the core register identifies each node.
#define KICKOS_AMP_SHARED_IMAGE (KICKOS_AMP_NODE && !KICKOS_AMP_OWN_IMAGE)

// Per-arch definition of `struct arch_context` (opaque to the kernel; sized by the arch).
// Resolved to arch/<arch>/include/kickos/arch/context.h.
#include <kickos/arch/context.h>

// Architecture-specific MPU descriptors. Included only with KICKOS_HAVE_MPU;
// otherwise the type stays incomplete and is used only through pointers.
#if KICKOS_HAVE_MPU
#include <kickos/arch/mpu_encoded.h>
#else
struct arch_mpu_encoded;
#endif

// C++ only: extern "C" below is unguarded.
extern "C"
{

// --- One-time backend bring-up ---------------------------------------------
// Called exactly once, before the kernel runs.
void arch_init(void);

// Initialize data and BSS from the linker copy/zero tables before constructors
// and arch_init. Do not access this code's globals before initialization.
// Targets without these tables need not call this.
void kickos_ranges_init(void);

// Terminate the whole system with the given process/exit status. On the sim this ends the host
// process; on MCUs it halts.
void arch_shutdown(int status) __attribute__((noreturn));

// Reboot into the chip's bootloader (firmware-download mode). Returns -KOS_ENOSYS on a chip with
// no such entry; success never returns. The backend masks interrupts itself before handing over.
int arch_reboot(void);

// Enter the panic reporter on its per-core stack. In order:
// 1. Mask interrupts before changing stacks.
// 2. Load SP from the fourth argument register; leave nothing on the old stack.
// 3. Preserve the first three arguments.
// 4. Branch to kickos_panic_report without a call edge.
// Keep all state in registers, never shared BSS: cores may panic together.
// Implement in assembly except on ARCH_SIM. The branch keeps the reporter
// out of trap red-zone callgraph bounds. No fallback is provided.
void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line,
                              uintptr_t top) __attribute__((noreturn));

// Panic reporter, entered on the panic stack. Exactly one of msg and file
// is non-null; line is meaningful only with file.
void kickos_panic_report(char const* msg, char const* file,
                         unsigned line) __attribute__((noreturn));

// Return the executing core index in [0, KICKOS_NUM_CORES).
// The single-core macro emits no symbol; multicore ports must define the function.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void);
#else
#define arch_cpu_id() 0u
#endif

// Send a doorbell to each core in the mask, then wait for every reply.
// A zero mask is a no-op. Separate send/wait calls allow one batch of requests.
// Translation maintenance belongs in map/unmap; use hardware broadcasts when
// available. On ARM64, peers also need an ISB when executable mappings are
// removed or replaced. The doorbell handler supplies that synchronization.
// Handlers must not take the kernel lock; lock acquisition must service
// pending doorbells to prevent deadlock. Publish data before sending: GICv2
// SGI generation provides no ordering (IHI 0048B.b).
// AMP nodes need doorbells even with one local core. The no-op macros below
// consume their arguments without evaluating them.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
void arch_ipi_send(uint32_t cores);
void arch_ipi_wait(uint32_t cores);

// Full store-to-load barrier. Release/acquire alone does not order an earlier
// store against a later load; AMP startup needs this ordering on both nodes.
void arch_ipi_fence(void);
#else
#define arch_ipi_send(cores) ((void)(cores))
#define arch_ipi_wait(cores) ((void)(cores))
#define arch_ipi_fence() ((void)0)
#endif

// Raise this core's doorbell to deliver a pending reschedule after unmasking.
#if KICKOS_KERNEL_CORES > 1
void arch_ipi_resched_self(void);
#else
#define arch_ipi_resched_self() ((void)0)
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
// Seat value for backends with no per-core controller publication state.
#define ARCH_IPI_SEAT_NONE 2u

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
// Per-core doorbell counts: services in bits 31:0, instruction rendezvous
// initiated in bits 63:32. Return zero for a core outside the built range.
uint64_t arch_ipi_counts(uint32_t core);

// Raises deferred because the target controller state was not published.
// Return zero when the backend needs no publication.
uint32_t arch_ipi_deferred(uint32_t core);

// Return the previous seat value so callers can restore it. Never seat an
// unpublished controller: affinity zero names a real core. ARCH_IPI_SEAT_NONE
// means no publication state, not an unseated controller.
uint32_t arch_ipi_seat_set(uint32_t core, uint32_t seated);
#else
#define arch_ipi_counts(core) ((void)(core), 0ull)
#define arch_ipi_deferred(core) ((void)(core), 0u)
#define arch_ipi_seat_set(core, seated) ((void)(core), (void)(seated), ARCH_IPI_SEAT_NONE)
#endif
#endif

// Primary AMP node only: start the other nodes at their partition image bases.
// Do not zero shared memory here; reset cleared it before nodes published data.
#if KICKOS_AMP_OWN_IMAGE
void arch_amp_release_peers(void);
#else
#define arch_amp_release_peers() ((void)0)
#endif

// Cross-core kernel lock covering capability resolution through use.
// Nonrecursive. Callers mask local interrupts separately to prevent re-entry.
// Acquisition must service pending doorbells while spinning.
#if KICKOS_KERNEL_CORES > 1
void arch_kernel_lock(void);
void arch_kernel_unlock(void);
#else
#define arch_kernel_lock() ((void)0)
#define arch_kernel_unlock() ((void)0)
#endif

// Build ctx to enter entry(arg) on [stack_base, stack_base + stack_size).
// privileged selects kernel or user mode. If entry returns, call kickos_thread_return().
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void* arg), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged);

// Rebuild a nonrunning context to enter entry in privileged thread mode at
// the stack top. Idempotent and infallible on every backend. Do not access
// fault status registers; arch_fault_redirect_to_exit handles live contexts.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size);

// Switch from from to to, immediately or at exception return. The scheduler
// must allow deferred completion. Call with interrupts masked or from an ISR;
// backends need not make the switch atomic.
void arch_switch(struct arch_context* from, struct arch_context* to);

// Enter the first thread. boot is an optional context save slot that backends
// may ignore. Callers must not switch back to it.
void arch_start(struct arch_context* boot, struct arch_context* first);

// --- Critical section (RAII-wrapped by kernel IrqLock) ---------------------
typedef uintptr_t arch_irq_state_t;
// An arch whose mask or unmask is a couple of instructions defines that half in its own
// kickos/arch/irq_inline.h and marks which half it took, so IrqLock carries no call for it.
// A half no header takes stays an ordinary out-of-line seam, and so does every half on an
// arch shipping no such header: the host unit fixtures answer this seam with definitions of
// their own, and an inline body would take that substitution away from them.
#if defined(__has_include) && __has_include(<kickos/arch/irq_inline.h>)
#include <kickos/arch/irq_inline.h>
#endif
#ifndef KICKOS_ARCH_IRQ_SAVE_INLINE
arch_irq_state_t arch_irq_save(void);
#endif
#ifndef KICKOS_ARCH_IRQ_RESTORE_INLINE
void arch_irq_restore(arch_irq_state_t state);
#endif

// Nonzero while executing in interrupt/ISR context.
int arch_in_isr(void);

// --- Tickless clock + one-shot next-event timer ----------------------------
uint64_t arch_clock_now(void); // monotonic nanoseconds
void arch_timer_arm(uint64_t deadline_ns);
void arch_timer_disarm(void);

// Running core clock in Hz (the CMSIS SystemCoreClock the chip tracks at PLL bring-up). 0 where
// the backend has no silicon core clock (the host sim, a QEMU virt guest).
uint64_t arch_cpu_clock_hz(void);

// Return the peripheral register block's clock in Hz, or zero if unknown.
// Only the exact block base is guaranteed. Read-only: drivers must query
// again after a rate change. The fallback returns zero.
uint32_t arch_periph_clock_hz(uintptr_t base);

// Ungate and remove bus supervisor protection for an exact allowlisted block
// base. Refuse gates that expose kernel-reserved registers. Idempotent.
// Return 0, -KOS_EINVAL for an unknown base, or -KOS_ENOSYS without a backend.
int arch_periph_enable(uintptr_t base);

// Write value to a privileged register at base + offset. Both must match
// the chip allowlist exactly. Validate alignment, overflow and ownership.
// The caller must ensure the block is clocked, powered and out of reset:
// a fault during this kernel store terminates the system.
// Return 0, -KOS_EINVAL for an unlisted register or disallowed value bits,
// or -KOS_ENOSYS without a backend. Reject disallowed bits; do not mask them.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// Set a pin's raw chip function code during initialization. Return 0,
// -KOS_EINVAL for invalid arguments, -KOS_EBUSY for a kernel-owned pin,
// or -KOS_ENOSYS without a backend.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func);

// Retune to target (a kos_pstate_t) and return the actual core frequency.
// A safe fallback also returns its nonzero frequency; callers must run the
// coherence hooks after any clock change. Zero means retuning is unsupported.
// The backend adjusts flash wait states and voltage, and reanchors
// arch_clock_now around the PLL/divider change. Call from privileged thread
// context with interrupts masked, never from an ISR.
uint64_t arch_cpu_clock_set(uint32_t target);

// Console clock hooks, with no-op fallbacks:
// flush_sync waits for transmission complete, including the shift register,
// under the caller's IrqLock before retuning or shutdown. Keep the wait bounded
// for panic use; drop remaining output if the UART is stuck. Synchronous
// host consoles need no flush.
// retune sets the baud from the current SystemCoreClock after it changes.
void arch_console_flush_sync(void);
void arch_console_retune(void);

// High-resolution telemetry counter in backend-defined units. The u32 value
// wraps; session anchors let the decoder reconstruct time. Targets without
// a source leave KICKOS_HAVE_TRACE_CLOCK undefined and reject telemetry builds.
uint32_t arch_trace_now(void);

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// Stamp the thread trace ID into its saved context once. Switch tracing must
// read IDs from the contexts being swapped, since an ISR may change scheduler
// state before the physical swap. Present only with telemetry.
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

// Query at grant admission, not commit: unsupported regions are dropped at
// commit. Describe the built enforcement mode and the arena's memory band.
// Recheck this answer if the linker moves the arena to a different cache type.
int arch_mpu_nocache_support(void);

struct arch_mpu_region
{
    uintptr_t base;
    size_t size;
    uint32_t attr; // OR of the ARCH_MPU_* bits
};

// Encode n regions and return a bitmask of accepted entries. Reject regions
// that fail arch_mpu_region_encodable; never round their bounds. Mark unused
// slots inactive, including all slots when n is zero. Called at mutation,
// not switch time. Defined only with KICKOS_HAVE_MPU.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out);

// Replace the active MPU set with nonoverlapping regions on switch-in.
// Attributes describe user access. image holds encoded descriptors; regions
// provides addresses for backends such as sim mprotect, which denies all
// unlisted arena memory. image is null only without KICKOS_HAVE_MPU; in that
// configuration MpuSet::apply makes no call here.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image);

// Program the set recorded by arch_mpu_apply after a deferred physical switch.
// No-op when apply programs hardware immediately or when no MPU exists; every
// backend must provide a definition. Called from a switch epilogue, and from
// arch_mpu_apply_now below, which brackets it; a grant that must be live before
// its syscall returns goes through THAT, never through apply then commit.
void kickos_arch_mpu_commit(void);

// Program this region set into the hardware NOW, for a grant (kos_mem_self_grant)
// that must be effective before the syscall returns. It must NOT become the image
// a pended switch commits: a deferred backend keeps ONE stash cell, and leaving
// this set in it would have the switch epilogue program the caller's descriptors
// onto the incoming thread. Same arguments as arch_mpu_apply. Every backend must
// provide a definition; where apply already programs the hardware this is apply.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image);

// Nontranslating protection regions; translating ports use arch_aspace_*.

// Smallest enforceable MPU region: zero for no MPU, otherwise a power of two.
// arch_ram_region_size uses this value for rounding; zero keeps byte granularity.
size_t arch_mpu_min_region(void);

// Region encoding, queried only when arch_mpu_min_region() is nonzero:
// 1 requires power-of-two size and natural alignment; 0 allows base/limit
// in multiples of the minimum region size. cmake/boot_arena.cmake parses this
// body: keep a plain return <integer>; and no closing brace in body comments.
int arch_mpu_region_pow2(void);

// Whether one MPU descriptor covers (base, size) exactly. Do not round MMIO
// grants into neighboring registers. Sim returns false: mprotect maps no MMIO.
bool arch_mpu_region_encodable(uintptr_t base, size_t size);

// Round want to the size covered by one descriptor. Allocation and descriptor
// creation must use this same size. The rounded size is backing storage,
// not additional usable capacity.
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

// Required block alignment; equal to region size in power-of-two mode.
// With KICKOS_TLS, every block uses a power-of-two stride so masking SP
// produces the correct thread pointer.
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

// User RAM for domain data and unprivileged stacks: an mmap arena on sim,
// a linker region on MCUs. Allocate with arch_ram_region_size/align so one
// MPU region covers the block. Return null for size zero or exhaustion.
uintptr_t arch_ram_base(void);
size_t arch_ram_size(void);
void* arch_ram_alloc(size_t size);

// Fill up to max shared application regions: code RX and static data RW/NX.
// Return their count, or zero if none are modeled. The kernel prepends these
// to each unprivileged thread's domain and stack regions.
size_t arch_domain_static_regions(struct arch_mpu_region* out, size_t max);

// Readable application memory outside the running thread's MPU regions.
// Called only after the region check fails. Enforcing backends return false.
// Nonenforcing backends admit mapped linker code/rodata and static RAM below
// the arena. Sim admits host-image ranges outside the arena. Arena access
// always requires the region check.
bool arch_user_text_readable(uintptr_t ptr, size_t len);

// Writable application static memory outside the running thread's MPU regions.
// Called after the region check fails. Enforcing backends return false.
// Nonenforcing backends admit static RAM below the arena; sim admits host-image
// ranges outside it. Arena access always requires the region check.
bool arch_user_data_writable(uintptr_t ptr, size_t len);

// An address that faults on unprivileged access (sim: a reserved arena page no domain owns).
// Used by the isolation self-test.
uintptr_t arch_mpu_probe_addr(void);

// Opaque translating address spaces, selected instead of MPU regions.
// Backends own translation tags and manage them in activate/destroy.
struct arch_aspace;

// The access a mapping grants the UNPRIVILEGED level. At least one bit is required; a guard page
// is an unmapped page.
enum
{
    ARCH_MAP_R = 1u << 0,
    ARCH_MAP_W = 1u << 1,
    ARCH_MAP_X = 1u << 2
};

// Physical address width is independent of virtual pointer width.
typedef uint64_t arch_phys_addr_t;

// A memory type, never the bits a backend encodes it to.
enum arch_map_memtype
{
    ARCH_MAP_NORMAL = 0,  // cacheable, write-back
    ARCH_MAP_NOCACHE = 1, // Normal, outer and inner non-cacheable
    ARCH_MAP_DEVICE = 2   // device / MMIO
};

// Query memory-type support at grant admission. Never silently downgrade a
// type: a DMA buffer must not become cacheable. The board may supply the answer.
bool arch_aspace_memtype_support(enum arch_map_memtype type);

// A failed map must leave no partially installed mapping.
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

// Create an empty user space with the fixed kernel range mapped, or return
// null on allocation failure. Later kernel-range edits must reach every space.
struct arch_aspace* arch_aspace_create(void);

// Free the root, child tables and owned mapped frames. Invalidate before
// reusing the root or tag. Null is a no-op. Activate another space first if
// this one is running. Unmap borrowed frames before destroy to avoid double-free.
void arch_aspace_destroy(struct arch_aspace* space);

// Map pages granules from va to pa, or unmap them. Changes, including maps
// into empty slots, must be visible locally on return. The active space may
// be edited. Wait for peers on multicore; remote unmap maintenance may be
// deferred until frame reuse, but revocation must complete immediately.
// Peer handlers must not take the kernel lock.
// Removing, replacing or rolling back executable leaves also requires peer
// instruction synchronization. Capture peers before editing. Backends whose
// ISA lacks that operation document the limitation (RV64IMAC lacks FENCE.I).
// Reject partially mapped ranges with ARCH_ASPACE_EINVAL before editing;
// wholly mapped ranges may be remapped. Rollback of a new map removes its
// installed leaves and must complete the same maintenance as unmap.
enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages,
                                        uint32_t rights, enum arch_map_memtype type);
enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va,
                                          size_t pages);

// Activate the incoming task's space. Infallible on every backend; the
// architecture defines ordering when both root and translation tag change.
void arch_aspace_activate(struct arch_aspace* space);

// Acquire a kernel pointer to the page containing va, preserving its byte
// offset. Return null for unmapped pages or addresses outside the user range.
// Callers split ranges at page boundaries; adjacent virtual pages need not
// be physically adjacent. Each core must support ARCH_ASPACE_ACQUIRE_MIN
// outstanding holds. Repeated acquisition of one page also counts as separate
// holds unless the backend reference-counts them.
// Release only successful acquisitions. Ignore releases outside the valid
// address range without counting a hold or a mispairing.
#define ARCH_ASPACE_ACQUIRE_MIN 6u
void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va);
void arch_aspace_release(struct arch_aspace* space, uintptr_t va);

// Return the granule-aligned physical frame containing va; discard its page
// offset. Return zero for unmapped or invalid addresses, so user mappings
// must not use physical frame zero. Large-leaf backends must include the
// granule offset within that leaf. This uses no acquire hold or frame data.
// Do not infer frame identity from acquire-pointer differences: windowed
// backends may map unrelated frames into adjacent slots.
arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va);

// Self-test translation model:
// bits 0..7: confirmed ARCH_ASPACE_MODEL_* flags
// bits 8..15: reported ASID width
// bits 16..23: reported physical address width
// bits 24..31: supported granules in architecture order, smallest first
// (ARM64: 4 KiB, 16 KiB, 64 KiB).
#define ARCH_ASPACE_MODEL_GRANULE 0x01u /* the granule this port programs is supported */
#define ARCH_ASPACE_MODEL_ASID    0x02u /* the identifier is as wide as the port's record */
#define ARCH_ASPACE_MODEL_PA      0x04u /* the physical range covers what the port programs */
/* The port assigns translation tags. Excluded from ALL because untagged
 * ports are valid.
 */
#define ARCH_ASPACE_MODEL_TAGGED  0x08u
#define ARCH_ASPACE_MODEL_ALL     0x07u
#define ARCH_ASPACE_MODEL_ASID_SHIFT 8u
#define ARCH_ASPACE_MODEL_PA_SHIFT   16u
#define ARCH_ASPACE_MODEL_GRAN_SHIFT 24u
#define ARCH_ASPACE_MODEL_FIELD_MASK 0xFFu
uint64_t arch_aspace_model(void);

// The boot address-space handle. Switch to it before destroying the running
// process space so the translation register cannot point at a freed root.
struct arch_aspace* arch_aspace_boot(void);

#if defined(KICKOS_ENABLE_SELFTEST)
// Map-maintenance counters since boot:
// bits 63..32: issued page invalidations
// bits 31..8: elided invalidations, saturating
// bits 7..0: releases with neither a hold nor a frame in the kernel window
// The low byte records defects and must remain zero.
uint64_t arch_aspace_tlbi_counts(void);

// Bit c is set when core c uses this root. Return zero for null. Backends
// without per-core roots use bit zero for the active space.
uint32_t arch_aspace_active_cores(struct arch_aspace* space);
#endif

// Cache maintenance for noncoherent observers over [addr, addr + bytes).
// Use a kernel pointer; callers split user ranges into pages. Backends round
// to cache-line boundaries. Invalidate also cleans to preserve neighboring
// bytes on partial lines. Flush leaves lines valid. No fallback is provided.
void arch_dcache_flush(void const* addr, size_t bytes);
void arch_dcache_invalidate(void* addr, size_t bytes);

// MMIO grants must exclude kernel-owned timers, interrupt and protection
// controllers, bus access gates, and clock/reset registers.
struct arch_reserved_block
{
    uintptr_t base;
    size_t size;
};

// Fill at most max reserved blocks and return the count. Zero is valid.
// Each chip with KICKOS_MEMORY_ENFORCED, including MMU chips, must define
// this function; there is no fallback.
#define KICKOS_RESERVED_NONE 0u
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max);

// Whether Cortex-M bit-band aliases exist. If set, device grants must reject
// both alias windows. The default returns zero; chips with aliases return one.
int arch_bitband_present(void);

// Syscall trap. Run syscall_dispatch in privileged thread context on the
// caller's continuation, with arch_in_isr() false. Blocking calls resume
// inline after a context switch. Split 64-bit arguments into uintptr_t
// halves as defined in sys/abi.h. Preserve the ABI's 64-bit return registers:
// arch_syscall reads the low half; arch_syscall64 reads both.
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);

// Kernel-text syscall traps. Split-image backends place these in the shared
// kernel mapping; unsplit images alias the user trap names. Kernel code must
// use these names so traps remain reachable under every address space.
#if KICKOS_HAVE_ASPACE
uintptr_t karch_syscall(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
uint64_t karch_syscall64(uintptr_t nr,
                         uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
#else
#define karch_syscall arch_syscall
#define karch_syscall64 arch_syscall64
#endif

// Register IPC trap, enabled by KICKOS_ARCH_HAS_IPC_FASTPATH.
// io has KOS_CALL_REG_WORDS + 3 words:
// input: io[0]=nr, io[1]=ep_cap, io[2]=packed lengths, io[3..]=request
// output: io[1..]=reply; the return value is the call result.
// Preserve the io pointer across the trap. KOS_CALL_REG_FALLBACK requires
// reissuing through KOS_SYS_CALL.
#if KICKOS_ARCH_HAS_IPC_FASTPATH
int32_t arch_syscall_reg(uint32_t* io);

// Set the result in a saved, nonlive context for a thread parked by the
// fastpath without a kernel continuation. Use the slot restored as the
// syscall return register.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result);
#endif

// Interrupt delivery control. All lines start masked. Raises while masked
// are latched and coalesced, then delivered through the ISR after unmasking.
// The generic ISR masks a device line until its driver acknowledges it.
// Mask, unmask, inject and clear_pending protect their own state with local
// interrupt masking; callers need no IrqLock.
// Portable code may unmask at most one line with pending redelivery per
// IrqLock region: backends with one shared doorbell identity would overwrite
// the first request. This applies even on backends with a pending bitmap.
void arch_irq_mask(int line);

// Unmask a line and deliver any latched raise through kickos_isr_irq,
// never by notifying the driver directly from unmask.
void arch_irq_unmask(int line);

// Discard a pending raise where hardware allows it; native PLIC lines may
// ignore this. Used at first registration and for level-triggered rearming.
void arch_irq_clear_pending(int line);

// Inject a device interrupt through the ISR path. Privileged test support;
// normal drivers register, wait and acknowledge.
void arch_irq_inject(int irq);

// Return the core assigned to line. Only that core may access the shared
// logical-line gating state.
#define KICKOS_IRQ_LINE_CORE_NONE (-1)
int arch_irq_line_core(int line);

// Whether line uses a private kernel vector instead of kickos_isr_irq.
// Such lines have no irq_table entry and must be refused by irq_claim and
// irq_attach. Multiple lines may be reserved; the fallback returns false.
bool arch_irq_line_kernel_owned(int line);

// Console output:
// write inserts one line with console_tx_insert_line and returns its result.
// Rejected lines must not bypass the ring: that would interleave device writes.
// write_sync is bounded, polled output for panic, faults and startup, safe
// without scheduling or IRQs. Each chip must define it; the fallback forwards
// to write and cannot provide those guarantees.
int arch_console_write(char const* buf, size_t n);
void arch_console_write_sync(char const* buf, size_t n);

// Restore the console UART for panic output after user access. Handover
// backends must rewrite the full register setup idempotently; default is a no-op.
void arch_console_reclaim(void);

// Return the register window written by arch_console_reclaim. The address
// comes from the backend, never userspace. A size of zero means no window.
void arch_console_reclaim_window(uintptr_t* base, size_t* size);

// Optional diagnostic LED, usable before drivers and during faults.
// Initialize once at boot; set the raw level with on != 0. Both default to no-op.
void arch_diag_led_init(void);
void arch_diag_led_set(int on);

// Optional chip-specific fault details after the CPU frame and status dump.
// Defaults to no-op.
void arch_fault_report_extra(void);

// Whether the fault occurred in unprivileged thread mode, using CPU privilege
// at fault time. Syscall dispatch is privileged. Defaults to false (panic).
// The frame is backend-specific. Before reading a frame on the thread stack,
// validate it with kickos_fault_frame_trusted; a wild SP may name arbitrary
// memory. Register-sourced privilege, cause and status remain valid.
bool arch_fault_is_user_thread(void* frame);

// After arch_fault_is_user_thread returns true, redirect exception return to
// kickos_thread_fault_exit in privileged thread mode at kickos_fault_stack_top.
// Record fault details with kickos_fault_record. Dying registers need not be
// preserved. The fallback does nothing.
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
// Dispatch syscalls. Sign-extend negative errno results; zero-extend other
// results. A 32-bit caller reads the low half.
uint64_t syscall_dispatch(uintptr_t nr,
                          uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3);
// A memory-protection violation was caught (sim: SIGSEGV over the arena).
void kickos_isr_fault(uintptr_t addr, int is_write);

#if KICKOS_KERNEL_CORES > 1
// Release the kernel lock inside the physical swap, after saving the outgoing
// context and moving to the incoming stack. This prevents peers from choosing
// a thread whose saved state is stale. Also raise arch_ipi_resched_self for
// pending work; a deferred ISR switch leaves this release to the swap.
void kickos_switch_unlock(void);

// Acquire whether this core's control block is published. A nonzero result
// makes all initialization writes visible.
int kickos_kernel_core_seated(void);

// Whether this core has a published thread. Query only after
// kickos_kernel_core_seated returns nonzero.
int kickos_kernel_core_ready(void);

// Publish this core's scheduler commitment once, immediately before start.
void kickos_kernel_core_arrive(void);

// Start this core's scheduler. Requires seated and ready; never returns.
void kickos_kernel_core_start(void) __attribute__((noreturn));

// Handle doorbell scheduling from interrupt dispatch only. Takes the kernel
// lock, so do not call from the doorbell service body.
void kickos_kernel_core_resched(void);

// Publish reschedule requests for cores before raising their doorbells.
void kickos_kernel_core_resched_owe(uint32_t cores);

// Whether this core has a pending reschedule, without consuming it.
int kickos_kernel_core_resched_owed(void);

// Consume this core's reschedule requests; return nonzero if any were pending.
int kickos_kernel_core_resched_take(void);
#endif

#if KICKOS_KERNEL_CORES > 1
// Service this core's IRQ routing requests from the doorbell service body,
// after snapshotting request sequences and before acknowledging them. This
// order prevents acknowledging requests that were not serviced. Takes no
// kernel lock; mask/unmask/clear_pending protect themselves. See irq_route.cc.
void kickos_irq_route_service(void);
#endif

#if KICKOS_AMP_NODE
// Drain the AMP node's inbox from its doorbell service body after sending
// acknowledgements, so payload processing cannot delay rendezvous. Uses only
// local interrupt masking; takes no kernel lock or kernel-built state.
void kickos_amp_node_service(void);

// Return the machine core assigned to node in the partition map.
uint32_t kickos_amp_node_core(uint32_t node);
#endif

// Allocate/free a physical frame for translation tables through the kernel
// allocator. Frames are granule-sized and aligned; contents are undefined.
// Zero means exhaustion (ARCH_ASPACE_ENOMEM). Backends must not keep separate
// frame pools. Available with KICKOS_HAVE_ASPACE.
arch_phys_addr_t kickos_frame_alloc(void);
void kickos_frame_free(arch_phys_addr_t frame);

// Try fault isolation before printing. True means the handler must return
// through the redirected frame to kickos_thread_fault_exit; false means panic.
bool kickos_fault_kill_thread(void* frame);

// Check that [frame, frame + bytes) lies in the running thread's stack.
// Return false for no current thread, idle, or no recorded stack. Always
// perform this check in addition to architecture-specific guards.
bool kickos_fault_frame_trusted(void const* frame, size_t bytes);

#if KICKOS_KERNEL_STACKS
// Check that the frame lies in the running thread's kernel stack block.
// Return false for no current thread, idle, or ctx.kernel_sp == 0. Available
// only on backends with kernel stack blocks.
bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes);
#endif

// Return the running thread's exclusive stack top, or zero for no thread,
// idle or no recorded stack. Validate the fault SP with
// kickos_fault_frame_trusted before using this for redirection.
uintptr_t kickos_fault_stack_top(void);

// Whether the fault address lies in KICKOS_FAULT_STACK_GUARD_BAND below the
// running stack. This detects overflow even if the exception restores SP
// (as on RXv3). Return true when no stack is recorded.
bool kickos_fault_below_stack(uintptr_t addr);

// Record fault details for printing later in thread context. Do not print
// in the handler: kpanic_enter permanently masks IRQs and reclaims the console.
// status_name labels the 64-bit status value. Read addr only when addr_valid.
void kickos_fault_record(char const* status_name, uint64_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid);

#if defined(KICKOS_ENABLE_SELFTEST)
// Report a damaged kickos_trapstack_witness from the panic path after
// kpanic_enter; stay silent if intact. The self-test checks that trap entry
// cannot write through a user SP into kernel memory.
void kickos_trapstack_witness_report(void);

// Record a nested trap's frame and interrupted thread stack bounds to check
// that it used a kernel stack. Pass lo == 0 with no current thread. Record
// counters only; KOS_SYS_NEST_WITNESS reads them for shutdown reporting.
void kickos_nestwitness_note(uintptr_t frame, uintptr_t lo, uintptr_t hi);
uint32_t kickos_nestwitness_count(int which);
#endif

// Where arch_fault_redirect_to_exit points the faulting thread. Runs privileged, in thread
// mode, on that thread's own stack.
void kickos_thread_fault_exit(void) __attribute__((noreturn));

// Contain the thread whose live SP trap entry rejected. offender must name
// the physically executing context, not scheduler current, which may already
// name an incoming thread before a deferred switch. Raise CANCEL_SLAY and
// return the context to resume, possibly the offender rebuilt for exit.
// On success, write the name to nonnull slain_name; leave it untouched on
// failure. Use that name on the caller's trap stack, not the bounded exit stack.
// Return null for no owner, idle, a privileged thread, failed slay or no
// runnable context. Takes the kernel lock; callers run with IRQs unmasked.
struct arch_context* kickos_thread_contain_wild_stack(struct arch_context* offender,
                                                      char const** slain_name);

// Exit stub for arch_ctx_redirect: privileged thread mode at the stack top.
// Prints nothing. arg is always null and matches the context entry signature.
void kickos_thread_slay_exit(void* arg) __attribute__((noreturn));

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// Emit SWITCH at the physical swap, using IDs from the two contexts, never
// shared scheduler state. from_tid is 0xFFFF on the first switch. RESCAN group.
void kickos_trace_switch_done(uint16_t from_tid, uint16_t to_tid);

// Emit the final SESSION record before arch_shutdown drains the ring.
// Include records_attempted and the second clock anchor for decoder resync.
void kickos_trace_final_session(void);

// Print attempted/dropped telemetry counts at shutdown for CI validation.
void kickos_trace_report_counters(void);
#endif

} // extern "C"

#endif
