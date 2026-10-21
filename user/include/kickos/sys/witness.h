// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The witness kernels' system calls. KOS_SYS_WITNESS_UACCESS is a ring-0 access to a user
// pointer, made on purpose: only a kernel configured by tests/integration/check_uaccess.sh
// carries it, and its banner says `witness uaccess`. KOS_SYS_WITNESS_REWINDS answers the
// switcher's rewind count: only a kernel configured by tests/integration/check_rewind.sh carries
// it, and its banner says `witness rewind`.

#ifndef KICKOS_SYS_WITNESS_H
#define KICKOS_SYS_WITNESS_H

#ifndef KICKOS_WITNESS_UACCESS
#define KICKOS_WITNESS_UACCESS 0
#endif

#ifndef KICKOS_WITNESS_REWIND
#define KICKOS_WITNESS_REWIND 0
#endif

// Each a ring-0 touch of uaddr at a different point. Append new values.
enum kos_witness_uaccess_op
{
    KOS_WITNESS_UREAD_NOW = 0,         // a read on the fresh entry from user mode
    KOS_WITNESS_UWRITE_NOW = 1,        // a write of 0xA5 on the fresh entry
    KOS_WITNESS_UREAD_AFTER_PARK = 2,  // a read once a sleep of ns has parked and resumed the
                                       //   caller, through the frame its switch built
    KOS_WITNESS_UREAD_SPACELESS = 3,   // a read under the root a core installs when no space
                                       //   is its own
    KOS_WITNESS_UREAD_AT_FAULT_EXIT = 4, // arms a read of uaddr in the fault-exit stub, which the
                                         //   caller then reaches by faulting: the frame the kill
                                         //   redirect built
    KOS_WITNESS_UFETCH_NOW = 5,          // a call to uaddr, user text returning a byte, on the
                                         //   fresh entry
    KOS_WITNESS_UREAD_IN_TIMER_IRQ = 6   // arms a read of uaddr in the first timer interrupt at
                                         //   least ns ahead that the caller takes, and programs
                                         //   one: the entry from user mode an interrupt builds
};

#endif
