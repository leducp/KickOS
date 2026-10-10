// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability rights and the thread authority word, shared verbatim by the kernel and every
// userspace TU. The kernel's own names alias these values and define none of their own.

#ifndef KICKOS_SYS_CAP_RIGHTS_H
#define KICKOS_SYS_CAP_RIGHTS_H

// Object capability rights: the rights of a semaphore, mutex, endpoint or reply cap. A
// delegation NARROWS only: the child cap gets parent.rights & mask, and a mask adding a bit
// the parent lacks is rejected, the one exception being KOS_CAP_WAIT from an endpoint cap
// carrying KOS_CAP_HANDOUT. Delegating requires the parent cap carry KOS_CAP_TRANSFER.
enum kos_cap_rights
{
    KOS_CAP_WAIT = 1 << 0,     // sem_wait; endpoint recv
    KOS_CAP_SIGNAL = 1 << 1,   // sem_post; endpoint send
    KOS_CAP_TRANSFER = 1 << 2, // may be delegated onward
    // Endpoint only: a delegation may grant KOS_CAP_WAIT from this cap although it holds no
    // WAIT itself. Its holder is no receiver, and while one exists a call the endpoint has no
    // receiver for answers -KOS_EAGAIN rather than -KOS_ECONNREFUSED.
    KOS_CAP_HANDOUT = 1 << 3
};

// The thread's authority word: its own field, sharing no numbering with kos_cap_rights. It is
// TCB state and not a table entry, so a spawning parent is what seats it. A thread may pass a
// bit to a child (kos_thread_params::authority) only if it holds that bit, and may drop bits
// with kos_cap_narrow(KOS_CAP_AUTHORITY, mask). Nothing widens. One bit per distinct holder:
// a bit merging two holders grants each the other's power.
enum kos_cap_authority
{
    KOS_AUTH_MEMORY = 1 << 0,  // kos_ram_alloc, a spawn's device window, kos_mem_self_grant
    KOS_AUTH_PINMUX = 1 << 1,  // kos_pinmux_set
    KOS_AUTH_PSTATE = 1 << 2,  // kos_cpu_clock_set
    KOS_AUTH_IRQ = 1 << 3,     // kos_irq_claim
    KOS_AUTH_SYSTEM = 1 << 4,  // kos_shutdown, kos_reboot
    KOS_AUTH_CONSOLE = 1 << 5, // kos_console_publish
    KOS_AUTH_TASKS = 1 << 6,   // kos_task_create, and a spawn that builds a task of its own
    KOS_AUTH_BUS_MASTER = 1 << 7 // a spawn's device window over a device that masters the bus
};

// The only place the full set is written. It must name every authority bit:
// cap_seat_authority masks a seated word to it.
#define KOS_AUTH_ALL                                                                           \
    (KOS_AUTH_MEMORY | KOS_AUTH_PINMUX | KOS_AUTH_PSTATE | KOS_AUTH_IRQ | KOS_AUTH_SYSTEM     \
     | KOS_AUTH_CONSOLE | KOS_AUTH_TASKS | KOS_AUTH_BUS_MASTER)

#endif
