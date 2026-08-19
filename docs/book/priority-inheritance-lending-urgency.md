<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Priority inheritance: lending a thread its blocker's urgency

> Why KickOS has a mutex at all when it already has a semaphore, and what the mutex
> does that userspace cannot. This chapter teaches priority inversion and the
> inheritance protocol that answers it. It builds on Chapter 2.2 (the blocking
> substrate the mutex parks on) and Chapter 8.1 (the capability handle a mutex is
> named by); it points into `../reference/architecture.md` ("Synchronization surface")
> for the exact object contract and `kernel/sched/sched.cc` for the scheduler entry
> point. Chapter 2 covers the scheduler and priorities this rests on.

## The problem: priority inversion

Give three threads priorities high, medium, low. The low thread takes a lock. The
high thread wakes, wants the same lock, and blocks -- correctly, the lock is held. So
far so good: high waits for low, briefly.

Now the medium thread wakes. It does not want the lock, so nothing stops it, and being
higher priority than low it *preempts* low. Low cannot run, so low cannot release the
lock, so high cannot proceed. A medium-priority thread that has nothing to do with the
lock is now indirectly blocking the highest-priority thread in the system, for as long
as it cares to run. This is **priority inversion**, and it is unbounded: any number of
medium threads can pile on, each delaying high indefinitely. It is the bug that
famously reset the Mars Pathfinder lander repeatedly until the fix was uploaded.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (scheduling and priority
inversion).*

## Why this cannot be fixed in userspace

The cure is to make the low thread run at high's priority for as long as it holds the
lock high is waiting on -- to *lend* low the urgency of the thread it is blocking, so
medium can no longer preempt it. That is the whole idea, and it is why the mutex must
be a kernel object.

Writing another thread's effective priority, and re-seating it in the scheduler's ready
structure so the change actually takes effect, is scheduler state behind the syscall
boundary. Userspace cannot reach it. And this is the *only* thing a mutex offers that a
semaphore does not: a plain lock, with no priority handling, is exactly a binary
semaphore initialized to one, which KickOS already has (Chapter 2.2). So the design
rule is sharp:

> A mutex is a kernel object **if and only if** it does priority inheritance. A
> mutex without PI earns nothing over a binary semaphore and must never be a distinct
> kernel object.

That is also why the kernel's synchronization surface stops at two objects -- the
counting semaphore (the general wait/wake primitive) and the PI mutex -- and nothing
else. Condition variables, read/write locks, barriers, mailboxes: all are userspace
libraries built over those two. The admission test for any *new* kernel sync object
is a single question: does it require a scheduler action userspace cannot safely
perform? For a mutex the answer is yes, exactly once, and the answer is priority
inheritance. (The exact surface is in `../reference/architecture.md`,
"Synchronization surface".)

## The options for bounding inversion

**Do nothing.** Accept unbounded inversion. Not an option for a real-time kernel.

**Priority ceiling protocol (PCP, or immediate-ceiling).** Each mutex is declared at
creation with a *ceiling* -- the highest priority of any thread that will ever take it
-- and a locker is raised to that ceiling immediately, on every lock. This bounds
inversion tightly and can prevent certain deadlocks, but it has two costs. First, the
ceiling is a user-supplied number that becomes scheduler policy: declare it wrong and
you silently distort scheduling for every locker, with nothing to check it against.
Second, it boosts on *every* lock, including uncontended ones -- taxing the fast path
that dominates real workloads, to pay for contention that usually is not there.

**Plain priority inheritance (PI).** Do nothing on an uncontended lock. Only when a
thread actually blocks on a held mutex, boost the holder to the blocker's priority;
restore it when the mutex is released. No configuration, no per-mutex ceiling, and the
fast path pays nothing.

## KickOS's choice: plain PI

KickOS uses plain priority inheritance. On a small system -- a handful of mutexes, a
few dozen threads -- PCP's extra deadlock-avoidance does not buy back its API surface
and its fast-path tax, and a misdeclared ceiling is an unverifiable user input turned
into scheduler behavior. Plain PI costs nothing until contention happens, needs zero
declaration, and its one non-trivial cost (walking a chain of blocked owners) is
bounded by the pool sizes. Deadlock, which PCP prevents structurally, KickOS instead
*detects* and refuses at lock time (below).

