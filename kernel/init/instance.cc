// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Storage for the kernel instances. constinit puts them in BSS with no boot-time
// constructor and no .init_array entry, so kernel() stays zero-cost and signal-safe; the
// compiler refuses the declaration the day a Kernel member loses its initialiser.

#include <kickos/instance.h>

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
        __thread unsigned g_instance_index __attribute__((tls_model("initial-exec"))) = 0;
#endif
    }
}
