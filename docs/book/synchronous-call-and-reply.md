<!-- SPDX-License-Identifier: CECILL-C -->
# Synchronous call/reply: the reply capability

> A device driver in a microkernel is an unprivileged server: a client asks it to do a
> transaction and waits for the result. The endpoint of Chapter 8.3 gives one-way send and
> one-way receive; a request-then-await-response needs a second half. This chapter teaches
> that half -- the one-shot reply capability -- as a minimal delta over the rendezvous, why
> it needs no new kernel object, why it must carry priority with it, and how a bus driver is
> built from it. It builds on Chapter 8.3 (the endpoint it extends), Chapter 8.1 (the
> handle that names the reply), Chapter 2.3 (the priority-donation vocabulary it reuses),
> and points into `../reference/ipc-call-reply.md` and `../reference/bus-service.md` for the
> exact contracts.

## Why one-way messages are not enough

The endpoint (Chapter 8.3) is deliberately one-way: a send meets a receive, one bounded
copy happens, both proceed. That is the right primitive for a byte stream -- a console
driver receives bytes a client sent and never "answers" a particular send.

A bus driver is different. A client asks an SPI driver to *clock these bytes and give me
back what came in*, and it cannot continue until it has the answer. This is remote
procedure call: request, block, response. Built from raw endpoints it is clumsy and
unsafe. The client would need its own endpoint for the driver to send the answer back on,
the driver would have to be told which endpoint to use, and nothing would stop a second
client from receiving the first client's answer. The pattern is common enough -- every
SPI, I2C, block, and network transaction is a call -- that it deserves a primitive.

*Further reading: the L4 microkernel family, whose call/reply "fastpath" this follows;
Liedtke, "Improving IPC by Kernel Design."*

## The options: a reply endpoint, or a reply capability

**A reply endpoint per client.** Give every client a private endpoint the server sends the
answer on. It works, but it is heavy: an endpoint object per client, a way to tell the
server which one to use per request, and the server must be trusted to send to the right
one. The isolation you want -- this answer reaches exactly this caller and no one else --
is a convention, not a mechanism.

**A reply capability.** Observe what the kernel already knows at the moment a call is
delivered: the caller is *blocked*, waiting for exactly this answer. A blocked thread is
not running, so its control block -- where its buffer pointer, its capacity, its priority
all live -- is stable, kernel-visible state. If the kernel hands the server a one-shot
*capability that names that blocked caller*, the server can complete it once and only
once, and the kernel guarantees the answer lands in that caller's buffer and wakes that
caller. No per-client endpoint, no addressing convention, no trust.

## What KickOS chose: the parked caller IS the reply object

KickOS takes the reply capability, and pushes the observation to its conclusion: **there
is no reply-object pool at all.** The reply capability is an ordinary entry in the
server's handle table (Chapter 8.1) whose object field *names the parked caller* by its
generational thread handle. The caller's own blocked control block is the reply object;
the capability is the server's one-shot right to complete it.

This costs almost nothing and breaks no existing invariant:

- **No new pool, no new allocation.** The caller already exists and is already parked; the
  kernel spends one handle-table slot in the server, which it was going to spend on
  *some* naming of the reply anyway.
- **It names by handle, not by address.** Like every capability, it stores a generational
  handle, never a raw pointer -- so it inherits the resolve chokepoint's staleness
  guarantee (Chapter 8.1). If the caller somehow went away and its thread slot were
  reused, the generation would not match and the reply would resolve to nothing.
- **It is rights-less and one-shot.** The capability carries no send/receive/transfer
  rights: it cannot be delegated, duplicated, or waited on -- only replied through once,
  or closed. Completing it consumes it (the slot is emptied and its generation bumped), so
  a second reply on the same handle simply finds nothing.

The caller does not park on a wait queue the way a plain sender does. Once its request has
been picked up it is blocked but *queue-less*, bound to the reply capability rather than to
the endpoint. That is a small new thread state, and the one place the kernel had to check
that "blocked" no longer implies "on a queue."

## Why the fast path is the isolation

The elegant part is what the reply capability buys structurally. When the server is
already waiting to receive, a call is a direct handoff: under one critical section the
kernel copies the request into the server's buffer, mints the reply capability, and
switches straight to the server. The server runs the transaction -- touching its device
registers -- *under its own MMIO grant*, because it is an ordinary unprivileged thread
whose memory-protection regions include exactly that device window and nothing else
(Chapter 7). Then it replies, and the kernel switches straight back to the caller.

So the request never passes through privileged code that "does the transaction on the
client's behalf." The driver is not a syscall; it is a peer in another protection domain,
reached by a message, doing the work under its own authority. The fast path and the
isolation are the same thing: the capability *is* the fast path (Chapter 8.4 makes the
same argument for a GPIO toggle). Nothing is gained by wrapping the driver in a kernel
service -- that would only re-pay the boundary crossing and dissolve the isolation.

## Why donation is required, and one funnel that keeps it honest

There is a trap in synchronous RPC that the endpoint chapter's "no priority inheritance"
note foreshadows. Suppose a high-priority client calls a low-priority driver. The driver
runs the transaction at *its own* low priority. Now a medium-priority thread becomes
ready. It preempts the driver -- which is, right now, doing the high-priority client's
urgent work -- and the transaction stalls for as long as the medium thread wants to run.
The high-priority client is blocked the whole time. This is priority inversion, and on a
bus doing cyclic real-time traffic it is fatal.

The fix is the same idea as the priority-inheritance mutex (Chapter 2.3): while the driver
is serving a caller, it should run at the caller's priority. So a call *donates* the
caller's urgency to the server across the handoff, and if a caller has to queue because the
server is busy, it boosts the server the moment it enqueues -- not later. When the reply is
sent, the donation is withdrawn.