## The invariants that make it correct

Four invariants pin the mechanism. State them first; the code is just their
maintenance.

- **I1 -- effective vs base priority.** Each thread has `base_prio` (its assigned
  anchor) and `prio` (its *effective* priority). The scheduler, the policy, and the
  wait-queue scan read **only** `prio`. Inheritance raises `prio`; it never touches
  `base_prio`.
- **I2 -- the inheritance value.** At every scheduling point, a thread's effective
  priority equals the max of its `base_prio` and of every *donor* currently lending to
  it. A donor is any thread parked on a mutex this thread holds, any caller parked
  waiting for this thread's reply, and any caller parked in a send-wait on an endpoint
  this thread serves. Mutex waiters are the donor this chapter walks; the other two are
  call/reply donation (Chapter 8.5), and one recompute funnel takes the max over all
  three. Boosting and reverting are both just re-establishing I2.
- **I3 -- ready-list integrity.** The ready structure files a thread by its `prio`
  (Chapter 2), and it holds the `RUNNING` thread as well as the `READY` ones. So no
  code may change the `prio` of a thread in either of those states in place: it must be
  removed from the ready structure, have its priority changed, and be re-added, or the
  per-priority lists and the priority bitmap corrupt silently. `RUNNING` is not the
  exotic case here: on a boost the target is very often the thread that is running.
  There is **one writer** of effective priority, and it obeys this.
- **I4 -- atomicity.** Every boost, revert, and chain walk runs entirely inside the
  same one `IrqLock` critical section as the block or transfer it accompanies. On a
  single core that makes the whole priority manipulation indivisible.

### The one writer: `sched::set_prio`

I1 and I3 are enforced by funneling every effective-priority change through a single
scheduler entry point:

```
void set_prio(Thread* t, uint8_t p)
{
    IrqLock lock;
    if (t->prio == p)
    {
        return;
    }
    if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
    {
        kernel().policy->on_remove(t);   // I3: pull it out first
        t->prio = p;
        kernel().policy->on_ready(t);    // ...then re-file at the new priority
        return;
    }
    t->prio = p;   // BLOCKED only: on no ready list, and on no prio-ordered queue
}
```

Only a `BLOCKED` thread takes the new value in place, and it may precisely because
neither structure it can be on is priority-ordered: wait queues are unsorted and scanned
at pop (Chapter 2.2), and the timer list is sorted by deadline. A parked thread whose
priority rises needs no re-queue. This is the lazy-scan payoff cashing in. Everything
else goes through remove-change-re-add, and reading that branch as "READY only" is the
mistake that corrupts the bitmap.

## Boosting: the lock path

On an uncontended lock the fast path adds exactly two stores: record the owner, and
push the mutex onto the owner's *held list* (an intrusive chain, via a `next_held`
link, of every mutex a thread currently owns -- the data I2's "any mutex this thread
holds" is computed over). No priority work at all.

On a *contended* lock -- the owner is someone else -- the caller must lend its priority
down the chain of blocked owners before it parks. The chain is: the mutex's owner;
if that owner is itself blocked on another mutex, that mutex's owner; and so on, each
hop followed through the parked thread's **wait edge**: a per-thread (kind, object) pair
naming what it is waiting for and which object owns the list it is on. The walk reads that
edge through a narrowing accessor, `wait_mutex()`, which answers the mutex for a mutex park
and null for every other kind. That is what makes the accessor, and not a raw pointer, the
right shape: a thread parked on something that is not a mutex has no owner to lend to, and
the walk must stop there rather than reinterpret whatever object the edge names.

Two things can go wrong on that walk, and they force it to be **two passes**:

```
c = current
if m->owner == c:  return -KOS_EDEADLK   // locking a mutex you hold: self-deadlock

// PASS 1 -- detect a cycle, write nothing:
t = m->owner
while t != nullptr:
    if t == c:  return -KOS_EDEADLK   // the chain loops back to us: it would deadlock
    if t->wait_mutex() == nullptr:  break
    t = t->wait_mutex()->owner

// PASS 2 -- boost, now that the chain is known acyclic:
t = m->owner
while t != nullptr:
    if t->prio >= c->prio:  break            // chain already at/above our urgency
    sched::set_prio(t, c->prio)
    if t->wait_mutex() == nullptr:  break
    t = t->wait_mutex()->owner

wq_block(m->waiters, WAIT_MUTEX, m)   // parks AND seats the wait edge
return current's wait_result           // what the waker wrote: the grant, or a refusal
```

