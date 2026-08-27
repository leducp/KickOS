// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PSP-BOUNDS security gate. Exception entry stacks the HARDWARE frame ABOVE the PSP with the
// pre-exception privilege, so a kernel-aimed PSP faults as MSTKERR before any handler runs. The
// {r4-r11, EXC_RETURN} block pushed BELOW that frame is written in handler mode and refused by
// nothing, so a PSP a few words above a stack's base clears the hardware check and still writes
// under the stack. Stacks come from a bump allocator with no padding between equal-size pow2
// blocks, so the word under a stack base is a NEIGHBOUR thread's granted region, and the last
// word pushed is EXC_RETURN: rewriting it to 0xFFFFFFF1 resumes the victim PRIVILEGED.
//
// One image per arm, each arm reachable only through the guard leg it names:
//   0  PSP 32 bytes above stack_lo: the 32-byte hardware frame fits, the 36-byte software block
//      does not. Refusal PendSV, need=36.
//   1  the same hole through the FP window: with FPCA live the frame is 104 and the block 100,
//      so 64 bytes above stack_lo satisfies a bound written for 36 alone. Refusal PendSV,
//      need=100, and a guard that ignores EXC_RETURN bit 4 passes it outright.
//   2  the SECOND site: SVC_Handler pushes the same block through the same PSP and its slow path
//      hands that PSP to svc_trampoline, which runs PRIVILEGED on it until it reaches the
//      caller's kernel block. Refusal SVCall, need = the whole SVC extent.
//   3  PSP ABOVE stack_hi, in a granted region allocated after the stack: only the upper-bound
//      leg can refuse it.
//   4  PSP BELOW stack_lo, in a granted region allocated before it: only the lower-bound leg
//      can refuse it. GRANTED, so hardware stacking succeeds and the software push is refused.
//   5  THE LOW EDGE, the one arm that expects the trap to be ACCEPTED: the PSP is placed with
//      exactly the room the SVC site asks for, which is legal because svc_trampoline moves SP
//      onto the caller's kernel block before calling anything. The claim is that the syscall
//      RETURNS and the poisoned band below the parked PSP is intact.
//
// Modes 0, 1, 3 and 4 provoke the switch with a HIGHER-priority ticker and not with a fault,
// the store under test being the SWITCHER's; the ticker's counter advancing separates "PendSV
// took the wild PSP" from "the loop merely spun". Modes 2 and 5 trap synchronously.
//
// ARMV6M CARRIES MODES 2 AND 5 AND NOTHING ELSE: its switch residual is {r4-r11} with no
// EXC_RETURN, rebuilt by the epilogue from a literal, so no PSP a thread writes steers an
// exception return, and mode 1 needs an FP frame Cortex-M0 cannot make.

#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
#include <kickos/arch/armv7m_trap_stack.h>
#else
#include <kickos/arch/armv6m_trap_stack.h>
#endif
/* KICKOS_KERNEL_STACKS: mode 5's whole premise is the transfer onto the kernel block, and
   the armv6m header has no reason to carry the posture (that arch has one entry design), so
   this app reads it itself. -Wundef makes the fallback load-bearing rather than tidy. */
#if defined(__has_include) && __has_include(<kickos/board_config.h>)
#include <kickos/board_config.h>
#endif
#ifndef KICKOS_KERNEL_STACKS
#define KICKOS_KERNEL_STACKS 0
#endif
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

// An undefined macro evaluates as 0 in the #if ladder below, i.e. as mode 0.
#ifndef KICKOS_PSPGUARD_MODE
#error "KICKOS_PSPGUARD_MODE must be set by this app's CMakeLists"
#endif
#if !defined(__arm__) && !defined(__thumb__)
#error "pspguard moves SP with ARM asm; it must not be built for another ISA"
#endif
#if KICKOS_PSPGUARD_MODE == 1 && !defined(__ARM_FP)
#error "pspguard mode 1 needs an FP frame to place; it must not be built soft-float"
#endif
#if KICKOS_PSPGUARD_MODE == 5 && !KICKOS_KERNEL_STACKS
#error "pspguard mode 5 witnesses the transfer onto the kernel block; without the blocks the same sp is refused"
#endif
#if (!defined(__ARM_ARCH) || (__ARM_ARCH < 7)) && KICKOS_PSPGUARD_MODE != 2 \
    && KICKOS_PSPGUARD_MODE != 5 && KICKOS_PSPGUARD_MODE != 6
