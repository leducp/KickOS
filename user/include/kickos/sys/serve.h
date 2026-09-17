// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Whether a kos_reply_recv refusal ends the TRANSACTION or ends the SERVICE. One header for
// every recv/dispatch loop, because the answer is a property of the syscall and not of the
// wire protocol layered over it.
//
// A SHARED SERVICE MUST SURVIVE ANYTHING ONE CLIENT CAN PROVOKE. The fused call carries the
// reply and the receive together, so a refusal that belongs to the client the server is
// answering arrives on the same return as a refusal that belongs to the server itself, and a
// loop that leaves on any negative result lets one client's bad reply buffer take the console
// away from every other client.
//
// The line is the one the kernel already draws between the fused call's two halves
// (kernel/syscall/syscall_ipc.cc): a code that says the server's own capabilities or arguments
// are wrong leaves it nothing to serve with, and a code that names the far end of a copy says
// nothing about the server at all.

#ifndef KICKOS_SYS_SERVE_H
#define KICKOS_SYS_SERVE_H

#include <kickos/sys/errno.h>

#include <stdint.h>

namespace kickos
{

// True where a negative kos_reply_recv result ends only the transaction in flight, so the
// loop drops the reply capability it was carrying and receives again.
//
// ONE CODE AND NOT A DEFAULT-CONTINUE, so a code this list does not name ends the service
// loudly instead of spinning silently: a loop that continued on its own argument being
// refused would re-issue the same refused call forever, and an exit is at least a respawn.
//
// -KOS_EFAULT is the only thing a client can do to a server that is otherwise well formed.
// The server's own buffer and opts struct are its stack and are proved accessible before
// anything is copied, so the side of a copy that can still be gone is the client's.
//
// NOT -KOS_ETIMEDOUT. A deadline is the server's own policy rather than anything a client
// did, and a loop that both asks for one and continues on it spins at its own priority, so
// whoever sets opts.timeout_us owns what the expiry means.
inline bool serve_transaction_failed(int32_t rc)
{
    return rc == -KOS_EFAULT;
}

}

#endif // KICKOS_SYS_SERVE_H
