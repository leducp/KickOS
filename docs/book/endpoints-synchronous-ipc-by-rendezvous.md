<!-- SPDX-License-Identifier: CECILL-C -->
# Endpoints: synchronous IPC by rendezvous

> In a microkernel the interesting work -- drivers, filesystems, networking -- lives in
> unprivileged userspace servers, and the only way an isolated client reaches a server
> is a kernel-mediated message. This chapter teaches KickOS's IPC primitive: the
> endpoint, a synchronous rendezvous with a kernel-copied bounded payload. It builds on
> Chapter 2.2 (the wait/wake substrate an endpoint parks on), Chapter 8.1 (the
> capability that names it), and Chapter 8.2 (the additive object recipe it follows).
> It points into `../reference/architecture.md` ("Object model, capabilities & IPC")
> for the exact contract and `../reference/porting.md` for the cross-domain-write
> porting requirement.

## Why IPC is the whole point

A monolithic kernel puts the disk driver, the TCP stack, and the filesystem *inside*
the trusted kernel, where they call each other as ordinary functions. A microkernel
does not: those services are unprivileged tasks in their own protection domains
(Chapter 7), and a client in a different domain cannot call them directly -- the whole
point of the isolation is that it *cannot*. So the client and the server need a
channel that crosses the domain boundary, and the only party both of them trust to
carry a message across is the kernel.

That channel is inter-process communication, IPC, and in a microkernel it is not a
peripheral feature -- it is *the* feature. Every `open`, `read`, `socket` that a
monolithic kernel answers with a function call, a microkernel answers with a message
to a server. If IPC is slow, the whole system is slow; if IPC is unsafe, the isolation
is theater. So the primitive has to be small, fast, and airtight.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (message passing) and the
seL4 IPC design, whose synchronous-endpoint shape this follows.*

## The options: buffered channel vs synchronous rendezvous

**Buffered channel.** The kernel holds a queue of messages per channel. A sender can
deposit a message and move on whether or not a receiver is ready, up to the buffer's
capacity; a receiver takes from the queue. This decouples the two sides, but it makes
the kernel a message *store*: it must allocate per-channel buffer memory, decide a
backpressure policy for when the buffer fills, and account for messages in flight.
That is real kernel state and real kernel storage, per channel, for a feature whose
job is to stay minimal.

**Synchronous rendezvous.** There is no kernel-side message storage at all. A send and
a receive must *meet*: whoever arrives first parks (Chapter 2.2), and when the second
arrives, the message is copied directly between the two threads' own buffers and both
proceed. The kernel holds only "who is waiting," never the message itself. This is the
seL4 endpoint shape.

## KickOS's choice: synchronous rendezvous

KickOS uses the synchronous rendezvous, for one decisive reason: **the parked side's
own buffer is the storage.** A blocked thread is not running, so its message buffer
does not change and -- because a thread's MPU regions are fixed for its lifetime
(Chapter 7) -- that buffer stays valid and reachable. The kernel needs no message
memory of its own; it needs only two wait queues and a small count. That matches the
minimalism of the two-object synchronization surface (Chapter 2.3) and reuses the
blocking substrate wholesale.

The object is correspondingly tiny:

```
struct Endpoint
{
    List    send_waiters;   // senders parked waiting for a receiver
    List    recv_waiters;   // receivers parked waiting for a sender
    uint8_t recv_holders;   // live capabilities that carry the receive right
};
```

Two wait queues, and one count whose job appears below. There is a structural
invariant worth stating: **the two queues are never both non-empty.** If a receiver is
parked, an arriving sender hands off to it immediately rather than parking -- and vice
versa -- so at most one side ever waits. A waiting sender means "no receiver was
here"; a waiting receiver means "no sender was here"; they cannot both be true.

## The rendezvous: the arriving thread copies, under the lock

Here is the load-bearing invariant of the whole design:

> **The arriving (running) thread performs the copy, under one `IrqLock`.**

When a sender arrives to find a receiver already parked, *the sender* copies its data
into the receiver's buffer. When a receiver arrives to find a parked sender, *the
receiver* copies out of the sender's buffer. It is the same copy, opposite direction,
and it is done by whichever thread is currently running.

Why this is safe is the elegant part. At the moment of copy, both buffers are stable:

- The **running** thread's buffer is stable because it is the one running -- nothing
  else can touch it.
- The **parked** thread's buffer is stable because that thread is `BLOCKED` (it is not
  running, so it cannot mutate the buffer) and its MPU regions are immutable for its
  lifetime (so the memory cannot be remapped underneath the copy).

