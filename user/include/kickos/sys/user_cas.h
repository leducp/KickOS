// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user-mode compare-and-swap on a 32-bit word, atomic against the threads of one kernel
// instance: on armv6m and rxv3 it is a restartable sequence, which a thread of another instance
// (the other core of an AMP part) does not respect.
//
// It orders as acquire and release against those threads. armv8a, rv32imac, rv64imac, lx6,
// x86_64, v8-M Mainline and the sim carry that ordering in the routine itself; armv6m, rxv3 and
// v7-M, whose instances run on one core, carry no barrier, so an observer outside the instance
// (a DMA master, another instance's core) is promised no order there.

#ifndef KICKOS_SYS_USER_CAS_H
#define KICKOS_SYS_USER_CAS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Stores desired at word if it holds expected, and answers what it held: the store landed
// exactly when the answer is expected.
uint32_t kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_SYS_USER_CAS_H
