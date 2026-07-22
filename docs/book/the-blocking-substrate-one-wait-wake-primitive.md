<!-- SPDX-License-Identifier: CECILL-C -->
# The blocking substrate: one wait/wake primitive

> The concept behind every blocking syscall in KickOS. A semaphore wait, a mutex
> lock, an endpoint receive -- all three park a thread and later wake it, and all
> three do it through the *same* primitive. This chapter teaches that primitive and
> the one subtle hazard it hides on a deferred-switch architecture. Points into
> `../reference/invariants.md` (the deferred-switch and switch-frame contracts) and
> the sync code (`kernel/sync/sync.cc`, `kernel/sched/sched.cc`) for the exact
> contract; Chapter 2 (kernel model) for `IrqLock` and the scheduler, and
> Chapter 3.5 for why the switch behaves differently across ISAs.

## What "blocking" actually is

A thread that calls `sem_wait` on an empty semaphore does not spin and it does not
return. It *parks*: the kernel records that the thread is waiting, takes it off the
run queue, and switches to someone else. Later, some other thread (or an interrupt)
does the thing the parked thread was waiting for -- posts the semaphore, unlocks the
mutex, sends the message -- and the parked thread is put back on the run queue and
eventually resumes, right after the call that blocked it, as if the call had simply
taken a long time.

Every blocking operation in a kernel is this same shape: *park now, be woken later.*
The question this chapter answers is whether each blocking object should implement
that shape itself, or whether there should be one implementation they all share.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (sleep/wakeup and the
lost-wakeup problem).*

## Why it matters: the lost wakeup

The classic bug in any park/wake code is the **lost wakeup**. A thread checks a
condition ("is the count zero?"), decides to block, and -- in the window between the
check and actually parking -- another thread changes the condition and signals. If
the signal fires before the first thread is on the wait queue, the wakeup lands on
nobody, and the first thread parks forever waiting for an event that already
happened.

The fix is always the same: the *predicate that decides to block* and the *act of
parking* must be one atomic step with respect to the waker. On a single-core kernel
that atomic step is a critical section -- one continuous `IrqLock` (Chapter 2)
spanning both the check and the park, so no waker can run in between.

That fix is easy to state and easy to get subtly wrong, and there is one such fix per
blocking object. If a semaphore, a mutex, and an endpoint each roll their own park/wake
loop, you get three chances to introduce a lost-wakeup bug, and three places to audit
when you suspect one. That is the argument for a single primitive.

## The options

**Option A -- each object owns its blocking logic.** A semaphore has its own wait
queue and its own block/wake; a mutex has its own; an endpoint has its own. Maximum
flexibility per object, but the lost-wakeup discipline is duplicated N times and
drifts.

**Option B -- one shared primitive; every object reduces to it.** There is exactly
one way to park a thread and one normative protocol to wake it. Each object adds only
the part that is genuinely its own (what it hands the woken thread), and inherits the
tricky park/wake correctness for free.

KickOS takes Option B. The insight that makes it cheap is that a wait queue is not a
new data structure at all.

## KickOS's choice: the waitq

**A wait queue IS a list of blocked threads.** It is the same intrusive `List`
(Chapter 2) the scheduler already uses, whose members happen to be threads in the
`BLOCKED` state. No new struct. The primitive is a *pair of operations* plus a
*normative wake protocol*.

```
namespace kickos
{
    // Park current on q and switch away. Returns when a waker has popped this
    // thread from q and readied it. Thread context only (parking from an ISR
    // has no thread identity to park).
    void wq_block(List& q);

    // Remove and return the highest-priority waiter (FIFO among equals), or
    // nullptr if q is empty. Selection only: does not change any thread state
    // and does not schedule. Callable from thread OR interrupt context.
    Thread* wq_pop_highest(List& q);
}
```

`wq_block` is the parking half: detach the current thread from the run queue, set its
state to `BLOCKED`, remember which queue it is on, push it onto that queue, and call
`sched::reschedule()` to switch away. The caller must already hold the `IrqLock` that
also covered the predicate -- that is the lost-wakeup fix, and it is a *precondition*
of `wq_block`, not something it does for you.

`wq_pop_highest` is the selection half: scan the queue, pick the highest-priority
thread, and unlink it. It deliberately does **not** wake the thread or touch its
state -- it only chooses.

### The lazy at-pop scan is load-bearing

`wq_pop_highest` scans for the highest priority *at pop time* rather than keeping the
queue sorted. This is not laziness for its own sake: it is what lets a thread's
priority change *while it is parked* without any re-queue work. Priority inheritance
(Chapter 2.3) boosts a parked thread's priority precisely in that situation, and
because the queue is unsorted and scanned at pop, the boost is simply visible the next
time someone pops. A sorted queue would need to be re-sorted on every boost. Do not
"optimize" the scan into a sorted insert -- it would break the cheapest correct
structure for PI.

## The wake protocol: pop, transfer, wake

Waking is never a single call. It is a **three-step protocol, run under one
`IrqLock`**:

```
w = wq_pop_highest(q);   // 1. select the waiter
<per-type transfer>      // 2. hand it what it was waiting for
sched::wake(w);          // 3. make it runnable (schedules now in thread
                         //    context; defers to interrupt exit in ISR context)
```

Step 2 is the **only** extension point, and it is what makes the primitive shared
rather than forked. Each blocking object fills in step 2 with the thing it, and only
it, knows how to do:

| object | what step 2 transfers |
|--------|------------------------|
| semaphore | nothing -- the token is handed implicitly; pop then wake |
| mutex | ownership of the lock, plus the priority-inheritance bookkeeping (Chapter 2.3) |
| endpoint | the message bytes copied peer-to-peer, plus the byte count or an error status (Chapter 8.3) |

Pop and wake are **deliberately separate calls.** Fusing them into a single
`wake_one(q)` would leave no place for step 2, and every object that needed to hand
something over would have to fork the primitive to get it. Keeping the gap open is
what lets one primitive serve three very different objects.

### The status channel: `wait_result`

Some blocking calls return a value: a mutex lock reports whether the previous owner
died holding it; an endpoint receive reports the byte count or an error. The waker
delivers that value through a single per-thread field, `Thread::wait_result`, written
in step 2 (before `sched::wake`) and read by the woken thread after it resumes. The
values form a **per-type namespace** -- the mutex's `1 = owner died` and the
endpoint's `-1 = broken pipe` never collide, because only that type's own resume code
ever reads the field. A semaphore wait returns `void`, so it never reads it.

One rule makes this safe: **the waker writes `wait_result`; the sleeper never writes
it.** The sleeper only reads, and only after it has genuinely resumed -- which brings
us to the one hazard that is not obvious.

## The hazard: a post-block read can be stale

Here is the trap, and it is worth teaching because it is invisible on the development
host and real on the hardware.

The natural way to consume `wait_result` is to read it right after `wq_block` returns,
while still holding the lock:

```
// TEMPTING, and WRONG on a deferred-switch architecture:
{
    IrqLock lock;
    ...
    wq_block(q);              // park
    return current->wait_result;   // read what the waker left us
}
```

On the development host this works. There `arch_switch` is a synchronous
`swapcontext`: when `wq_block` returns, the switch has *already happened*, the thread
really did stop and resume, and everything the waker wrote before waking us
happened-before our read.

On a deferred-switch architecture (ARM, and the same class of mechanism on RX and
RISC-V -- see Chapter 3.5) it is false. There `arch_switch` does not switch; it only
*pends* the switch (ARM PendSV), and `arch_irq_restore` carries no instruction
barrier. So `wq_block` **returns on the still-current, not-yet-switched thread.** The
actual context switch fires a few instructions later, when the outermost `IrqLock`
drops the interrupt mask and the pended switch finally tail-chains in. A read of
`wait_result` performed *right after `wq_block`, still under the lock*, therefore runs
**before the thread ever parked** -- it reads the pre-block value, not what the waker
will eventually write.

This is not a data race in the usual sense; it is a control-flow surprise. The line of
C that "obviously" runs after the block actually runs before it on that hardware.

### The fix: confirm the resume, then read outside the lock

Two rules, together:

1. **Read `wait_result` outside the block's critical section**, not while still
   holding the `IrqLock` that covered `wq_block`.
2. Before reading, call `wq_confirm_resume(current, epoch)`, where `epoch` is the
   thread's `switch_count` sampled *under the lock, before* `wq_block`. It spins until
   the switch count actually advances -- i.e. until the thread has genuinely been
   switched out and back in -- so the read that follows sees the waker's write.

```
uint64_t epoch;
{
    IrqLock lock;
    ...                       // predicate + transfer setup, all under one lock
    epoch = current->switch_count;   // sample BEFORE parking
    wq_block(q);              // park (returns pre-switch on deferred archs)
}                             // lock released -> the pended switch can now fire
wq_confirm_resume(current, epoch);   // spin until genuinely resumed
return current->wait_result;         // now the waker's write is visible
```

The waker's side stays simple and is the same on every architecture: under the lock,
it writes `wait_result` and clears any per-thread "what am I blocked on" marker for
the sleeper -- the sleeper touches neither. A semaphore wait skips all of this because
it returns `void`: with no post-block read, there is nothing that can be stale.

The transferable lesson: on a deferred-switch kernel, "the instruction after the
block" is not a synchronization point. If you need to observe state the waker
produced, wait for the switch to have actually occurred before you look.

## Why this is enough (and not more)

One primitive, two operations, a three-step wake protocol, and one confirm-resume
barrier cover semaphores, priority-inheritance mutexes, and IPC endpoints without any
of them forking the park/wake path. The parts that genuinely differ -- what a mutex
does to priorities, how an endpoint copies bytes -- live entirely in step 2, where
they belong. There is exactly one lost-wakeup discipline to get right, one place where
the deferred-switch subtlety is handled, and one field for delivering a result.

A future timed wait (wake either on the event or on a deadline) is the one planned
extension, and it fits the same shape: it needs one distinguishable timeout value per
type reserved in the `wait_result` namespace, and a deadline on the tickless timer
list (Chapter 2.1) alongside the queue membership. It does not need a second
primitive.

## Where to go next

- The mutex's step 2 -- ownership transfer plus priority inheritance:
  Chapter 2.3, *Priority inheritance*.
- The endpoint's step 2 -- the peer-to-peer message copy: Chapter 8.3, *Endpoints*.
- Why the switch is synchronous on the host but deferred on ARM/RX/RISC-V, and the
  frame/return contracts behind it: Chapter 3.5, *Context switching and the silicon
  contract*, and `../reference/invariants.md` (deferred-switch, switch-frame).
- The `IrqLock` critical-section model this rests on, and the scheduler it drives:
  Chapter 2, *Kernel model*.