Returning from `wq_block` is not itself the acquire. The waker writes the outcome into
`wait_result` (below), and a cancel ends the same park *without* transferring ownership:
that unwind unlinks the waiter and recomputes the owner's inherited priority without it,
and the lock call reports the failure. Only a status that says the mutex was granted
means the mutex is held.

A cancel is the *only* way out of this park that is not a grant, and that is structural
rather than a gap in the interface. Locking a mutex takes no deadline argument, so no
timer is ever armed against a mutex wait; correspondingly, the timer expiry dispatch
carries an arm for each kind of wait a deadline *can* be attached to and none for this
one, and would treat a mutex waiter arriving there as a kernel invariant violation
rather than something to unwind. The absence is enforced from both ends, not merely
unexposed at the syscall.

The contrast with the endpoint parks of Chapter 8.3 is the thing to take away. A message
may never come at all, so waiting for one has to be bounded by something outside the
wait. A *contended* mutex always has a live owner, and the boost above is the mechanism
pushing that owner through its critical section; the wait is bounded by the owner's
remaining section rather than by the clock. A deadline would hand the caller a way to
abandon a wait it is simultaneously lending its urgency to, which is close to the
opposite of what the boost exists to do.

The park and the edge are seated by the same call, and the waker clears both when it hands
the mutex over. Neither may be left to the parked thread to do after it resumes: on an
architecture that defers the switch, a self-clear leaves a window in which a still-parked
thread already answers null from `wait_mutex()`, and a walk crossing that window stops
short and loses a boost.

Why two passes and not one: a single fused walk might boost the first few owners in a
chain and *then* discover a cycle -- leaving those boosts applied for a lock that will
never be granted, violating I2 (the boosts are only justified if the caller actually
parks). Pass 1 reads only and proves the chain acyclic; pass 2 boosts. Both run under
the same lock (I4), so nothing changes between them.

Three subtleties are worth internalizing:

- **The walk terminates.** A chain longer than the number of mutexes must revisit a
  node, which is a cycle, which pass 1 catches. A defensive depth bound backs this up
  but cannot legitimately trigger.
- **The early stop at `prio >= c->prio` is sound.** Inheritance only ever raises
  toward a waiter's priority; once a node in the chain is already at or above the
  caller's priority, every node past it was boosted to at least that level when its own
  waiter blocked (I2, inductively). No lower node remains.
- **PI stops at semaphores.** A thread parked on a *semaphore* carries a semaphore-kind
  wait edge, so `wait_mutex()` answers null and the walk stops there. A semaphore has no
  owner to boost, so inheritance genuinely cannot propagate through it. This is the classic
  PI boundary -- document it, do not try to "fix" it. The same is true of an IPC park: it
  names an endpoint or a server, not an owner, and call/reply donation is a separate
  mechanism reaching the same `set_prio` funnel rather than an extension of this walk.

## Reverting: the unlock path

Unlock is where I2 is re-established for both the releaser and, if there is a waiter,
the new owner:

```
held_list_remove(current, m)               // m no longer contributes to current's prio
w = wq_pop_highest(m->waiters)             // Chapter 2.2, step 1
if w == nullptr:
    m->owner = nullptr
else:
    m->owner = w                           // step 2: transfer ownership
    w->wait_result = 0                      // step 2: normal grant (see below)
    held_list_push(w, m)
    // the new owner inherits the REMAINING waiters at transfer time, since no
    // future lock() call will ever boost w for waiters that parked before it:
    set to max(w->prio, highest prio still parked on m->waiters)

recompute current's prio over its remaining held list   // re-establish I2 for the releaser
if current's prio dropped:
    reschedule()                            // a middle-prio READY thread may now win
if w != nullptr:
    sched::wake(w)                          // step 3
```

