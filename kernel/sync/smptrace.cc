// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The rings themselves. In .bss and never initialised at run time: a cursor of zero is an
// empty ring, which is what a core that recorded nothing must read as.

#include <kickos/smptrace.h>

#if defined(KICKOS_SMP_TRACE) && KICKOS_SMP_TRACE

namespace kickos
{
    KosTraceRing g_kos_trace[KICKOS_KERNEL_CORES] = {};
}

#endif
