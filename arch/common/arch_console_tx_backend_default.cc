// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Null leaves the console on the synchronous path (sim + polled-only chips). The
// struct stays incomplete here: the arch layer does not carry lib/'s console_tx.h on
// its include path, and a pointer return needs no definition.

#include <stdint.h>

extern "C"
{
    struct console_tx_backend;

    struct console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size,
                                                             int* irq_line)
    {
        (void)storage;
        (void)size;
        (void)irq_line;
        return nullptr;
    }
}
