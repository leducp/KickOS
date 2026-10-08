// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A synchronous illegal-instruction fault of the running thread, for the apps and drivers that
// fault on purpose.

#ifndef KICKOS_SYS_TRAP_H
#define KICKOS_SYS_TRAP_H

// The instruction as an asm string, for a statement that moves SP first. Absent on a host.
#if defined(__XTENSA__)
#define KOS_TRAP_ILLEGAL_INSN "ill"
#elif defined(__riscv)
#define KOS_TRAP_ILLEGAL_INSN ".word 0x00000000" // all-zero encoding: illegal on RV32 and RV64
#elif defined(__arm__) || defined(__thumb__)
#define KOS_TRAP_ILLEGAL_INSN "udf #0"
#elif defined(__aarch64__)
// All-zero is a permanently-undefined A64 encoding, reported with EC 0x00. NOT
// __builtin_trap, which is `brk` on this ISA and raises a DEBUG exception (EC 0x3C).
#define KOS_TRAP_ILLEGAL_INSN ".inst 0x00000000"
#elif defined(__RX__)
// MVTIPL in user mode is a defined privileged-instruction exception (RXv3 ISA UM sec.5.1.2).
// IPL is already 0, so an execution that lands in supervisor mode changes nothing. GCC lowers
// __builtin_trap to abort().
#define KOS_TRAP_ILLEGAL_INSN "mvtipl #0"
#endif

#if defined(KOS_TRAP_ILLEGAL_INSN)
#define KOS_TRAP_ILLEGAL() __asm volatile(KOS_TRAP_ILLEGAL_INSN)
#else
#define KOS_TRAP_ILLEGAL() __builtin_trap() // host: x86 ud2 -> SIGILL
#endif

#endif
