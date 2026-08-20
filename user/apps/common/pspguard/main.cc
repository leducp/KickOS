// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// PSP-BOUNDS security gate (armv7m). Exception entry stacks the HARDWARE frame ABOVE the
// PSP with the pre-exception privilege, so a kernel-aimed PSP faults as MSTKERR before any
// handler runs. The {r4-r11, EXC_RETURN} block PendSV and the SVC trap push BELOW that frame
// is written in handler mode and is refused by nothing, so a PSP a few words above a stack's
// base clears the hardware check and still writes under the stack. Thread stacks come from a
// bump allocator with no padding between equal-size pow2 blocks, so the word under a stack
// base belongs to a NEIGHBOUR thread's own granted region, and the last word pushed is
// EXC_RETURN. A neighbour that rewrites that word to 0xFFFFFFF1 resumes the victim in
// HANDLER MODE, PRIVILEGED.
//
// One image per arm: a run observes one refusal, and each arm is reachable only through the
// guard leg it names.
//
// MODE 0: PSP 32 bytes above stack_lo. The 32-byte hardware frame fits, the 36-byte software
// block does not. Refusal: PendSV, "no room below", need=36.
//
// MODE 1: the same hole through the FP window. With FPCA live the hardware frame is 104
// bytes and the software block is 100, so a PSP 64 bytes above stack_lo satisfies a bound
// written for 36 alone and still escapes. Refusal: PendSV, "no room below", need=100. A
// guard that ignores EXC_RETURN bit 4 passes this arm outright.
//
// MODE 2: the SECOND site. SVC_Handler pushes the same block through the same PSP for the
// register-carrying IPC trap, and its slow path hands that PSP to svc_trampoline, which runs
// PRIVILEGED on it. Refusal: SVCall, "no room below". This PSP is short of room for even the
// push, so the arm establishes only that the site is guarded at all; mode 5 establishes the
// figure.
//
// MODE 5: the SVC slow path's KERNEL DESCENT. The PSP is placed so that the push FITS: the
// SVC's own hardware frame takes the top 32 bytes and leaves the handler a PSP 40 bytes above
// the base, one word clear of the 36-byte push. svc_trampoline then runs PRIVILEGED IN
// THREAD MODE on that PSP and syscall_dispatch descends hundreds of bytes below stack_lo,
// with the MPU not consulted. Refusal: SVCall, "no room below", need = the whole SVC extent.
//
// MODE 3: PSP ABOVE stack_hi, inside a granted domain region allocated after the stack. The
// room below such a PSP is enormous, so only the upper-bound leg can refuse it.
//
// MODE 4: PSP BELOW stack_lo, inside a granted domain region allocated before the stack. Only
// the lower-bound leg can refuse it. The region is GRANTED so that hardware stacking succeeds
// and the software push is what gets refused.
//
// Modes 0, 1, 3 and 4 provoke the switch with a HIGHER-priority ticker and not with a fault,
// because the store under test is the SWITCHER's; the ticker's counter advancing separates
// "PendSV took the wild PSP" from "the loop merely spun". Modes 2 and 5 trap synchronously.
//
// armv6m is deliberately out of scope: its residual is {r4-r11} with no EXC_RETURN, which the
// epilogue rebuilds from a literal, so nothing a thread can reach steers privilege.

#include <kickos/arch/armv7m_trap_stack.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

// Undefined would silently select mode 0 rather than fail.
#ifndef KICKOS_PSPGUARD_MODE
#error "KICKOS_PSPGUARD_MODE must be set by this app's CMakeLists"
#endif
#if !defined(__arm__) && !defined(__thumb__)
#error "pspguard moves SP with ARM asm; it must not be built for another ISA"
#endif
#if KICKOS_PSPGUARD_MODE == 1 && !defined(__ARM_FP)
#error "pspguard mode 1 needs an FP frame to place; it must not be built soft-float"
#endif

using kickos::emit;

namespace
{
    // Power of two and at least KICKOS_MIN_STACK_SIZE: under enforcement the child's stack is
    // committed as ONE MPU region, and PMSAv7 can only name a naturally aligned pow2.
    constexpr uint32_t STACK_BYTES = 2048;
    // The out-of-stack landing pads for modes 3 and 4. Also a pow2, for the same reason: they
    // are granted to the child as its domain region so hardware stacking into them succeeds.
    constexpr uint32_t PAD_BYTES = 512;

#if KICKOS_PSPGUARD_MODE == 0
    constexpr uint32_t SP_OFFSET = 32;
#elif KICKOS_PSPGUARD_MODE == 1
    // 104 (the extended hardware frame) + 64 (room a bound written for the non-FP push alone
    // accepts).
    constexpr uint32_t SP_OFFSET = 168;
#elif KICKOS_PSPGUARD_MODE == 2
    // 32 (the hardware frame the SVC stacks) + 32 (less than the 36 the push needs).
    constexpr uint32_t SP_OFFSET = 64;
#elif KICKOS_PSPGUARD_MODE == 5
    // The hardware frame the SVC stacks (32), plus ONE WORD MORE than the software push
    // needs, so the push leg accepts and only a bound that also charges the kernel descent
    // can refuse.
    constexpr uint32_t SP_OFFSET = 32 + KICKOS_ARMV7M_TRAP_FRAME + 4;
    // A guard that does not charge the descent lets it run: hundreds of privileged bytes
    // below stack_lo. Without a sink under the stack the arena it corrupts is the runner's
    // own, and the failure arrives as a fault elsewhere instead of as this arm's report.
    constexpr uint32_t SINK_BYTES = 2048;
#endif

