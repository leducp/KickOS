// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ENDPOINT_H
#define KICKOS_ENDPOINT_H

#include <stdint.h>

#include <kickos/list.h>

namespace kickos
{
    struct Thread; // kickos/thread.h

    // A cap-named synchronous rendezvous point. There is NO kernel payload storage: the
    // parked side's own user buffer is the storage, stable because that side is BLOCKED,
    // and the ARRIVING thread does the bounded copy under IrqLock.
    // INVARIANT: the endpoint names nothing by address. No buffer address is stored here
    // or handed out as a badge; the transient user-buffer pointer lives in the parked
    // thread's TCB ipc descriptor for the rendezvous only.
    // INVARIANT: the two waitqs are never simultaneously non-empty, because an arrival
    // always drains the opposite queue before parking on its own.
    struct Endpoint
    {
        List send_waiters;    // parked senders (buffer descriptor in their TCB)
        List recv_waiters;    // parked receivers (buffer + badge-out descriptor in their TCB)
        // Live caps carrying CAP_WAIT naming this endpoint, and the single home for that
        // state. NOT the pool refcount, which counts ALL caps: this one gates the send-side
        // dead-endpoint check and fires EPIPE at 0. Shares endpoint_refs' uint8_t ceiling
        // and refusal, because obj_ref_inc tests both before moving either.
        uint8_t recv_holders;
        // The conventional single receiver, re-set at every recv. One server per endpoint
        // is documented, not enforced. Also the boost target when a caller parks on
        // send_waiters. MUST be cleared in the endpoint close/teardown arm when the server
        // drops its WAIT cap, else this raw pointer dangles onto a reused TCB.
        Thread* server;
    };
}

#endif
