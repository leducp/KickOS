// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Storage for the singleton kernel instance. All-constant init keeps it in BSS:
// no dynamic-init guard, so kernel() stays zero-cost and signal-safe.

#include <kickos/instance.h>

namespace kickos
{
    namespace detail
    {
        Kernel g_instance;
    }
}
