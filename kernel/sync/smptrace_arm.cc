// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos_smp_trace_arm's own unit. The arm's other units compile clean with the knob off as
// well, so this one is what says the forced -DKICKOS_SMP_TRACE=1 reached them.

#if !defined(KICKOS_SMP_TRACE) || KICKOS_SMP_TRACE != 1
#error "kickos_smp_trace_arm is compiled with KICKOS_SMP_TRACE off: its forced -D did not land"
#endif
