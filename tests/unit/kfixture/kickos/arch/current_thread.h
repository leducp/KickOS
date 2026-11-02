// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K-seam's running-thread read: one read of the fixture core's current cell, the form a
// port with a per-core register gives, so it opens no gap. The store has nothing to mirror.

#ifndef KICKOS_TESTS_UNIT_KFIXTURE_KICKOS_ARCH_CURRENT_THREAD_H
#define KICKOS_TESTS_UNIT_KFIXTURE_KICKOS_ARCH_CURRENT_THREAD_H

#if KICKOS_KERNEL_CORES > 1

#define KICKOS_ARCH_CURRENT_ONE_READ 1

void* arch_current_load(void);
void arch_current_store(void* t);

#endif

#endif
