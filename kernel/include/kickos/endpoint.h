// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ENDPOINT_H
#define KICKOS_ENDPOINT_H

#include <stdint.h>

#include <kickos/list.h>

namespace kickos
{
    // A cap-named synchronous rendezvous point. No kernel payload storage: the
    // parked side's own (bound-checked, stable-because-BLOCKED) user buffer is the
    // storage, and the ARRIVING thread does the bounded copy under IrqLock.
    // INVARIANT (MMU-era load-bearing): the endpoint names nothing by address -- no
    // physical buffer address is stored here or handed out as a badge. The transient
    // user-buffer pointer lives in the parked thread's TCB ipc descriptor for the
    // rendezvous only; ep_copy reaches it via the arch's privileged access. Two
    // waitqs; both are the shared List + wq_pop_highest primitive (sem/mutex are the
    // other users). INVARIANT: the two waitqs are never simultaneously non-empty (an
    // arrival always drains the opposite queue before it parks on its own).
    struct Endpoint
    {
        List send_waiters;    // parked senders (buffer descriptor in their TCB)
        List recv_waiters;    // parked receivers (buffer + badge-out descriptor in their TCB)
        // Live caps carrying CAP_WAIT (recv right) naming this endpoint. NOT the pool
        // refcount (endpoint_refs[] counts ALL caps): it gates the send-side
        // dead-endpoint check and fires EPIPE at 0. The single home for this state.
        uint8_t recv_holders;
    };
}

#endif
