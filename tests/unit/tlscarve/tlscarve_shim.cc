// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/thread/tls.cc compiled under a forced TLS configuration. The sim build sets
// KICKOS_TLS=0 tree-wide and defines neither KICKOS_TLS_STRIDE nor KICKOS_ARCH_TLS_TCB, so
// without this the target would compile the do-nothing half of the file and gate nothing.
// The arch header is pulled in FIRST so an arch that does state KICKOS_ARCH_TLS_TCB is
// overridden here rather than colliding with the define below.

#include "tlscarve_config.h"

#undef KICKOS_TLS
#define KICKOS_TLS 1

#include <kickos/arch/arch.h>
#include <kickos/config/system.h>

#ifdef KICKOS_ARCH_TLS_TCB
#undef KICKOS_ARCH_TLS_TCB
#endif
#define KICKOS_ARCH_TLS_TCB TLSCARVE_TCB

#ifdef KICKOS_TLS_STRIDE
#undef KICKOS_TLS_STRIDE
#endif
#define KICKOS_TLS_STRIDE TLSCARVE_STRIDE

// The sim build resolves this 0, its arch seating no thread pointer at all, so without the
// override the shim would compile the seating half of tls_stack_admissible for every case
// and the stride arms would gate nothing.
#ifdef KICKOS_TLS_FROM_SP
#undef KICKOS_TLS_FROM_SP
#endif
#define KICKOS_TLS_FROM_SP TLSCARVE_FROM_SP

#include "../../../kernel/thread/tls.cc"
