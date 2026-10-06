// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default system's main faulting: the task ends with KOS_EXIT_FAULT, which ends the system
// with that status.

#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("sysdefault: main faults\n");
    fflush(stdout);
    // Not a store to address 0, which no unit faults where none protects it.
#if defined(__XTENSA__)
    __asm volatile("ill");
#elif defined(__riscv)
    __asm volatile(".word 0x00000000");
#elif defined(__arm__)
    __asm volatile("udf #0");
#elif defined(__aarch64__)
    __asm volatile(".inst 0x00000000");
#elif defined(__RX__)
    // BRK is the catch-all trap, never a thread's fault; WAIT in user mode is a privileged
    // instruction exception (RXv3 ISA 1.4.3, p.30).
    __asm volatile("wait");
#else
    __builtin_trap();
#endif
    return 0;
}