#error "pspguard on armv6m has the synchronous SVC arms and the block arm (modes 2, 5 and 6)"
#endif
#if KICKOS_PSPGUARD_MODE == 6 && !KICKOS_KERNEL_STACKS
#error "pspguard mode 6 parks inside a kernel block; without the blocks there is none to park in"
#endif
#if KICKOS_PSPGUARD_MODE == 6 && KICKOS_HAVE_MPU
#error "pspguard mode 6 needs a board that carves blocks and does NOT enforce: with enforcement the hardware refuses the entry stacking first and the block leg is never reached"
#endif

using kickos::emit;

#if KICKOS_PSPGUARD_MODE == 6
// The RUNNING thread's kernel block top (arch/arch.h). Already exported for the fault path, so
// this arm needs no test-only kernel surface; reachable from a thread only because the board
// this arm runs on does not enforce, which is the same fact the arm exists to exploit.
extern "C" uintptr_t kickos_fault_stack_top(void);
#endif

namespace
{
    // Power of two and at least KICKOS_MIN_STACK_SIZE: under enforcement the child's stack is
    // committed as ONE MPU region, and PMSAv7 can only name a naturally aligned pow2.
    constexpr uint32_t STACK_BYTES = 2048;
    // Landing pads for modes 3 and 4, granted as the child's domain region, so pow2 too.
    constexpr uint32_t PAD_BYTES = 512;

#if KICKOS_PSPGUARD_MODE == 0
    constexpr uint32_t SP_OFFSET = 32;
#elif KICKOS_PSPGUARD_MODE == 1
    // 104 (the extended hardware frame) + 64, which a bound written for the 36-byte push takes.
    constexpr uint32_t SP_OFFSET = 168;
    // WHERE A DEFERRED LAZY FP SAVE WOULD LAND. Exception entry from an FPCA-live thread
    // allocates the extended frame below the parked sp and only RESERVES {s0-s15,FPSCR} inside
    // it: FPCCR.LSPACT is set, FPCAR names the reservation, and the write waits for the next FP
    // instruction ANY context executes. Poisoning that reservation and reading it back after an
    // INNOCENT thread has executed FP is what separates "the offender's pending save was
    // discarded" from "it fired later, through a pointer the offender chose".
    constexpr uint32_t HW_FRAME_FP = 104;  // 8 integer words, {s0-s15}, FPSCR, the pad
    // s0 sits above the integer words. Verified against the live FPCAR under gdb.
    constexpr uint32_t FP_SAVE_OFF = SP_OFFSET - HW_FRAME_FP + 32u;
    constexpr uint32_t FP_SAVE_LEN = 17u * 4u; // {s0-s15} + FPSCR
    constexpr uint32_t FP_POISON = 0x0FF5A1EDu;
    static_assert(FP_SAVE_OFF + FP_SAVE_LEN <= SP_OFFSET,
                  "the reservation must sit under the parked sp, or the loop below writes it");
#elif KICKOS_PSPGUARD_MODE == 6
    // THE BLOCK LEG'S ROOM BOUND, and the one arm whose target is not an offset from stack_lo:
    // it parks the PSP inside the thread's OWN KERNEL BLOCK, four bytes above the block's
    // lowest word. The block leg admits a PSP by range, and without a room bound the switcher
    // then writes the callee block through that pointer, past the canary and into the
    // neighbouring slot's privileged continuation. Computed at run time in wild(), the block
    // top being the running thread's and known only to the kernel.
    constexpr uint32_t SP_OFFSET = 0;
    // WHERE TO PARK, and the hardware decides the floor. Exception entry stacks its frame
    // BELOW the parked PSP with no guard ahead of it, so a park too near the base makes the
    // HARDWARE clobber the canary and the arm measures entry instead of the leg. Park high
    // enough that even an FP-live entry's 104-byte frame clears the block's lowest word, and
    // still deep enough that the PSP the guard then sees is past the leg's room bound.
    // The plain 32-byte frame, this arm touching no FP anywhere. THREE constraints pin the
    // park, and the window between them is narrow enough that each is stated rather than
    // trusted.
    constexpr uint32_t HW_FRAME = 32;
    constexpr uint32_t BLOCK_PARK_UP = 64;
    static_assert(BLOCK_PARK_UP - HW_FRAME > 4,
                  "exception entry would stack through the block's canary before any guard "
                  "runs, and this arm would witness the hardware and not the block leg");
    // The bound the leg enforces, from the same macros switch.S derives BLOCK_PSP_MAX_DROP
    // from, so the two cannot drift apart silently.
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
    constexpr uint32_t PG_NEST_SVCK = KICKOS_ARMV7M_TRAP_NEST_SVCK;
    constexpr uint32_t PG_PENDSV_PUSH = KICKOS_ARMV7M_TRAP_FRAME;
#else
    constexpr uint32_t PG_NEST_SVCK = KICKOS_ARMV6M_TRAP_NEST_SVCK;
    constexpr uint32_t PG_PENDSV_PUSH = KICKOS_ARMV6M_TRAP_FRAME;
#endif
    static_assert(KICKOS_KERNEL_STACK_SIZE - (BLOCK_PARK_UP - HW_FRAME)
                      > KICKOS_KERNEL_STACK_SIZE - PG_NEST_SVCK - 4,
                  "the guarded PSP must sit deeper than the block leg's room bound, or the "
                  "leg accepts it and this arm witnesses nothing");
    // AND THE DAMAGE MUST REACH THE CANARY. A park that leaves the callee block room above the
    // lowest word still reads INTACT when the leg wrongly accepts, which makes the readback
    // below a clause that cannot fail. Measured against the switcher's own push figure.
    static_assert(BLOCK_PARK_UP - HW_FRAME < PG_PENDSV_PUSH + 4,
                  "a wrongly-accepted PSP would save ABOVE the canary, so the readback would "
                  "pass whatever the leg did");
#elif KICKOS_PSPGUARD_MODE == 2
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
    // 32 (the hardware frame the SVC stacks) + 32 (less than the 36 the push needs).
    constexpr uint32_t SP_OFFSET = 64;
#else
    // The same placement against the 32-byte v6-M block: 32 (the hardware frame) + 24, written
    // 8-aligned because ARMv6-M exception entry clears bit 2 of SP before stacking
    // (CCR.STKALIGN is Read-As-One) and would round an odd multiple of 4 down.
    constexpr uint32_t SP_OFFSET = 32 + KICKOS_ARMV6M_TRAP_FRAME - 8;
#endif
#elif KICKOS_PSPGUARD_MODE == 5
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
    constexpr uint32_t PG_SVC_NEED = KICKOS_ARMV7M_TRAP_NEED_SVC;
#else
    constexpr uint32_t PG_SVC_NEED = KICKOS_ARMV6M_TRAP_NEED_SVC;
#endif
    // The hardware frame (32) plus exactly what the site asks for below it, rounded UP to 8 and
    // given one spare word pair: entry clears bit 2 of SP before stacking on both arches
    // (CCR.STKALIGN Read-As-One), so a 4-mod-8 sp loses a word to that adjustment.
    constexpr uint32_t SP_OFFSET = ((32u + PG_SVC_NEED + 8u + 7u) / 8u) * 8u;
    static_assert(SP_OFFSET - 32u >= PG_SVC_NEED,
                  "the parked sp must leave the SVC site exactly the room it asks for, or "
                  "this arm measures a refusal instead of the acceptance it exists to pin");
    // From stack_lo up to the guarded PSP, which is what a kernel still dispatching on the
    // parked PSP writes FIRST: syscall_dispatch's frame starts there and descends.
    constexpr uint32_t BAND_BYTES = SP_OFFSET - 32u;
    constexpr uint32_t BAND_POISON = 0x5AFEBA5Eu;
    static_assert(BAND_BYTES >= 32u,
                  "a band this narrow could be stepped over by the first frame of a dispatch "
                  "that still ran on the parked PSP");
#endif

