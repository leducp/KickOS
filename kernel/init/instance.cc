// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Storage for the singleton kernel instance. constinit puts it in BSS with no boot-time
// constructor and no .init_array entry, so kernel() stays zero-cost and signal-safe; the
// compiler refuses the declaration the day a Kernel member loses its initialiser.

#include <kickos/instance.h>

namespace kickos
{
    namespace detail
    {
        constinit Kernel g_instance;
    }
}
