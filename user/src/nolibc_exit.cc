// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// exit() for the posture that links NO C library. Its own TU: the archive pulls it only for an
// image that calls exit().

#include <kickos/sys.h>

#include <stdlib.h>

extern "C" void exit(int code) noexcept
{
    kos_exit(code);
    while (true)
    {
    }
}
