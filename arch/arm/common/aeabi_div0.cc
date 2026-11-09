// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The run-time ABI's divide-by-zero hooks, which libgcc's division helpers call on a zero divisor:
// every 32-bit division on v6-M, every 64-bit one on every Arm core. libgcc's own are weak and
// return a value. An OBJECT linked into the kernel and every image ahead of libgcc, never an
// archive member: libgcc extracts its own before a later archive is searched again.

#include "aeabi_div0.h"

// Linked into every image: KOS_IMAGE_BOTH_OBJECT of <kickos/sys/image.h>, outside this
// library's include path.
__asm__(".set __kickos_image_both_object, 1");

#define KICKOS_AEABI_STR_(x) #x
#define KICKOS_AEABI_STR(x) KICKOS_AEABI_STR_(x)

extern "C" __attribute__((naked)) void __aeabi_idiv0(void)
{
#if defined(__ARM_FEATURE_IDIV)
    // Under CCR.DIV_0_TRP this faults as the compiler's own divides do, UFSR.DIVBYZERO. The UDF
    // ends the call where the trap is off.
    __asm volatile("movs r1, #0\n\t"
                   "udiv r0, r0, r1\n\t"
                   "udf #" KICKOS_AEABI_STR(KICKOS_ARM_UDF_DIV0));
#else
    __asm volatile("udf #" KICKOS_AEABI_STR(KICKOS_ARM_UDF_DIV0));
#endif
}

extern "C" void __aeabi_ldiv0(void) __attribute__((alias("__aeabi_idiv0")));
