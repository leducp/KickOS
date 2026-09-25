// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The forced TLS configuration shared by the shim that compiles kernel/thread/tls.cc and
// the cases that read its results. Split out so the stride the shim compiles against and
// the stride the cases pass in cannot drift apart.

#ifndef KICKOS_TESTS_UNIT_TLSCARVE_TLSCARVE_CONFIG_H
#define KICKOS_TESTS_UNIT_TLSCARVE_TLSCARVE_CONFIG_H

// Power of two, and wider than any block the fabricated sections below can produce.
#ifndef TLSCARVE_STRIDE
#define TLSCARVE_STRIDE 2048u
#endif

// The armv6m/armv7m/xtensa value: a variant 1 arch reserves this much BELOW the thread
// pointer. Picking the non-zero one keeps the reserve inside every arithmetic check.
#define TLSCARVE_TCB 8u

// WHICH MECHANISM DERIVES THE THREAD POINTER. 1 is the masking arch, where the stride is a
// real constraint on every stack block; 0 is the arch that seats the register from the
// context and so owes the stride nothing.
#ifndef TLSCARVE_FROM_SP
#define TLSCARVE_FROM_SP 1
#endif

// 1 keeps the control block, which holds libc's reentrant-state pointer, with no thread_local.
#ifndef TLSCARVE_REENT_IN_TCB
#define TLSCARVE_REENT_IN_TCB 0
#endif

#endif