    struct Arm
    {
        uintptr_t stack_lo;
        uintptr_t stack_hi;
        uintptr_t target;
#if KICKOS_PSPGUARD_MODE == 6
        // Read by the wild thread before it parks and by root after the refusal, so the verdict
        // needs no kernel constant: the claim is that the word did not MOVE.
        uintptr_t canary_at;
        uint32_t canary_was;
#endif
    };
#if KICKOS_PSPGUARD_MODE == 6
    Arm g_arm = {0, 0, 0, 0, 0};
#else
    Arm g_arm = {0, 0, 0};
#endif

    // Big enough for a 2 ms tick to land, bounded so a switcher that never refuses reports its
    // own failure instead of hanging the runner out to the ctest timeout.
    constexpr uint32_t SPIN_LIMIT = 20000000u;

#if KICKOS_PSPGUARD_MODE != 2 && KICKOS_PSPGUARD_MODE != 5
    volatile uint32_t g_ticks = 0;

    void ticker(void*)
    {
        while (true)
        {
            kos::sleep_ns(2u * 1000u * 1000u);
            g_ticks = g_ticks + 1;
        }
    }
#endif

#if KICKOS_PSPGUARD_MODE == 1
    // Runs AFTER containment, on a thread that had nothing to do with the refusal. The FP
    // instruction is the trigger: a lazy save the refused entry left armed fires on the first
    // FP instruction ANY context executes, into the address that entry reserved.
    void fpcheck(void* arg)
    {
        Arm const* const a = static_cast<Arm const*>(arg);
        __asm volatile("vmov s0, %0 \n\t"
                       "vadd.f32 s0, s0, s0 \n\t"
                       :
                       : "r"(FP_POISON)
                       : "s0");
        volatile uint32_t const* const fpres =
            reinterpret_cast<volatile uint32_t*>(a->stack_lo + FP_SAVE_OFF);
        bool blown = false;
        for (uint32_t i = 0; i < FP_SAVE_LEN / 4u; i++)
        {
            if (fpres[i] != FP_POISON)
            {
                blown = true;
            }
        }
        if (blown)
        {
            emit("[pspguard] [lazyfp] CORRUPTED: a lazy FP save fired through the refused frame\n");
            return;
        }
        emit("[pspguard] [lazyfp] INTACT: the pending lazy FP save was discarded with the thread\n");
    }
#endif