The releaser's priority is not "restored to base" -- it is **recomputed over its
remaining held list** (`recompute` walks each still-held mutex's waiters and takes the
max with `base_prio`). A thread holding two contended mutexes that releases one must
*stay* boosted for the other; only releasing the last contended mutex drops it to base.
Restore-to-base would be a real bug here. And lowering a priority can make a
previously-shadowed thread the highest ready one, so a self-lowering must be followed
by a reschedule.

The new owner inheriting the *remaining* waiters at transfer time is the step to think
carefully about, because it is where the two halves of the design meet. Those waiters
parked before this thread owned the mutex, so no future lock call will ever boost the
new owner on their behalf: transfer is the only moment left to establish I2 for them.
And yet, as written, the boost computes to nothing. The pop hands over the *highest*
priority waiter, so every waiter still on the queue is at or below the new owner
already, and the max is the new owner's own priority. It is I2 stated where it belongs
rather than an operation that changes a value, and it stops being vacuous the moment the
pop stops being a priority pop. Write it, and know that under a priority pop it computes
no change.

## Owner-died: the mutex's status value

The mutex is the first real user of the `wait_result` status channel (Chapter 2.2). Its
values are drawn from the one fleet-wide error taxonomy every syscall returns from
(Chapter 3.9): `0` for a normal grant, and otherwise a negated `KOS_E*` code. Some of
those end the park without the mutex, `-KOS_EDEADLK` for the refusals the walk above
found, `-KOS_ECANCELED` for a waiter cancelled while parked. And one does not.

That one arises at thread exit. If a thread exits while owning a mutex, its teardown
(Chapter 8.2, the exit path that closes a thread's capabilities) **force-unlocks** each
owned mutex: pop the highest waiter, transfer ownership to it, set its `wait_result` to
`-KOS_EOWNERDEAD`, boost it from the remaining waiters, and wake it. The woken thread's
lock call returns that code, telling it the data the mutex protected may be
inconsistent, because the previous holder died mid-critical-section. This is the POSIX
`EOWNERDEAD` idea reduced to one return value, with no robust-list machinery. What the
woken thread does about it is its own policy; the kernel's job is only to never strand
the waiter and to tell it the truth.

Read that value carefully, because it is the one place in the whole API where a
negative return means the operation **succeeded**. Everywhere else, negative means the
call did nothing and the caller owns nothing. Here the mutex *is* held, so the reflex
test

```
if (rc < 0) { return rc; }   // WRONG for a mutex lock
```

walks away from a mutex it is holding and strands every waiter behind it, permanently.
The correct shape tests that code by name:

```
int const rc = kos_mutex_lock(m);
if (rc == -KOS_EOWNERDEAD)
{
    // HELD. Repair or discard whatever the mutex protected, then carry on and unlock.
}
else if (rc < 0)
{
    return rc;   // genuinely not held
}
```

The alternative would be a positive success-variant, which is exactly the per-object
value namespace the substrate refuses to have (Chapter 2.2): every wake reports through
one taxonomy so that no waker has to know which object woke a thread. The price of that
uniformity is this single special case, and it is written down at the top of the error
header rather than left for a caller to discover.

The mirror rule is that closing the capability to a mutex *you currently own* is
refused: the capability is the only unlock authority, so letting an owner drop it
voluntarily would strand any waiter. An owner that wants out unlocks; an owner that
*dies* is handled by the force-unlock above. The refcounted lifecycle this rides on --
and why a parked waiter can never cause the object to be freed underneath it -- is the
leak-don't-strand discipline in Chapter 8.2.

## Where to go next

- The park/wake primitive the mutex blocks on, and the three-step wake protocol whose
  "step 2" is the ownership transfer above: Chapter 2.2, *The blocking substrate*.
- The capability handle a mutex is named by, and rights (possession is the lock/unlock
  authority -- there is no meaningful WAIT/SIGNAL split): Chapter 8.1, *Naming a
  kernel object*.
- The refcount and force-unlock-on-exit lifecycle, and how a new object type is added:
  Chapter 8.2, *Adding a kernel object type*.
- The exact object contract and the synchronization-surface rule:
  `../reference/architecture.md` ("Synchronization surface", "Object model").
