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
    && KICKOS_PSPGUARD_MODE != 5
#error "pspguard on armv6m has the two synchronous SVC arms only (modes 2 and 5)"
#endif

using kickos::emit;

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
    };
    Arm g_arm = {0, 0, 0};

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

        uint32_t const target = static_cast<uint32_t>(a->target);
        ksnprintf(msg, sizeof(msg), "[pspguard] arm: mode=%u stack=[0x%x,0x%x) sp=0x%x\n",
                  static_cast<unsigned>(KICKOS_PSPGUARD_MODE),
                  static_cast<unsigned>(a->stack_lo), static_cast<unsigned>(a->stack_hi),
                  static_cast<unsigned>(target));
        emit(msg);

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
            : [sv] "=&r"(saved), [sn] "=&r"(spins), [ob] "=&r"(observed)
            : [low] "r"(target), [ad] "r"(addr), [bf] "r"(before), [lim] "r"(SPIN_LIMIT)
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
    kos::thread::Handle const tk = kos::thread::spawn(ticker, nullptr, "ticker", 20);
    if (not tk.valid())
    {
        emit("[pspguard] ERROR: ticker spawn refused\n");
        return 1;
    }
#endif
    kos::thread::Handle const w =
        kos::thread::spawn(wild, &g_arm, "wild", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                           mem, mem_size, st, STACK_BYTES);
    if (not w.valid())
    {
        emit("[pspguard] ERROR: wild spawn refused\n");
        return 1;
    }
    w.join();
#if KICKOS_PSPGUARD_MODE == 5
    // Reached only because the trap was ACCEPTED: a refusal panics and root never runs again.
    return 0;
#else
    emit("[pspguard] ERROR: the wild thread came back; its PSP was never refused\n");
    return 1;
#endif
}
