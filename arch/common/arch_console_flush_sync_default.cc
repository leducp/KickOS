// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Taking this fallback means the chip's console cannot outrun EITHER caller: not a clock
// retune, and not arch_shutdown. The second half is the easy one to get wrong: a chip
// that never retunes still truncates its last line at shutdown if its FIFO outlives the
// core, which is what f302nucleo did. See arch.h for the contract.

#include <kickos/arch/arch.h>

extern "C" void arch_console_flush_sync(void)
{
}
