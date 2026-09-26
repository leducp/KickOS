<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# One kernel lock, many ready queues

> How a kernel decides which thread runs on which core, the different ways to
> organise runnable threads, and why KickOS combines a ready queue per core
> with one lock around shared kernel transactions. Builds on Chapter 2.2
> (park and wake) and Chapter 3.5 (the saved switch frame). The exact rules
> live in [the architecture reference](../reference/architecture.md) and
> [the scheduler invariants](../reference/invariants.md).

## What the scheduler is choosing

A CPU core executes one thread at a time. An operating system lets more
threads exist than there are cores, so the kernel's scheduler must decide
which runnable thread each core executes next. A thread waiting for an
endpoint message or a timer is **blocked**: it has nothing to run yet. When
its wait completes it becomes **ready**, and it needs a place from which a
scheduler can find it. That place is a *ready queue* (often called a run
queue). A context switch then saves the outgoing thread and resumes the one
chosen from that queue.

On one core, this is simple. A wake adds a thread to the ready set; in a
priority scheduler like KickOS, the next decision picks the highest-priority
eligible thread. With two cores, a new question appears: do both choose from one shared ready
queue, or does each choose from its own? Both are workable designs. The choice
affects placement, balancing and the amount of shared scheduler state; it does
not by itself decide how the rest of the kernel is locked.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (threads,
scheduling and multiprocessors).*

## One ready queue for the whole kernel

Imagine two cores, A and B, and one newly ready thread. Put that thread on a
global queue. Whichever core next needs work takes the highest-priority thread
it is allowed to run, removing it before the other core can choose the same
one. An idle core sees work immediately; no waker has to decide which core
should hold the thread. There is one visible ordering of runnable work.

That simplicity is real. A small kernel can keep one lock over the queue and
make a wake and a pick straightforward. The costs are real too. Every core
reads and changes the same structure, so the queue and its lock become shared
traffic as the core count rises. Affinity makes the apparent single pop more
complicated: the highest-priority thread may be forbidden on A but runnable
on B, so A must skip it or use an index that can find eligible work. A global
queue also says little about where a thread's data was last used; a thread may
move between cores on successive picks unless placement adds another rule.

The global queue is therefore a good simplicity baseline, not a mistake to
be avoided. For few cores and light scheduler traffic, its short path may be
the best answer.

## A ready queue for each core

Now give A and B a ready queue each. A wake must choose a *home* for its
thread. A picks from A's queue, and B picks from B's. Each queue can have one
writer core and a local selection path. A thread that belongs on B is sent
there rather than inserted into B's list by A.

The gain is ownership: it is clear which core may change each link, and a
core's scheduling decision reads its own ready structure. The cost is
placement. A thread may wait on busy A while B has room unless the scheduler
checks load and moves work. Affinity and priority still matter when choosing a
home. Moving a thread requires a handoff protocol, and changing its priority
while it sits on another core needs a request to that core. Multiple queues
trade the global queue's automatic visibility for local ownership and more
bookkeeping.

Neither design makes the scheduler inherently faster in every workload. A
global queue can be cheaper on a small machine; per-core queues can avoid
shared-list traffic at width but spend work on placement and handoff. The
answer belongs to the workload and to the kernel's ownership goals, not to
the queue count alone.

## The separate question of kernel exclusion

Two cores may enter the kernel at once. Masking interrupts on A prevents a
handler on A from interrupting its update, but it does nothing to B. If both
cores touch a capability table, endpoint, timer list or thread state, the
kernel needs a rule that keeps their updates ordered.

A **big kernel lock** (BKL) is the simplest rule: one core at a time changes
shared kernel state. A CALL can resolve a cap, take a waiting receiver, copy
the request, mint a reply cap, park the caller and wake the server in one
ordered span. Timeout, close and death cannot free or reassign those objects
halfway through. A resolved pointer remains valid until the span ends.

The price is that unrelated kernel transactions also take turns. A finer-lock
kernel could let them run together, but each transaction then needs a complete
lock order and object-lifetime rule. For CALL, an endpoint lock alone is not
enough: the operation also touches two threads, a receiver's cap table,
deadline state, priority donation and scheduler placement. Timeout and close
reach those objects from different directions. A lock scheme that covers only
the common fast arm leaves the other arms racing with it.

The ready-queue layout and the kernel lock are **two design axes**:

| Ready-queue layout | Shared-state rule | Main trade |
| --- | --- | --- |
| One global queue | One BKL | Fewest ownership rules; shared queue and serialized transactions |
| One global queue | Finer locks | Object work may overlap, but every scheduler still coordinates on the shared queue |
| Per-core queues | One BKL | Explicit local scheduler ownership and handoff; transactions still serialized |
| Per-core queues | Finer locks | Independent transactions may overlap; lock order, lifetime and switch handoff become a larger protocol |

A blocked thread's *object wait queue* is another thing again. It records
who waits for an endpoint or semaphore; it is not a queue of runnable work.
Likewise, the ticket or CLH queue of cores waiting for the BKL is not a ready
queue of threads. The word "queue" does not make these structures
interchangeable.

## Why KickOS uses per-core queues with one BKL

KickOS values one understandable lifetime rule across small MCUs and larger
application processors. The BKL gives CALL, timeout, cancellation, close and
teardown one exclusion domain. Its wait is fair, and the interrupt-masked
waiter services doorbells so a peer can still receive a cross-core request.
This is a deliberate simplicity and predictability choice. It also means
independent IPC transactions do not run inside the kernel at the same time;
the lock's fairness does not turn serialized work into parallel work.

KickOS nevertheless gives each core its own ready queue. That is an ownership
choice, not a claim that adding queues breaks the BKL. Only a queue's core may
link, remove or re-seat its ready threads. A wake or placement on A that
chooses B stages a `HANDOFF` request; B links the thread during its scheduler
dispatch. A changed priority or affinity is conveyed as a `RESEAT` request to
re-read the thread's current fields, rather than a peer writing B's list.
When A's runnable level falls, it can ask a holder to push suitable work
instead of pulling a link from the holder's queue.

The extra placement and ring bookkeeping earns a clear single-writer rule
for scheduler structures, including under the BKL. What it does not earn is
a second exclusion protocol for IPC. Keeping the BKL avoids making cap
reclamation and a parked thread's saved frame depend on a separate local
path. The detailed [BKL decision](../design-m9.5-bkl-options.md) records the
workload evidence and the alternatives. The combination keeps a single
lifetime rule while making scheduler ownership explicit.

## How the boundary appears in the code

Each core's ready structure has priority-indexed FIFO lists and a bitmap.
The thread's affinity says where it *may* run; placement chooses the core
that *holds* it. A thread in `HANDED` state is on no ready queue while its
ring entry travels to that core. The producer publishes staged entries as its
BKL span ends, or after the outgoing context is saved when that span booked a
switch. The target consumes them in its scheduler dispatch. This is how a
local-writer rule survives a cross-core wake without a peer editing the list.

`IrqLock` masks the local core and, above one kernel core, takes the BKL.
The architecture supplies the atomic lock operation. A ticket backend gives
each contender a number and serves them in order. The x86 backend uses a CLH
queue, where a waiter polls its predecessor's cache line. These algorithms
change how cores *wait* for the same BKL; they do not change the kernel's
exclusion or lifetime rule. The code paths and exact publication invariants
are in [the reference](../reference/invariants.md),
`kernel/sync/klock.cc` and `kernel/sched/sched.cc`.
