<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Priority inheritance: lending a thread its blocker's urgency

> Why KickOS has a mutex at all when it already has a semaphore, which part of it only the
> kernel can do, and which part user code does alone. This chapter teaches priority
> inversion, the inheritance protocol that answers it, and the lock word that keeps a free
> mutex out of the kernel. It builds on [Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)
> (the blocking substrate a waiting thread parks on) and [Chapter 2.4](an-atomic-buys-definedness-not-atomicity.md)
> (the compare-and-swap that takes the word). [Chapter 2](kernel-model.md) covers the
> scheduler and the priorities this rests on, and [Journey 4, A mutex](../journey/04-mutex.md)
> walks the code. The exact contract is in the Reference:
> [Architecture](../reference/architecture.md) ("Synchronization surface") and
> [Invariants](../reference/invariants.md).

## The problem: priority inversion

Give three threads priorities high, medium and low. The low thread takes a lock. The high
thread wakes, wants the same lock, and blocks. That is correct, since the lock is held, and
so far it costs little: high waits for low, briefly.

Now the medium thread wakes. It does not want the lock, so nothing stops it, and since its
priority is higher than low's, it *preempts* low. Low cannot run, so low cannot release the
lock, so high cannot go on. A medium thread that has nothing to do with the lock now blocks
the highest-priority thread in the system, for as long as it cares to run. This is
**priority inversion**, and it is unbounded: any number of medium threads can pile on, each
delaying high again. It is the bug that reset the Mars Pathfinder lander again and again,
until a fix was uploaded.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (scheduling and priority
inversion).*

## Why this cannot be fixed in user code

The cure is to make the low thread run at high's priority for as long as it holds the lock
that high waits on. Low borrows the urgency of the thread it blocks, so medium can no longer
preempt it. That is the whole idea, and it is called **priority inheritance** (PI).

Changing another thread's priority, and moving it in the scheduler's ready queues so that the
change takes effect, is scheduler state behind the syscall boundary. User code cannot reach
it. And this is the *only* thing a mutex offers that a semaphore does not. A plain lock, with
no priority handling, is a binary semaphore that starts at one, and KickOS already has the
semaphore ([Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)).

Inheritance has work to do only when a thread waits. While a mutex is free, or held with
nobody waiting, there is no priority to lend. So the kernel's part in a mutex is exactly the
waiting part:

**The kernel takes part in a mutex only when a thread must wait for it.**

The test for any synchronization mechanism the kernel might offer is one question: does it
need a scheduler action that user code cannot safely perform? The semaphore passes it only
when a thread must wait for a unit, or when a post must wake a thread that waits. A free unit
is one compare-and-swap in user mode, and the kernel's part is the plain wait and wake on the
semaphore's word ([Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)). The mutex
passes it the same way, only on contention, and adds the priority it lends. Condition
variables, read-write locks, barriers and mailboxes are user libraries built over those two.

## The options for bounding inversion

**Do nothing.** Accept unbounded inversion. That is not an option for a real-time kernel.

**Priority ceiling protocol (PCP), also called immediate ceiling.** Each mutex is declared
with a *ceiling*, the highest priority of any thread that will ever take it, and a thread is
raised to that ceiling at once, on every lock. This bounds inversion tightly and prevents
some deadlocks, but it has two costs. First, the ceiling is a number the user supplies, and
it becomes scheduler policy: declare it wrong and every locker is scheduled wrong, with
nothing to check it against. Second, it raises the priority on *every* lock, free ones
included. So it taxes the common case to pay for contention that is usually not there.

**Plain priority inheritance.** Do nothing on a free lock. Only when a thread blocks on a
held mutex, raise the holder to the blocked thread's priority, and lower it again when the
mutex is released. There is nothing to configure, no ceiling per mutex, and a free lock pays
nothing.

## KickOS's choice: plain PI

