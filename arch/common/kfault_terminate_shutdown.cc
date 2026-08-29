// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// The fault dead-end for a chip whose arch_shutdown reaches a debug or emulator channel
// that turns the status into a process exit (terminate.cmake). The LED fallback beside this
// file would spin until the harness times out instead.
//
// Lone TU (arch/CMakeLists.txt states the rule): exactly one symbol, and the opt-in drops
// kfault_terminate_default.cc from this archive rather than leaving both members in it.
//
// The flush is what a chip whose console can outrun a shutdown needs; where the console
// hands every byte to the host inside the write, it resolves to the no-op fallback. Which
// of the two a board gets is arch_console_flush_sync's contract, not this function's.

#include <kickos/arch/arch.h>

#include "fatal_status.ld.h"

extern "C" __attribute__((noreturn)) void kfault_terminate(void)
{
    arch_console_flush_sync();
    arch_shutdown(KICKOS_FATAL_STATUS);
}
