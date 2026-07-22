<!-- SPDX-License-Identifier: CECILL-C -->
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
- **I2 -- the inheritance value.** At every scheduling point, a lock owner's effective
  priority equals `max(base_prio, the highest priority among all threads currently
  parked on any mutex this thread holds)`. Boosting and reverting are both just
  re-establishing I2.
- **I3 -- ready-list integrity.** The ready structure files a thread by its `prio`
  (Chapter 2). So no code may change a `READY` thread's `prio` in place: it must be
  removed from the ready structure, have its priority changed, and be re-added -- or
  the per-priority lists and the priority bitmap corrupt silently. There is **one
  writer** of effective priority, and it obeys this.
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
    if (t->state == ThreadState::READY)
    {
        kernel().policy->on_remove(t);   // I3: pull it out first
        t->prio = p;
        kernel().policy->on_ready(t);    // ...then re-file at the new priority
    }
    else
    {
        t->prio = p;   // BLOCKED/SLEEPING/RUNNING just take the value
    }
}
```

A `BLOCKED` thread can take the new value in place precisely because wait queues are
unsorted and scanned at pop (Chapter 2.2) -- a parked thread whose priority rises needs
no re-queue. This is the lazy-scan payoff cashing in.

## Boosting: the lock path

On an uncontended lock the fast path adds exactly two stores: record the owner, and
push the mutex onto the owner's *held list* (an intrusive chain, via a `next_held`
link, of every mutex a thread currently owns -- the data I2's "any mutex this thread
holds" is computed over). No priority work at all.

On a *contended* lock -- the owner is someone else -- the caller must lend its priority
down the chain of blocked owners before it parks. The chain is: the mutex's owner;
if that owner is itself blocked on another mutex, that mutex's owner; and so on, each
hop followed through a per-thread `blocked_on` pointer (the mutex a thread is currently
parked on, or null).

Two things can go wrong on that walk, and they force it to be **two passes**:

```
c = current
if m->owner == c:  return -2      // locking a mutex you already hold: self-deadlock

// PASS 1 -- detect a cycle, write nothing:
t = m->owner
while t != nullptr:
    if t == c:  return -2         // the chain loops back to us: locking would deadlock
    if t->blocked_on == nullptr:  break
    t = t->blocked_on->owner

// PASS 2 -- boost, now that the chain is known acyclic:
t = m->owner
while t != nullptr:
    if t->prio >= c->prio:  break            // chain already at/above our urgency
    sched::set_prio(t, c->prio)
    if t->blocked_on == nullptr:  break
    t = t->blocked_on->owner

c->blocked_on = m
wq_block(m->waiters)              // park; we own the mutex when we return
c->blocked_on = nullptr
```

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
- **PI stops at semaphores.** `blocked_on` is set only on the mutex path; a thread
  parked on a *semaphore* has `blocked_on == nullptr`, so the walk stops there. A
  semaphore has no owner to boost, so inheritance genuinely cannot propagate through
  it. This is the classic PI boundary -- document it, do not try to "fix" it.

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
    // the new owner inherits the REMAINING waiters immediately -- they blocked
    // before w owned m, so no future lock() call will ever boost w for them:
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

The new owner inheriting the *remaining* waiters at transfer time (rather than waiting
for a future lock call) is the non-obvious step: those waiters parked before this
thread owned the mutex, so nothing else will ever boost the new owner on their behalf.
Transfer is the only moment to establish I2 for them.

## Owner-died: the mutex's status value

The mutex is the first real user of the `wait_result` status channel (Chapter 2.2).
Its value namespace is two values: `0` (locked normally) and `1` (locked, but the
previous owner died while holding it).

The second case arises at thread exit. If a thread exits while owning a mutex, its
teardown (Chapter 8.2, the exit path that closes a thread's capabilities)
**force-unlocks** each owned mutex: pop the highest waiter, transfer ownership to it,
set its `wait_result = 1`, boost it from the remaining waiters, and wake it. The woken
thread's lock call returns `1` -- telling it the data the mutex protected may be
inconsistent, because the previous holder died mid-critical-section. This is the POSIX
`EOWNERDEAD` idea reduced to one return value, with no robust-list machinery. What the
woken thread does about it is its own policy; the kernel's job is only to never strand
the waiter and to tell it the truth.

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