KickOS uses plain priority inheritance. On a small system, with a handful of mutexes and a
few dozen threads, the deadlocks PCP prevents do not pay for its extra interface and its cost
on every lock, and a wrong ceiling is a user input that nothing can check, turned into
scheduler behavior. Plain PI costs nothing until there is contention and needs no
declaration. Its one real cost, walking a chain of blocked holders, is bounded by the number
of threads. Deadlock, which PCP prevents by construction, KickOS instead *detects* and
refuses at lock time (below).

## Where the lock lives

A mutex has state: who holds it, and who waits for it. There are two places to keep it.

**In the kernel.** Each mutex is a kernel object, created by a syscall and named by a
handle. Then every lock and every unlock is a syscall, even when no other thread wants the
lock. A syscall costs a trap, the saving and restoring of the caller's registers, a check of
the handle, and the return ([Chapter 3.9](the-syscall-path-trap-dispatch-return.md)). That
cost buys nothing when the lock is free, and a lock is free on almost every take. For
instance, the C library takes a lock on every `printf` and every `malloc`, and in KickOS's
own workloads another thread almost never holds that lock at that moment.

**In the threads' own memory.** The mutex is one word in memory that every thread sharing
it can write. A thread takes a free mutex by writing its own id into the word with one
**compare-and-swap**: an instruction, or a short sequence, that stores a new value only if
the word still holds the expected one, as one indivisible step
([Chapter 2.4](an-atomic-buys-definedness-not-atomicity.md)). It releases the mutex the same
way. The kernel is entered only when the word is held and the caller must wait, or when the
holder releases a word that someone waits on. This is the idea of the *futex*, the "fast
user-space mutex" of Linux.

KickOS keeps its mutex in the threads' memory. The kernel holds no object, no handle and no
budget for it. A static `kos_mutex_t m = KOS_MUTEX_INIT;` is a ready mutex, and a program may
have as many as it likes. The price is paid in the kernel's contended path, and most of the
rest of this chapter is about that path, because the kernel must now act on a word that user
code writes.

*Further reading: Hubertus Franke, Rusty Russell and Matthew Kirkwood, "Fuss, Futexes and
Furwocks: Fast Userlevel Locking in Linux" (Ottawa Linux Symposium, 2002), and Ulrich Drepper,
"Futexes Are Tricky".*

## The word and the thread's own id

The word has two parts:

- The low 31 bits name the holder by its **lock id**, and 0 means free.
- The top bit, the **waiters bit**, says that a thread is or was parked on the word, so its
  release must enter the kernel.

A lock id is the holder's slot in the kernel's table of threads, plus one so that no id is 0,
with the slot's **generation** in the bits above it. The generation is a counter that moves on
each time the slot is used again, so a dead thread's id does not name the next thread in its
slot. [Chapter 8.7](stale-handles-generation-width-and-allocation-policy.md) teaches the same
idea for handles.

To write its own id, a thread must know it without asking the kernel. So the kernel **seats**
each thread's id in user memory, where the thread reads it with one load. For instance, on a
board with one core it is one word of the program's data that the kernel rewrites at every
switch, and on a board whose threads have a thread pointer register it is a word next to
where that register points. The kernel never reads this copy back: every kernel path knows the
running thread from its own state. A thread that overwrites its copy misleads only its own
locks.

The user side of the mutex is then short:

```
lock(m, timeout):
    held = compare_and_swap(&m->word, 0, self)    // answers what the word held
    if held == 0:              return 0            // it was free: taken, no kernel entry
    if owner(held) == self:    return -KOS_EDEADLK // locking a mutex you hold
    return syscall(MUTEX_LOCK, &m->word, timeout)  // another thread holds it: wait

unlock(m):
    held = compare_and_swap(&m->word, self, 0)
    if held == self:           return 0            // nobody waits: released, no kernel entry
    if owner(held) != self:    return -KOS_EPERM   // only the holder may unlock
    return syscall(MUTEX_UNLOCK, &m->word)         // the waiters bit is set: hand it over
```

A free lock and its unlock run no kernel code and do no priority work. The kernel's books
hold nothing about a mutex that nobody waits on. Only threads take a mutex: an interrupt
handler has no thread id to write, and no interrupt handler ever writes a lock word.