There is no time-of-check-to-time-of-use gap, either: the only way a KickOS thread
goes away is by exiting itself (there is no `thread_kill`), so a parked peer cannot
vanish mid-copy. And the resolve, the peer check, and the copy-or-park all happen
inside *one* continuous `IrqLock`, so no third party interleaves.

The copy is a bounded `memcpy` between the two user buffers. The size is `n = min(sender
length, receiver capacity)`: a receiver with a smaller buffer *truncates* the message
(datagram semantics -- both sides simply learn `n` bytes moved), which is not an error.
A zero-length send is a legitimate `n == 0` signal, not a failure -- userspace must not
conflate it with the `-1` that means "something went wrong."

The parked side leaves behind exactly what the waker needs -- a small descriptor in its
own thread control block:

```
struct IpcDesc
{
    uintptr_t buf;       // the parked thread's own (already bound-checked) buffer
    size_t    len;       // sender: bytes to send; receiver: buffer capacity
    uintptr_t badge_out; // receiver: where to store the sender's badge, or 0
};
```

This descriptor is the endpoint's "step 2" in the three-step wake protocol
(Chapter 2.2): the arriving thread reads the parked peer's `IpcDesc`, does the copy,
writes the byte count into the peer's `wait_result`, and wakes it. On resume the parked
thread reads only `wait_result` (after the confirm-resume barrier -- Chapter 2.2); it
never re-reads its own descriptor.

Sketch of the send path (the receive path is its mirror):

```
kos_send(cap, buf, len):
    if len > KOS_EP_MSG_MAX:            return -1   // reject oversize; do not clamp
    if not user_readable_ok(buf, len):  return -1   // sender's own buffer, checked once
    {
        IrqLock lock
        e = cap_resolve(current, cap, CAP_ENDPOINT, CAP_SIGNAL)   // need the send right
        if e == nullptr:            return -1
        if e->recv_holders == 0:    return -1        // dead endpoint (below)
        w = wq_pop_highest(e->recv_waiters)
        if w != nullptr:                             // a receiver is parked -> deliver now
            n = min(len, w->ipc.len)
            copy buf -> w->ipc.buf, n bytes          // running sender copies into parked receiver
            w->wait_result = n
            sched::wake(w)
            return n                                  // did not block
        current->ipc = { buf, len, ... }             // no receiver -> park
        sample epoch; wq_block(e->send_waiters)
    }
    wq_confirm_resume(current, epoch)                // Chapter 2.2 barrier
    return current->wait_result                       // n (>= 0), or -1 (broken pipe)
```

