// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The forced TLS configuration shared by the shim that compiles kernel/thread/tls.cc and
// the cases that read its results. Split out so the stride the shim compiles against and
// the stride the cases pass in cannot drift apart.

#ifndef KICKOS_TESTS_UNIT_TLSCARVE_TLSCARVE_CONFIG_H
#define KICKOS_TESTS_UNIT_TLSCARVE_TLSCARVE_CONFIG_H

// Power of two, and wider than any block the fabricated sections below can produce.
#define TLSCARVE_STRIDE 2048u

// The armv6m/armv7m/xtensa value: a variant 1 arch reserves this much BELOW the thread
// pointer. Picking the non-zero one keeps the reserve inside every arithmetic check.
#define TLSCARVE_TCB 8u

#endif