## The contended path: the word is hostile input

When the kernel is entered, it is given an address and nothing else: no handle to look up and
no right to check. The word at that address was written by user code, perhaps by a buggy or a
hostile thread. So the kernel treats the address, and the id in the word, as claims to check.

**May the caller name this word?** Before it reads anything, the kernel checks that the word
is aligned, that the calling thread could write it itself, and that it lies in ordinary
memory, not in device registers or in uncached memory. A word that fails is refused. So the
kernel only ever writes a word that its caller could have written.

**Which mutex is it?** The kernel names a word by its physical address: the address of the
memory itself, not the address the thread used. On a chip with a memory management unit
(MMU), two tasks that share a page may see it at two different addresses. Keyed on the
memory, they still meet on one mutex, and two tasks that share no memory can never meet on
one. On a chip without an MMU the two addresses are the same.

**The record.** The kernel keeps a record for a word only while some thread waits on it. A
record names the holder and holds the queue of parked waiters, and the kernel finds it by
the word's physical address. It is taken at the first park and freed when its last waiter
leaves. The records come from a fixed array with one record per thread slot. That is always
enough, because each waiter parks on one record and a record with no waiter is free. While a
record lives, the record, not the word, says who holds the mutex. A word that disagrees with
its record was broken by a writer, and the call is refused.

**Who holds it?** With no record, the holder is whoever the word names, and that id is only a
candidate. The kernel acts on it only when the slot holds a live thread that is not on its
way out, with exactly that id, so the generation matches, and that could have written this
word with its own memory access. Any other id names no live holder: a thread that died, or an
id that a writer forged. Either way the kernel takes the word for the caller (see
"Owner-died" below). So a forged word can make the kernel boost only a thread that could have
taken the lock itself. Where every task can write every app's static data, that is any thread
of any task, and a try of such a word also tells the forger whether the thread it names is
alive.

## The invariants that make it correct

Four invariants pin the priority mechanism. State them first, and the code is only their
upkeep.

- **I1, effective and base priority.** Each thread has a *base* priority, the one it was
  given, and an *effective* priority. The scheduler, the policy and every wait-queue scan read
  only the effective one. Inheritance raises the effective priority and never touches the
  base.
- **I2, the inheritance value.** At every scheduling point, a thread's effective priority is
  the highest of its base priority and of every *donor* lending to it. A donor is any thread
  parked on a mutex this thread holds, any caller parked waiting for this thread's reply, and
  any caller parked in a send on an endpoint this thread serves. This chapter walks the mutex
  donors. The other two are call/reply donation ([Chapter 8.5](synchronous-call-and-reply.md)),
  and one recompute takes the highest over all three. Boosting and reverting both only
  re-establish I2.
- **I3, ready-list integrity.** A core's ready structure files a thread by the priority that
  core last applied, and it holds the running thread as well as the ready ones. When a
  priority change is applied on the holder's core, the thread is removed, filed again under
  its new priority, and added back. Changing the priority in place would corrupt the lists.
  Another core instead updates the priority and asks the holder's core to apply it, so the
  priority may run ahead of the filing until that core does. No core edits another core's
  ready structure. There is **one function** that writes an effective priority, and it obeys
  this rule.
- **I4, atomicity.** Every boost, revert and chain walk runs inside the same kernel critical
  section as the park or the hand-over it goes with. On one core, the critical section masks
  this core's interrupts. Above one core, it also holds the one kernel lock, which keeps the
  other cores out.

### The one writer of effective priority

I1 and I3 hold because every change of an effective priority goes through that one function.
It has three cases. A blocked thread takes the new value in place: the mutex and endpoint wait
queues are not sorted, and each is scanned when a thread is taken off it
([Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)), while the timer list is
sorted by deadline, not by priority. A thread on this core's ready structure is removed,
filed again and added back, whether it is ready or running. A thread on another core gets a
request to apply the change. The request carries no priority value, so an older request
cannot overwrite a newer priority.

