// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// The fault dead-end for a chip whose arch_shutdown reaches a debug or emulator channel that
// turns the status into a process exit (its chip file's exits_on_fault). The LED fallback beside
// this file would spin until the harness times out instead.
//
// Lone TU (arch/CMakeLists.txt states the rule): exactly one symbol, and the opt-in drops
// kfault_terminate_default.cc from this archive rather than leaving both members in it.

#include <kickos/arch/arch.h>

#include "fatal_status.ld.h"

extern "C" void kickos_terminal_outcome_apply(void);

extern "C" __attribute__((noreturn)) void kfault_terminate(void)
{
    arch_console_flush_sync();
    kickos_terminal_outcome_apply();
    arch_shutdown(KICKOS_FATAL_STATUS);
}