    void wild(void* arg)
    {
        Arm const* const a = static_cast<Arm const*>(arg);
        char msg[160];

        // SP bits[1:0] are RAZ/WI on both arches (ARM ARM B1.4.1), so there is no
        // word-unaligned PSP for a guard to refuse.
        uint32_t sp_low = 0;
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
        __asm volatile("mov  r1, sp     \n\t"
                       "orr  r2, r1, #3 \n\t"
                       "mov  sp, r2     \n\t"
                       "mov  %0, sp     \n\t"
                       "mov  sp, r1     \n\t"
                       : "=r"(sp_low)
                       :
                       : "r1", "r2");
#else
        // Thumb-1 has no orr immediate, so the mask comes from a movs and the orrs writes the
        // flags. gcc opens every inline block .syntax divided, where those do not assemble.
        __asm volatile(".syntax unified \n\t"
                       "mov  r1, sp     \n\t"
                       "movs r2, #3     \n\t"
                       "orrs r2, r1     \n\t"
                       "mov  sp, r2     \n\t"
                       "mov  %0, sp     \n\t"
                       "mov  sp, r1     \n\t"
                       : "=r"(sp_low)
                       :
                       : "r1", "r2", "cc");
#endif
        ksnprintf(msg, sizeof(msg), "[pspguard] ok - sp low bits read back as %u\n",
                  static_cast<unsigned>(sp_low & 3u));
        emit(msg);

#if KICKOS_PSPGUARD_MODE == 6
        // The block top is this thread's own, so it is read HERE and not by root.
        Arm* const aw = static_cast<Arm*>(arg);
        uintptr_t const top = kickos_fault_stack_top();
        if (top == 0 or top <= KICKOS_KERNEL_STACK_SIZE)
        {
            emit("[pspguard] ERROR: no kernel block seated, so there is none to park in\n");
            return;
        }
        uintptr_t const base = top - KICKOS_KERNEL_STACK_SIZE;
        // The refusal is CLASSIFIED against the thread's stack bounds, so the arm states the
        // layout it expects instead of letting a moved block report as another leg.
        if (base >= a->stack_lo)
        {
            emit("[pspguard] ERROR: the kernel block is not below stack_lo\n");
            return;
        }
        aw->canary_at = base;
        aw->canary_was = *reinterpret_cast<volatile uint32_t*>(base);
        aw->target = base + BLOCK_PARK_UP;
#endif
        uint32_t const target = static_cast<uint32_t>(a->target);
        ksnprintf(msg, sizeof(msg), "[pspguard] arm: mode=%u stack=[0x%x,0x%x) sp=0x%x\n",
                  static_cast<unsigned>(KICKOS_PSPGUARD_MODE),
                  static_cast<unsigned>(a->stack_lo), static_cast<unsigned>(a->stack_hi),
                  static_cast<unsigned>(target));
        emit(msg);

#if KICKOS_PSPGUARD_MODE == 1
        // Laid at the NORMAL sp, far above it, so nothing this thread does afterwards writes it.
        volatile uint32_t* const fpres =
            reinterpret_cast<volatile uint32_t*>(a->stack_lo + FP_SAVE_OFF);
        for (uint32_t i = 0; i < FP_SAVE_LEN / 4u; i++)
        {
            fpres[i] = FP_POISON;
        }
#endif
#if KICKOS_PSPGUARD_MODE == 5
        // Laid at the NORMAL sp, so the loop writes nothing into the band it is laying.
        volatile uint32_t* const band = reinterpret_cast<volatile uint32_t*>(a->stack_lo);
        for (uint32_t i = 0; i < BAND_BYTES / 4u; i++)
        {
            band[i] = BAND_POISON;
        }
#endif
#if KICKOS_PSPGUARD_MODE == 2 || KICKOS_PSPGUARD_MODE == 5
        // svc_trampoline exits to the STACKED LR, so the return address is arranged here the way
        // arch_syscall_reg does. Nothing may touch memory between the SP move and its restore.
        uint32_t saved = 0;
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
        __asm volatile("mov    %[sv], sp        \n\t"
                       "mov    sp, %[low]       \n\t"
                       "movs   r0, #12          \n\t" // KOS_SYS_CLOCK_NOW
                       "adr    r1, 3f           \n\t"
                       "orr    r1, r1, #1       \n\t" // svc_trampoline exits with bx
                       "mov    lr, r1           \n\t"
                       "svc    #0               \n\t"
                       "3:                      \n\t"
                       "mov    sp, %[sv]        \n\t"
                       : [sv] "=&r"(saved)
                       : [low] "r"(target)
                       : "r0", "r1", "r2", "r3", "r12", "lr", "cc", "memory");
#else
        // Thumb-1: adds, orr carrying no immediate, and the .balign is what makes the adr
        // encodable at all, its target being Align(PC,4) + imm8*4. Its pad sits after the svc,
        // so a return that does arrive falls through it to the SP restore.
        __asm volatile(".syntax unified          \n\t"
                       "mov    %[sv], sp        \n\t"
                       "mov    sp, %[low]       \n\t"
                       "movs   r0, #12          \n\t" // KOS_SYS_CLOCK_NOW
                       "adr    r1, 3f           \n\t"
                       "adds   r1, #1           \n\t" // svc_trampoline exits with bx
                       "mov    lr, r1           \n\t"
                       "svc    #0               \n\t"
                       ".balign 4               \n\t"
                       "3:                      \n\t"
                       "mov    sp, %[sv]        \n\t"
                       : [sv] "=&r"(saved)
                       : [low] "r"(target)
                       : "r0", "r1", "r2", "r3", "r12", "lr", "cc", "memory");
#endif
#if KICKOS_PSPGUARD_MODE == 5
        // The gate matches this line and both verdicts below, so it reads an answer rather than
        // inferring one from a missing line. The acceptance must stay AHEAD of them.
        emit("[pspguard] accepted: the syscall trap ran on the low-edge sp\n");
        bool corrupt = false;
        for (uint32_t i = 0; i < BAND_BYTES / 4u; i++)
        {
            if (band[i] != BAND_POISON)
            {
                corrupt = true;
            }
        }
        if (corrupt)
        {
            emit("[pspguard] [lowband] CORRUPTED: the dispatch ran below the parked sp\n");
            return;
        }
        emit("[pspguard] [lowband] INTACT: the kernel wrote nothing below the parked sp\n");
        return;
#else
        emit("[pspguard] ERROR: the syscall trap accepted a PSP it must refuse\n");
#endif
#else
        uint32_t const before = g_ticks;
        volatile uint32_t* const addr = &g_ticks;
        uint32_t saved = 0;
        uint32_t spins = 0;
        uint32_t observed = 0;
        // Nothing may touch memory between the SP move and its restore.
        __asm volatile(
#if !defined(__ARM_ARCH) || (__ARM_ARCH < 7)
            // gcc opens every inline block .syntax divided, where Thumb-1's three-operand
            // ADDS does not assemble. The same line the two v6-M blocks below carry.
            ".syntax unified         \n\t"
#endif
#if KICKOS_PSPGUARD_MODE == 1
            "vmov   s16, %[bf]       \n\t"
#endif
            "mov    %[sv], sp        \n\t"
            "mov    sp, %[low]       \n\t"
            "movs   %[sn], #0        \n\t"
            "1:                      \n\t"
#if KICKOS_PSPGUARD_MODE == 1
            "vmov   s16, %[sn]       \n\t" // FPCA must still be set when the tick lands
#endif
            "ldr    %[ob], [%[ad]]   \n\t"
            "cmp    %[ob], %[bf]     \n\t"
            "bne    2f               \n\t"
            "adds   %[sn], %[sn], #1 \n\t"
            "cmp    %[sn], %[lim]    \n\t"
            "bne    1b               \n\t"
            "2:                      \n\t"
            "mov    sp, %[sv]        \n\t"
            // "l" on Thumb-1: its cmp/adds reach the low registers only, and the generic
            // "r" lets gcc hand this block an r8-r12 it cannot encode.
#if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
            : [sv] "=&r"(saved), [sn] "=&r"(spins), [ob] "=&r"(observed)
            : [low] "r"(target), [ad] "r"(addr), [bf] "r"(before), [lim] "r"(SPIN_LIMIT)
#else
            : [sv] "=&l"(saved), [sn] "=&l"(spins), [ob] "=&l"(observed)
            : [low] "l"(target), [ad] "l"(addr), [bf] "l"(before), [lim] "l"(SPIN_LIMIT)
#endif
            : "cc", "memory"
#if KICKOS_PSPGUARD_MODE == 1
            , "s16"
#endif
        );

        if (observed == before)
        {
            emit("[pspguard] ERROR: no switch landed while the PSP was out of bounds\n");
            return;
        }
        emit("[pspguard] ERROR: the switcher saved through a PSP it must refuse\n");
#endif
    }
}