## Boosting: the lock path

Once the holder is known, the caller must lend its priority down the chain of blocked
holders before it parks. The chain starts at the word's holder. If that holder itself waits
on another mutex, the next link is that mutex's holder, and so on. The kernel follows each hop
through the parked
thread's **wait edge**: two fields on each thread naming what kind of thing it waits for and
which object. The walk asks the edge for a mutex record. A thread that waits on anything else,
such as a plain word, an endpoint or a sleep, answers none, and the walk stops there, since
such a wait has no holder to lend to.

Two things can go wrong on that walk, and they force it to be **two passes**:

```
c = current
check the word, read it, find its record            // the checks above
if a record r exists:
    if the word disagrees with r:  return -KOS_EINVAL
    if r->owner == c:              return -KOS_EDEADLK
    t = r->owner
else:
    if owner(word) == 0:           compare-and-swap it to c, return 0   // freed meanwhile
    if owner(word) == c:           return -KOS_EDEADLK
    t = the live holder owner(word) names, or nullptr
    if t == nullptr:               compare-and-swap it to c, return -KOS_EOWNERDEAD
if timeout == 0:                   return -KOS_ETIMEDOUT  // a try lends nothing, parks nothing

// PASS 1, detect a cycle and write nothing:
o = t
while o != nullptr:
    if o == c:  return -KOS_EDEADLK   // the chain loops back to us: it would deadlock
    o = the holder of the mutex o waits on, or nullptr

set the waiters bit in the word
take a record keyed on the word's memory, owned by t, on t's held list

// PASS 2, boost, now that the chain is known to have no cycle:
o = t
while o != nullptr:
    if o->prio >= c->prio:  break     // the chain is already at or above our urgency
    set_prio(o, c->prio)
    o = the holder of the mutex o waits on, or nullptr

arm the deadline, unless timeout is forever
park c on r's queue                   // parks AND seats the wait edge
return c's wait_result                // what the waker wrote: the grant, or a refusal
```

The **held list** is a chain, on each thread, of the records it holds. It holds only records,
so a mutex held with nobody waiting is not on it, and it lends nothing.

A compare-and-swap in this path can fail, because user code may write the word at any moment.
Then the kernel leaves its critical section and starts the attempt again from the top. A word
that changes forever makes only the threads that write it spin, and the kernel never holds its
critical section across that loop. Every refusal comes before the record is taken, and taking
the record leads only to the park. So a refused lock leaves nothing behind.

Returning from the park is not itself the acquire. The waker writes the outcome into the
thread's result field (below). A cancel and an expired deadline end the same park *without*
handing the mutex over, and answer `-KOS_ECANCELED` and `-KOS_ETIMEDOUT`. Only a result that
says the mutex was granted means the mutex is held.

The deadline is the caller's, as on every other blocking call. A thread that would rather do
something else than wait past some point says so. A try, a deadline of zero, asks only whether
the mutex is free: when it is held, the try answers at once, before either pass, so it lends
no priority and never parks. A waiting caller lends its urgency for as long as it waits, and
takes it back when it stops. That is all I2 asks: the boost is justified by a donor that is
parked, and it goes when the donor goes, by grant, cancel or expiry alike.

The park and the wait edge are seated by the same step, and the waker clears both when it
hands the mutex over. Neither may be left to the parked thread to do after it resumes. On an
architecture that defers the switch, a thread that cleared its own edge would leave a window in
which it is still parked but already looks as if it waits on nothing. A walk that crosses that
window stops short and loses a boost.

Why two passes and not one: a single walk might boost the first few holders in a chain and
*then* find a cycle. Those boosts would stay applied for a lock that will never be granted,
which breaks I2, since the boosts are justified only if the caller really parks. Pass 1 only
reads and proves the chain has no cycle, and pass 2 boosts. Both run in the same critical
section (I4), so nothing changes between them.

Three subtleties are worth keeping in mind:

