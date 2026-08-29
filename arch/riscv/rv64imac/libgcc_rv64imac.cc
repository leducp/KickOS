// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two compiler-runtime helpers kernel text calls on this arch, in the kernel's own half.
// cmake/kernel_runtime_rv64imac.syms rewrites the kernel archives' references to these names,
// so a rename here has to happen there too.
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
