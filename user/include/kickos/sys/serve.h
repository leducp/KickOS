// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Error classification shared by reply-receive service loops.

#ifndef KICKOS_SYS_SERVE_H
#define KICKOS_SYS_SERVE_H

#include <kickos/sys/errno.h>

#include <stdint.h>

namespace kickos
{

// True for a transaction error that lets the service receive again.
// These loops use valid server buffers, so EFAULT can come from the client.
// Other errors stop the loop to avoid retrying invalid server arguments.
// Timeout handling belongs to whichever service sets a deadline.
inline bool serve_transaction_failed(int32_t rc)
{
    return rc == -KOS_EFAULT;
}

}

#endif // KICKOS_SYS_SERVE_H