- **The walk ends.** Each hop is a thread parked on one record, so a chain longer than the
  number of threads must visit some thread twice. That is a cycle, which pass 1 catches. A
  depth bound backs this up, and it cannot be reached legitimately.
- **The early stop at an equal or higher priority is sound.** Inheritance only ever raises a
  holder toward a waiter's priority. Once a holder in the chain is already at or above the
  caller's priority, every holder past it was raised to at least that level when its own
  waiter blocked (I2, by induction). No lower one is left.
- **PI stops at a plain word.** A thread parked on a semaphore waits on its plain word. Its
  wait edge names a plain record, not a mutex record, so the walk stops there. A plain word
  has no holder to boost, so inheritance cannot pass through it. This is the classic limit of
  PI, so state it and do not try to "fix" it. The plain records and the mutex records are the
  same structure, keyed the same way, but each record is one kind, and a word that has
  waiters of one kind refuses a wait of the other. The same is
  true of a thread parked on an endpoint: its edge names an endpoint, not a holder. Call/reply
  donation is a separate mechanism that reaches the same priority function, not an extension of
  this walk.

## Reverting: the unlock path

The holder enters the kernel at unlock only when the waiters bit is set. The kernel then
re-establishes I2 for the releaser and for the new holder:

```
check the word, read it, find its record r
if r == nullptr:                    // every waiter left, and the bit is stale
    if owner(word) != current:  return -KOS_EPERM
    compare-and-swap the word to 0
    return 0
if r->owner != current:         return -KOS_EPERM
w = the highest-priority waiter on r   // among equals, the one that waited longest
store w's id in the word, with the waiters bit if others still wait
held_list_remove(current, r)
pop w, then r->owner = w, w->wait_result = 0, held_list_push(w, r)   // the wake's step 2
if r has no waiter left:  free r
recompute current's prio over its remaining held list   // re-establish I2 for the releaser
wake(w)                             // the wake's step 3, which also lets the scheduler choose
```

The unlock does not free the word for the waiters to race for. It hands the word to one of
them and writes that thread's id into it. The new holder comes from the record, never from the
word, so a word that a writer changed cannot choose who holds it next.

The releaser's priority is not "restored to base". It is **recomputed over its remaining held
list**: the kernel takes the highest priority waiting on each record it still holds, and its
base priority. A thread that holds two contended mutexes and releases one must *stay* boosted
for the other, and only the release of its last contended mutex drops it to its base. A
restore to base would be a real bug here. And a lower priority can make a thread that was in
the shadow the highest ready one, so the wake lets the scheduler choose again.

The new holder needs no boost from the waiters still queued, and the reason is worth
checking. Those waiters parked before this thread held the mutex, so no later lock call will
boost the new holder for them, and the hand-over is the only moment left to establish I2.
But the pop hands the mutex to the *highest-priority* waiter, so every waiter still queued is
at or below the new holder already. Their boost changes nothing. That holds only because the
pop is a priority pop: a queue that handed over in any other order would have to boost the new
holder here.

## When a waiter goes away

A waiter whose deadline passes, or that is cancelled, leaves the queue without the mutex. The
kernel unlinks it, frees the record if it was the last waiter, and recomputes each holder up
the chain without it, stopping at the first whose priority does not change. It never touches
the word. A waiters bit left set costs the holder one kernel entry at its unlock, which finds
no record and frees the word.

On a chip with an MMU, a waiter whose word leaves its reach, for instance because the page is
unmapped from its address space, is ended the same way, with `-KOS_EFAULT`. So a parked
waiter can always write its own word, which the next section relies on.

## Owner-died: the mutex's status value

The mutex is the first real user of the result field, the per-thread status channel of the
blocking substrate ([Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)). Its
values come from the one error list that every syscall returns from
([Chapter 3.9](the-syscall-path-trap-dispatch-return.md)): `0` for a normal grant, and
otherwise a negated `KOS_E*` code. Most of those mean the caller does not hold the mutex:
`-KOS_EDEADLK` for the refusals the walk found, `-KOS_ECANCELED` for a waiter cancelled while
parked, `-KOS_ETIMEDOUT` for a waiter whose deadline passed or a try that found the mutex held.
And one code does not.

