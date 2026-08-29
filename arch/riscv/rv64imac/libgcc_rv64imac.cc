// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two compiler-runtime helpers KERNEL text calls on this arch, in the kernel's own half.
//
// WHY THE MULTILIB'S COPIES DO NOT SERVE. A static link gives a global symbol one address, and
// where the image is split (docs/design-m6-mmu.md R2.2) libgcc is linked into the APP's half:
// a kernel call of it is an auipc the two halves are deliberately out of range of, so the link
// refuses it.
//
// UNDER PRIVATE NAMES, WHICH IS NOT COSMETIC. Defining __clzdi2 here would hand the whole link
// this definition, and an APP-side libgcc member that needs it then makes the call the halves
// cannot carry: soft-float's __adddf3 and __floatunsidf both call __clzdi2, so the first app to
// print a double fails to link. cmake/kernel_runtime_rv64imac.syms rewrites the kernel
// archives' references to the names below, exactly as the fleet-wide map does for memcpy.
//
// rv64imac names no bit-manipulation extension, so the compiler lowers __builtin_clzll and
// __builtin_ctzll to these calls (kernel/sched/policy_fifo_rr.cc, kernel/mem/frame.cc).
//
// Undefined at zero, as libgcc's own are.

#include <kickos/board_config.h>

#if KICKOS_HAVE_ASPACE

#include <stdint.h>

extern "C"
{

int kickos_rv64_clzdi2(uint64_t v)
{
    int n = 0;
    for (int shift = 32; shift != 0; shift >>= 1)
    {
        if ((v >> (64 - shift)) == 0)
        {
            n += shift;
            v <<= shift;
        }
    }
    return n;
}

int kickos_rv64_ctzdi2(uint64_t v)
{
    int n = 0;
    for (int shift = 32; shift != 0; shift >>= 1)
    {
        uint64_t const mask = (static_cast<uint64_t>(1) << shift) - 1u;
        if ((v & mask) == 0)
        {
            n += shift;
            v >>= shift;
        }
    }
    return n;
}

}

#endif