    // Filled by main before the spawn: the child has no way to find its own bounds.
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

    volatile uint32_t g_ticks = 0;

    void ticker(void*)
    {
        while (true)
        {
            kos::sleep_ns(2u * 1000u * 1000u);
            g_ticks = g_ticks + 1;
        }
    }

    void wild(void* arg)
    {
        Arm const* const a = static_cast<Arm const*>(arg);
        char msg[160];

        // SP bits[1:0] are RAZ/WI on ARMv7-M, so there is no word-unaligned PSP for a guard
        // to refuse. The write and the read-back are one asm block, so nothing intervenes.
        uint32_t sp_low = 0;
        __asm volatile("mov  r1, sp     \n\t"
                       "orr  r2, r1, #3 \n\t"
                       "mov  sp, r2     \n\t"
                       "mov  %0, sp     \n\t"
                       "mov  sp, r1     \n\t"
                       : "=r"(sp_low)
                       :
                       : "r1", "r2");
        ksnprintf(msg, sizeof(msg), "[pspguard] ok - sp low bits read back as %u\n",
                  static_cast<unsigned>(sp_low & 3u));
        emit(msg);

        uint32_t const target = static_cast<uint32_t>(a->target);
        ksnprintf(msg, sizeof(msg), "[pspguard] arm: mode=%u stack=[0x%x,0x%x) sp=0x%x\n",
                  static_cast<unsigned>(KICKOS_PSPGUARD_MODE),
                  static_cast<unsigned>(a->stack_lo), static_cast<unsigned>(a->stack_hi),
                  static_cast<unsigned>(target));
        emit(msg);

#if KICKOS_PSPGUARD_MODE == 2 || KICKOS_PSPGUARD_MODE == 5
        // svc_trampoline exits to the STACKED LR, so the return address has to be arranged
        // here the way arch_syscall_reg does it. KOS_SYS_CLOCK_NOW is nullary and cannot fail,
        // so a dispatch that DOES run leaves nothing behind but the frame it wrote under the
        // stack. Nothing may touch memory between the SP move and its restore.
        uint32_t saved = 0;
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
        emit("[pspguard] ERROR: the syscall trap accepted a PSP it must refuse\n");
#else
        uint32_t const before = g_ticks;
        volatile uint32_t* const addr = &g_ticks;
        uint32_t saved = 0;
        uint32_t spins = 0;
        uint32_t observed = 0;
        // Nothing may touch memory between the SP move and its restore, so the whole window is
        // one asm block with every operand in a register.
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
    // `pad_lo` is below the stack and `pad_hi` above it. Modes 3 and 4 need exactly that.
#if KICKOS_PSPGUARD_MODE == 5
    void* const sink = kos_ram_alloc(SINK_BYTES);
    if (sink == nullptr)
    {
        emit("[pspguard] ERROR: arena ram_alloc refused the descent sink\n");
        return 1;
    }
#endif
    void* const pad_lo = kos_ram_alloc(PAD_BYTES);
    void* const st = kos_ram_alloc(STACK_BYTES);
    void* const pad_hi = kos_ram_alloc(PAD_BYTES);
    if (pad_lo == nullptr or st == nullptr or pad_hi == nullptr)
    {
        emit("[pspguard] ERROR: arena ram_alloc refused (root AUTH_MEMORY seat?)\n");
        return 1;
    }
    g_arm.stack_lo = reinterpret_cast<uintptr_t>(st);
    g_arm.stack_hi = g_arm.stack_lo + STACK_BYTES;

    // The child's domain region, granted so that hardware stacking into the pad SUCCEEDS and
    // the software push below it is what the guard refuses. Only modes 3 and 4 need one.
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
    kos::thread::Handle const tk = kos::thread::spawn(ticker, nullptr, "ticker", 20);
    if (not tk.valid())
    {
        emit("[pspguard] ERROR: ticker spawn refused\n");
        return 1;
    }
    kos::thread::Handle const w =
        kos::thread::spawn(wild, &g_arm, "wild", 10, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                           mem, mem_size, st, STACK_BYTES);
    if (not w.valid())
    {
        emit("[pspguard] ERROR: wild spawn refused\n");
        return 1;
    }
    w.join();
    emit("[pspguard] ERROR: the wild thread came back; its PSP was never refused\n");
    return 1;
}