That code arises when a holder dies. There are two cases.

- **A holder with waiters.** Its records are on its held list. When the thread exits, the
  kernel hands each of them to its highest-priority waiter: it writes the waiter's id into the
  word, through the waiter's own view of the word, since the dying thread's memory may leave
  with it. It sets the waiter's result to `-KOS_EOWNERDEAD` and wakes it.
- **A holder with no waiter.** The kernel never knew of the lock. The dead thread's id stays
  in the word, and that id is the mark. The next taker's compare-and-swap fails, so it enters
  the kernel, finds no record, and finds the id dead. The kernel takes the word for it with
  one compare-and-swap and answers `-KOS_EOWNERDEAD`. Exactly one taker is told, because its
  own id replaces the dead one. The holder's id was written by its own compare-and-swap, so a
  thread ended between any two of its instructions leaves the word free or naming itself.
  There is no moment at which the word is held and names nobody.

Either way the thread that gets the code holds the mutex. The code tells it that the data the
mutex protects may be inconsistent, because the previous holder died in the middle of its
critical section. This is the `EOWNERDEAD` idea of the Portable Operating System Interface
(POSIX), reduced to one return value. It needs no *robust list*, the list of held locks that a
Linux thread registers so that its kernel can find them at its death: the dead id in the word
is enough. What the thread does about it is its own policy. The kernel's job is only to never
strand a waiter, and to tell it the truth.

There is one limit, and it is the generation. A dead holder's id reads as live again only when
the generation of its slot has gone all the way round to the same value, and the new thread in
that slot could write the word. The generation is at least 24 bits wide, so that takes more
than 16 million new threads in that one slot.

Read the owner-died value carefully, because it is the one place in the whole interface where
a negative return means the operation **succeeded**. Everywhere else, a negative return means
the call did nothing and the caller owns nothing. Here the mutex *is* held, so the reflex test

```
if (rc < 0) { return rc; }   // WRONG for a mutex lock
```

walks away from a mutex it holds and strands every waiter behind it, for good. The correct
shape tests that code by name:

```
int const rc = kos_mutex_lock(&m, KOS_TIMEOUT_NONE);
if (rc == -KOS_EOWNERDEAD)
{
    // HELD. Repair or discard whatever the mutex protects, then carry on and unlock.
}
else if (rc < 0)
{
    return rc;   // really not held
}
```

The alternative would be a positive success value of its own, which is exactly the kind of
private value the blocking substrate refuses to have
([Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md)): every wake reports through
one list of codes, so that no waker has to know which object woke a thread. The price of that
rule is this one special case, and it is written at the top of the error header, not left for
a caller to discover.

There is no capability to close and no object to free. The word is the mutex: any thread that
can write the word can take the lock, and only the holder can release it, since an unlock by
any other thread answers `-KOS_EPERM`. A thread that wants out unlocks, and a thread that dies
is handled above.

## Where to go next

- The park and wake the mutex waits on, and the three-step wake whose step 2 is the hand-over
  above: [Chapter 2.2](the-blocking-substrate-one-wait-wake-primitive.md), *The blocking
  substrate*.
- The compare-and-swap that takes the word, and how a core with none gets one:
  [Chapter 2.4](an-atomic-buys-definedness-not-atomicity.md), *An atomic buys definedness, not
  atomicity*.
- Why a free lock and a thread's own id need no syscall:
  [Chapter 3.9](the-syscall-path-trap-dispatch-return.md), *The syscall path*.
- The same priority function under call/reply donation:
  [Chapter 8.5](synchronous-call-and-reply.md), *Synchronous call and reply*.
- The code, from the word to the hand-over: [Journey 4, A mutex](../journey/04-mutex.md).
- The exact contract: [Architecture](../reference/architecture.md) ("Synchronization
  surface") and [Invariants](../reference/invariants.md) (the mutex word and owner death).