The bound-checks run **up front, in the caller's context**, before the lock -- each
buffer checked against *its own owner's* regions: the sender's buffer for read, the
receiver's buffer (and any badge-out pointer) for write. An unchecked receive buffer
would be a privileged-write oracle; the check is what closes it. (This is the
syscall-argument validation floor -- Chapter 7.1 and the Reference's "Syscall-argument
validation".)

## Two counts, two jobs: refs and recv_holders

An endpoint follows the additive object recipe (Chapter 8.2), so it has the usual
`endpoint_refs[]` refcount that governs *object lifetime* -- how many capabilities name
it, freed at zero. But it *also* carries `recv_holders`, and conflating the two is the
mistake to avoid.

`recv_holders` counts only the live capabilities that carry the **receive right**
(`CAP_WAIT`). It answers a different question than the refcount: not "does anything
still name this endpoint?" but "can a receiver ever appear?" It drives two behaviors a
plain refcount cannot:

- **The dead-endpoint gate.** When a sender arrives, if `recv_holders == 0` there is no
  receiver and -- because rights are only ever narrowed on delegation, never widened
  (Chapter 8.1) -- no new receive capability can ever be minted. So the send fails
  immediately with `-1` rather than parking forever. A send-only client closing its own
  capability drops `endpoint_refs` but not `recv_holders`, so it must *not* trigger
  this; that is exactly why the two counts are separate.
- **Broken pipe (EPIPE).** When the *last* receive-holder goes away -- closes its
  capability or exits -- any senders already parked would otherwise wait forever. So
  the close protocol (Chapter 8.2) for an endpoint, on dropping `recv_holders` to zero,
  wakes every parked sender with `wait_result = -1`:

```
case CAP_ENDPOINT:   // obj_close_protocol arm; runs on voluntary close AND teardown
    ep = resolve(e.obj)
    if ep != nullptr and (e.rights & CAP_WAIT) != 0 and ep->recv_holders > 0:
        ep->recv_holders--
        if ep->recv_holders == 0:
            while ((s = wq_pop_highest(ep->send_waiters)) != nullptr):
                s->wait_result = -1        // broken pipe
                sched::wake(s)
    return 0     // an endpoint NEVER refuses a close (unlike the mutex)
```

The dead-endpoint gate and the EPIPE wake are gap-free with respect to each other
precisely because both run under the one `IrqLock`: a sender either parks while
`recv_holders >= 1` (and is later EPIPE-woken by the last close) or observes
`recv_holders == 0` and fails at once. There is no window in between.

Note the rights contrast with the mutex. For a mutex, possession *is* the authority --
there is no meaningful send/receive split (Chapter 2.3). For an endpoint the split is
real and deniable: `CAP_SIGNAL` gates send, `CAP_WAIT` gates receive, and a server can
hand a client a send-only capability. Rights are only ever interpreted through a type's
own syscalls, so the same bits mean different things per type -- which is fine, as long
as it is a decision and not an accident.

## The minimal set, and what it deliberately omits

The endpoint is intentionally the smallest thing that is a real IPC primitive. Three
omissions are deliberate, not unfinished:

- **No priority inheritance.** A thread parked on an endpoint has no `blocked_on`
  mutex, so the PI chain walk (Chapter 2.3) correctly stops at it -- a high-priority
  sender does *not* lend its urgency to a low-priority receiver. There is no owner to
  boost; an endpoint is a rendezvous, not a lock. The mitigation is configuration: a
  server that must be responsive should out-rank its clients, so it is scheduled
  promptly when a client sends.
- **No call/reply.** The primitive is one-way send and one-way receive. The
  request-then-await-response (RPC) pattern is a userspace convention built from a pair
  of endpoints, not a kernel mechanism.
- **No timed send/receive.** A blocked party waits indefinitely. Timed waits are the
  one planned substrate extension (Chapter 2.2) and would apply to sem, mutex, and
  endpoint uniformly; they are not special-cased here.

There are also three honest lifecycle asymmetries in the minimal set, each safe in the
intended client/server topology but worth naming:

- **No sender-side broken pipe.** If the last *send*-capable capability disappears
  while a receiver is parked, that receiver waits forever. It cannot happen in the
  intended topology (a server keeps receive rights and hands clients send rights), and
  a symmetric `send_holders` count would fix it, but the minimal set does not carry it.
- **Self-send.** The sole holder of the receive right calling `send` parks forever --
  its own receive capability keeps `recv_holders >= 1`, so the dead-endpoint gate
  passes and no broken-pipe wake ever fires. Unlike the mutex's self-deadlock there is
  no cheap detection, because `recv_holders` counts capabilities, not threads.
- **`-1` is overloaded.** Bad capability, bad buffer, dead endpoint, and broken pipe
  all return `-1` -- there is no errno. Adequate for the primitive; worth knowing before
  building retry logic that tries to distinguish them.

## The porting contract: a cross-domain privileged write

One property of the rendezvous reaches down into the arch layer and must be stated as a
porting requirement. When the arriving thread copies into a *parked* peer's buffer, it
writes memory belonging to a **different protection domain** -- the peer's -- while the
*running* thread's MPU regions are the ones loaded, not the peer's. This is the first
and only place the kernel writes user memory outside the current domain (a syscall
normally touches only the caller's own memory).

It works because every MPU backend KickOS targets grants the privileged kernel
background access to all of RAM regardless of which thread's user regions are loaded
(ARM `PRIVDEFENA`, the K64F SYSMPU supervisor background region, RISC-V's M-mode
exemption for unlocked PMP entries, and so on). So the copy, running privileged under
the lock, reaches the parked peer's buffer.

The requirement this creates: **a future memory-protection backend that does not grant
the kernel privileged background access to a parked peer's memory must arrange
equivalent access before this pattern is sound.** A cross-domain rendezvous test, run
under enforcement, is the guard that a new backend has not silently broken it. (See
`../reference/porting.md` and Chapter 7.3, *Peripheral isolation & the hardware
ceiling*, for where protection backends differ.)

## Where to go next

- The park/wake substrate an endpoint blocks on, and the confirm-resume barrier its
  return value depends on: Chapter 2.2, *The blocking substrate*.
- The capability that names an endpoint, and the send/receive rights split:
  Chapter 8.1, *Naming a kernel object*.
- The additive recipe the endpoint follows, and the close-protocol hook its broken-pipe
  wake rides on: Chapter 8.2, *Adding a kernel object type*.
- The service-publication story an endpoint is one half of (endpoint control + shared
  memory data, naming/discovery, badged endpoints for authenticating callers):
  `../reference/architecture.md` ("Object model, capabilities & IPC", "Service
  publication").