int main(int, char**)
{
    // Allocation order IS the geometry: the bump allocator hands out increasing addresses, so
    // `pad_lo` lands below the child stack and `pad_hi` above it.
#if KICKOS_PSPGUARD_MODE == 3 || KICKOS_PSPGUARD_MODE == 4
    void* const pad_lo = kos_ram_alloc(PAD_BYTES);
#endif
    void* const st = kos_ram_alloc(STACK_BYTES);
#if KICKOS_PSPGUARD_MODE == 3 || KICKOS_PSPGUARD_MODE == 4
    void* const pad_hi = kos_ram_alloc(PAD_BYTES);
    if (pad_lo == nullptr or pad_hi == nullptr)
    {
        emit("[pspguard] ERROR: arena ram_alloc refused a landing pad\n");
        return 1;
    }
#endif
    if (st == nullptr)
    {
        emit("[pspguard] ERROR: arena ram_alloc refused (root AUTH_MEMORY seat?)\n");
        return 1;
    }
    g_arm.stack_lo = reinterpret_cast<uintptr_t>(st);
    g_arm.stack_hi = g_arm.stack_lo + STACK_BYTES;

    // Granted, so hardware stacking into the pad SUCCEEDS and the push below it is refused.
    void* mem = nullptr;
    uint32_t mem_size = 0;
#if KICKOS_PSPGUARD_MODE == 3
    mem = pad_hi;
    mem_size = PAD_BYTES;
    g_arm.target = reinterpret_cast<uintptr_t>(pad_hi) + PAD_BYTES;
#elif KICKOS_PSPGUARD_MODE == 4
    mem = pad_lo;
    mem_size = PAD_BYTES;
    g_arm.target = reinterpret_cast<uintptr_t>(pad_lo) + PAD_BYTES;
#else
    g_arm.target = g_arm.stack_lo + SP_OFFSET;
#endif

    if (g_arm.target == 0)
    {
        emit("[pspguard] ERROR: no arm target computed\n");
        return 1;
    }
#if KICKOS_PSPGUARD_MODE != 2 && KICKOS_PSPGUARD_MODE != 5
    kos::thread::Handle const tk = kos::thread::create(ticker, nullptr, "ticker", 20);
    if (not tk.valid())
    {
        emit("[pspguard] ERROR: ticker spawn refused\n");
        return 1;
    }
#endif
    kos::thread::Handle const w =
        kos::thread::create(wild, &g_arm, "wild", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                            mem, mem_size, st, STACK_BYTES);
    if (not w.valid())
    {
        emit("[pspguard] ERROR: wild spawn refused\n");
        return 1;
    }
    w.join();
#if KICKOS_PSPGUARD_MODE == 5
    // Reached only because the trap was ACCEPTED: a refusal that is not contained ends the
    // system, and a contained one slays the wild thread rather than returning it here.
    return 0;
#else
#if KICKOS_PSPGUARD_MODE == 1
    // THE INNOCENT FP THREAD, spawned only once the join above says containment is done, so
    // the ordering is root's and not a race. It is granted the dead thread's stack as its
    // DOMAIN region because that is the one way to read the reservation: root has no grant on
    // it and faults (MMFSR DACCVIOL), the wild thread that owned it is gone, and the
    // reservation must live where the refused frame put it.
    kos::thread::Handle const fc =
        kos::thread::create(fpcheck, &g_arm, "fpcheck", 10, KOS_POLICY_FIFO, 0,
                            /*privileged=*/false, st, STACK_BYTES);
    if (not fc.valid())
    {
        emit("[pspguard] ERROR: fpcheck spawn refused\n");
        return 1;
    }
    fc.join();
#endif
#if KICKOS_PSPGUARD_MODE == 6
    // THE CANARY, and BOTH directions are clauses. A guard that admitted the in-block PSP wrote
    // the callee block through it, so the block's lowest word moved; a run that never reached
    // the park left canary_at at zero and has witnessed nothing.
    if (g_arm.canary_at == 0)
    {
        emit("[pspguard] ERROR: the wild thread never parked inside its block\n");
        return 1;
    }
    uint32_t const now = *reinterpret_cast<volatile uint32_t*>(g_arm.canary_at);
    if (now != g_arm.canary_was)
    {
        emit("[pspguard] [kcanary] CORRUPTED: the switcher saved through an in-block PSP\n");
        return 1;
    }
    emit("[pspguard] [kcanary] INTACT: the block leg refused the PSP before the save\n");
#endif
    // ROOT OUTLIVING THE REFUSAL IS THE CLAIM, and the join is what carries it: the wild
    // thread reached its death point instead of the whole system reaching one. That the
    // thread died SLAIN and not by returning normally is the wild arm's own verdicts, none of
    // which reached the wire. A backend that does not contain never gets here at all.
    emit("[pspguard] contained: the wild thread was slain and root outlived it\n");
    return 0;
#endif
}
