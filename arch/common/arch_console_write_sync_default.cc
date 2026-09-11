// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// EVERY CHIP MUST DEFINE ITS OWN BOUNDED POLLED WRITER, and this body is a trap marker
// rather than a service: arch_console_write enters console_tx_insert_line, whose refusal
// path calls console_write_line_sync -> arch_console_write_sync, so a chip that resolves
// here recurses off its stack on the panic path.

#include <kickos/arch/arch.h>

#include <stddef.h>

extern "C" void arch_console_write_sync(char const* buf, size_t n)
{
    arch_console_write(buf, n);
}