The danger with any such scheme is withdrawing wrongly. If you "restore the server to its
base priority" on reply, you clobber a boost it legitimately still holds -- from a mutex it
owns, or another caller queued behind this one. Chapter 2.3 already solved this for mutexes
with a single rule: never restore, always *recompute* the effective priority from all live
reasons to be boosted. KickOS extends that one function to also account for reply
capabilities and queued callers, and routes the mutex path through it too. There is exactly
one place that computes a thread's effective priority, and it looks at every source at once:
the thread's base, the mutexes it holds, the callers behind its reply capabilities, and the
callers queued on the endpoint it serves. Withdraw a donation and the recompute simply stops
counting that source; anything else still boosting the thread survives. Two separate
recompute paths would eventually disagree and let a driver deflate below a donation it still
owes -- one funnel or the invariant rots.

## Refusing a call the server cannot host

A reply capability is minted into the *server's* handle table, and that table is finite.
Two failure shapes have to be honest, and both are resolved by probing before committing:

- A server that receives with no interest in replying -- an info-less receive, the shape a
  byte-stream server like the console uses -- must not be handed a reply capability at all.
  A call against such a server fails cleanly rather than minting a capability the server
  will never complete. This also closes a denial-of-service path: every task holds a send
  right on the console, and if a call could force a reply capability into the console's
  table, hostile clients could fill it and pin its priority. Because the console receives
  info-less, their calls simply bounce.
- A server that cannot mint refuses the call. Two things stop it: its table has no free slot,
  or it already holds as many parked callers as it was provisioned for. The second is what keeps
  the first from being reachable by peers alone -- without a bound on inbound replies, enough
  simultaneous callers could fill a server's table and leave it unable to create anything of its
  own, which is a failure its own code never asked for. Either way the kernel discovers it
  *before* it pops the waiting server or copies anything, so a call that cannot be hosted fails
  with no side effect rather than stranding a half-served server off its queue.

Everything else is the death matrix in the Reference: a server that dies mid-transaction,
a reply capability closed instead of replied, a stale caller -- each wakes the caller with
a broken-pipe error or is a cheap no-op, and each consumes the capability exactly once. The
guarantee a client relies on is simple: a call returns, one way or another. It is never
left blocked forever by a server's misbehavior (the one exception being cycles a client
builds itself -- calling itself, or two servers calling each other -- which have no
detection and no timeout).

## The service model: a class, and a thread that speaks the wire

With the primitive in hand, a driver is built in two layers, and the split is the same
constructor-freedom boundary the driver model draws elsewhere (Chapter 3.7).

- **The class is the transaction engine.** Given a device register window and a request, it
  clocks the bytes: fill and drain the FIFO, assert and release chip-select, poll or wait
  on the completion IRQ. It is chip-specific, freestanding, and takes its register base as
  an explicit argument -- no constructor, no static state -- so the *same* engine code can
  run inside the privileged kernel or inside an unprivileged driver.
- **The service is a thread that owns one class instance and speaks a wire format.** Its
  whole life is a loop: receive a request, run it through the class under the MMIO grant,
  reply. One service thread per controller instance -- four SPI controllers are four
  threads, each with its own device window, its own endpoint, its own chip-select surface.
  This is the honest expression of "a grant is a security boundary": a fault in one
  controller's driver cannot reach another's window.

The wire format between client and service is a small request/reply struct -- a header, a
list of segments, and inline payload bytes -- that describes a bus transaction without
naming any controller's registers. The exact layout, the segment model (full-duplex for
SPI, addressed phases for I2C), and the chip-select policy live in
`../reference/bus-service.md`. The one discipline worth stating here because it is a
correctness rule, not a detail: **the service replies on every path, including the error
paths.** A reply capability the service drops instead of completing is a client blocked
forever. The loop must consume it -- with a real result or with an error status -- every
time.

## How a driver reaches its client

One question remains: how does a client come to hold a capability on the driver's endpoint
in the first place? The answer ties straight back to the handle table and the resolve
chokepoint (Chapter 8.1). Capabilities are distributed statically, at spawn. When init
brings up a bus service it creates the endpoint, then spawns the driver granted a
receive-only capability on it and hands clients a send-bearing capability on the same
endpoint by delegation -- each capability narrowed to exactly the rights that side needs
(the driver receives and does not re-delegate; the client sends and cannot receive). The
client never "looks up" the driver by a global name; it is *given* a handle, and every use
of that handle resolves through the one chokepoint that checks liveness, type, and rights.
A client that was not given the capability cannot manufacture one -- which is the whole
point of capability-based authority, applied to reaching a driver.

The board decides *which* services exist and how they are configured -- their register
base, window size, priority, chip-select choice, target clock -- and it says so as data: an
ordered service list and a per-instance config block the default init walks at bring-up,
before the application's `main`. That keeps the routing space (which peripheral, which
pads, how many instances) where it belongs, with the product integrator, and out of the
kernel. The shape of that data surface is in `../reference/architecture.md` (service
publication) and `../reference/bus-service.md`.

## Where to go next

- The endpoint rendezvous this extends, and its deliberate one-way minimalism:
  Chapter 8.3, *Endpoints: synchronous IPC by rendezvous*.
- The handle that names a reply, and the resolve chokepoint every use passes through:
  Chapter 8.1, *Naming a kernel object*.
- The priority-donation vocabulary and the recompute-not-restore rule the funnel unifies:
  Chapter 2.3, *Priority inheritance*.
- Why a driver's device access is direct MMIO under its own grant, not a kernel call:
  Chapter 8.4, *The fast path is the capability*.
- The exact numbers, error codes, donation contract, and limits:
  `../reference/ipc-call-reply.md`. The bus wire format and chip-select policy:
  `../reference/bus-service.md`.
