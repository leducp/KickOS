<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 4. A mutex: uncontended, contended, priority inheritance

By the end of this chapter you will have followed a mutex through the kernel on the `qemu`
reference walk. You will see the user call and the C library lock built on it, the kernel object
behind it, the uncontended lock and unlock, a contended lock where the caller parks and the owner
hands the mutex over, and priority inheritance: a low thread holding, a high thread waiting, the
boost, a two-hop chain, and the drop on unlock. The semaphore comes last, as a contrast: it parks
on the same wait queue but has no owner, so nothing is ever boosted through it.

The ideas are in two Book chapters: [The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md)
(how a thread parks and is woken) and [Priority inheritance](../book/priority-inheritance-lending-urgency.md)
(why the mutex exists at all, and the invariants it keeps). This chapter shows the code that
keeps them. It assumes you have read [the first syscall](02-first-syscall.md) (trap, dispatch,
handle resolve) and [sleep](03-sleep.md) (park, wake, the deferred switch).

## The images we use

Two images from the `qemu` build contend a mutex for real:

- **`libcrace`** (`user/apps/common/libcrace/main.cc`). Four worker threads at the same
  priority call `printf`, `malloc` and `free` in a loop. Every one of those C library calls takes
  the C library's lock, which is a kernel mutex. With four threads and a short round-robin
  slice, the lock is contended thousands of times. This is the plain contended case: no
  priority differences, so no boost.
- **`selftest`** (`user/apps/common/selftest/main.cc`). Its mutex arms build exact scenarios:
  `t_mutex_pi` (a low thread holds, a high thread waits, a medium thread must not get in),
  `t_mutex_chain` (a boost that travels two hops), `t_mutex_multi_held` and
  `t_mutex_owner_died`. We use it for priority inheritance.

Launch either one stopped, as [Setup](00-setup.md) explains, and attach gdb:

```
qemu-system-arm -M mps2-an386 -nographic -semihosting \
    -kernel build/qemu/user/apps/common/libcrace/libcrace -gdb tcp::1234 -S &
gdb-multiarch build/qemu/user/apps/common/libcrace/libcrace \
    -ex 'target remote :1234'
```

Two gdb habits for this chapter. First, never `p kickos::sched::current()`: that makes gdb run
kernel code on the target, and if a breakpoint fires inside it the expression is abandoned.
Wait until the kernel has stored the current thread in a local (`c` in `mutex_lock`) and print
that. Second, several stops use `$_caller_is("...")` in a breakpoint condition, so a shared
function only stops when a mutex path called it. It is a gdb convenience function that needs a
gdb built with Python, which the Debian `gdb-multiarch` is. The gdb output below is trimmed:
long paths are shortened to `...` and source line numbers are left out, since they move as the
code changes. Addresses are from one run of the `qemu` build and will differ on yours.

### Stop 1: the user call
**Where:** `user/include/kickos/sys.h`, `kos_mutex_create`, `kos_mutex_lock`,
`kos_mutex_unlock`; `user/src/syscall_stubs.cc` (the same three); `user/include/kickos/kos.h`,
`kos::Mutex`

A mutex is named by a **capability**, an entry in the calling thread's capability table; user
code holds a **handle**, the number that picks the entry (see
[Naming a kernel object](../book/handles-and-the-resolve-chokepoint.md)).
`kos_mutex_create` writes a fresh handle to its out-pointer. The lock and unlock calls take that
handle. Possession is the whole authority: there is no separate "lock right", so the create
hands back a capability whose only right is `KOS_CAP_TRANSFER` (the right to give a copy to a
thread you spawn). The C++ class `kos::Mutex` creates one in its constructor, closes its handle
in its destructor, and forwards `lock()` and `unlock()` to the C calls.

Each stub is one `arch_syscall` with a syscall number from `user/include/kickos/sys/abi.h`
(`KOS_SYS_MUTEX_CREATE`, `KOS_SYS_MUTEX_LOCK`, `KOS_SYS_MUTEX_UNLOCK`) and the handle in the first
argument register. The exact argument list of the lock call, and the full list of codes it can
return, are in `user/include/kickos/sys.h`; read them there. One code needs a warning even at
this distance: `-KOS_EOWNERDEAD` is negative and yet means **you hold the mutex** (its previous
owner died holding it). A caller that treats every negative return as "not acquired" walks away
from a mutex it owns. Stop 12 shows where that code comes from.

**Try it:** `break kos_mutex_lock` then `continue` on `libcrace`. You stop in the stub with
`mtx=1`. That is not a handle main created: it is the C library's lock (next stop).

```
Breakpoint 1, kos_mutex_lock (mtx=1) at .../user/src/syscall_stubs.cc
```

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: Object model, capabilities & IPC](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 2: the C library's lock is a kernel mutex
**Where:** `user/src/newlib_lock.cc`, `libc_lock_acquire`, `libc_lock_release`,
`__retarget_lock_acquire_recursive`; `system/include/kickos/sys/cap_index.h`, `KOS_CAP_LIBC_LOCK`;
`kernel/syscall/cap.cc`, `cap_libc_lock_take`, `cap_seat_libc_lock`

The C library (newlib) protects its own shared state, the `FILE` buffers and the heap, with
locks. It lets the system supply them by defining the `__retarget_lock_*` functions. KickOS
defines all of them over **one** kernel mutex per address space (an address space is the
memory a set of threads share: the whole image on this MPU board). The kernel seats that
mutex at capability index 1, `KOS_CAP_LIBC_LOCK`, in every thread before its first
instruction: `cap_libc_lock_take` finds the mutex another thread of the same address space
already holds there, or makes a fresh one, and `cap_seat_libc_lock` writes it into the new
thread's table. That is why `mtx` was 1 above.

newlib nests its locks (`printf` can take the stdio lock and then a `FILE` lock), and one kernel
mutex is not recursive: locking it twice returns `-KOS_EDEADLK`. So the recursion is done in
userspace, with an owner and a depth kept in the address space's own memory:

```cpp
        kos_thread_t const self = kos_thread_self();
        if (__atomic_load_n(&s_owner, __ATOMIC_RELAXED) == self)
        {
            s_depth++;
            return;
        }
        // ...
        __atomic_store_n(&s_owner, self, __ATOMIC_RELAXED);
        s_depth = 1;
```

Only the thread holding the kernel mutex writes `s_owner`, so a thread that reads its own handle
there really is the owner. The release clears `s_owner` **before** it unlocks, so the next owner
never sees a stale name. Any refusal other than `0` or `-KOS_EOWNERDEAD` traps the thread: a C
library running unlocked would corrupt the heap silently.

**Try it:** `break kos_mutex_lock`, `continue`, then `bt`. The first lock in `libcrace` is main's
first `printf`:

```
#0  kos_mutex_lock (mtx=1) at .../user/src/syscall_stubs.cc
#1  (anonymous namespace)::libc_lock_acquire () at .../user/src/newlib_lock.cc
#2  __sfp_lock_acquire () at .../newlib/libc/stdio/findfp.c
#3  __sinit (s=...) at .../newlib/libc/stdio/findfp.c
#4  _vfprintf_r (...)
#5  printf (fmt=0x14254 "libcrace: %d workers x %d rounds\n")
#6  kickos_app_main () at .../user/apps/common/libcrace/main.cc
```

Then `finish` (the call returns `0`), `next` twice, and `p '(anonymous namespace)::s_owner'`:
it now holds main's thread handle, `1`. At the matching `kos_mutex_unlock` it is back to
`4294967295` (`KOS_THREAD_NONE`) before the trap is even taken.

**Why, and the contract:** [Book: What's under #include](../book/whats-under-include-libc-and-the-cxx-runtime.md),
[Reference: invariants](../reference/invariants.md) (`libc-lock-one-per-address-space`)

### Stop 3: the kernel object
**Where:** `kernel/include/kickos/sync.h`, `struct Mutex`; `kernel/include/kickos/instance.h`,
`mutexes`, `mutex_refs`; `kernel/syscall/syscall_obj.cc`, `mutex_create`;
`kernel/include/kickos/thread.h`, `Thread` fields `prio`, `base_prio`, `held_list`,
`wait_kind`, `wait_obj`, `wait_result`

The whole mutex is three fields:

```cpp
    struct Mutex
    {
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr; // intrusive link in the owner's held list
    };
```

`owner` is the lock state: null means free. `waiters` is a **wait queue**, the same intrusive
list of parked threads every blocking object uses ([sleep](03-sleep.md) met it). `next_held`
chains every mutex one thread owns into that thread's `held_list`; priority inheritance needs
that list to answer "who is waiting on anything I hold?".

Mutexes live in a fixed pool, `SlotPool<Mutex, KICKOS_MAX_MUTEXES> mutexes`, inside the one
kernel instance, with a reference count per slot in `mutex_refs` (one per capability that names
it). There is no heap in the kernel. `mutex_create` checks two budgets first: the task's own
share (`task_object_admit`, refused `-KOS_EAGAIN`) and a reserve kept so every address space can
still get its C library lock (`cap_mutex_create_admitted`, refused `-KOS_ENOMEM`). Then it takes
a slot, runs `mutex_init` and installs a capability with `cap_install`.

The other half of the state is on the threads. Each `Thread` carries two priorities: `prio`,
the **effective** priority the scheduler reads, and `base_prio`, the one it was given.
Inheritance only ever raises `prio`. A parked thread also carries a **wait edge**: `wait_kind`
(what it waits for) and `wait_obj` (the object), and `wait_result`, where whoever wakes it
writes the outcome.

**Try it:** on `libcrace`, `break kickos::mutex_lock`, `continue`, `p *m`. The first lock finds
the C library mutex free, in the kernel instance:

```
Breakpoint 1, kickos::mutex_lock (m=0x200049ec <kickos::detail::g_instance+572>) at .../kernel/sync/sync.cc
$1 = {owner = 0x0, waiters = {head = 0x0, tail = 0x0}, next_held = 0x0}
```

**Why, and the contract:** [Book: Adding a kernel object type](../book/adding-a-kernel-object-type-the-additive-recipe.md),
[Reference: Object model, capabilities & IPC](../reference/architecture.md#object-model-capabilities--ipc)

### Stop 4: the syscall arm
**Where:** `kernel/syscall/syscall.cc`, `syscall_body`, the `KOS_SYS_MUTEX_LOCK` and
`KOS_SYS_MUTEX_UNLOCK` cases; `kernel/syscall/cap.cc`, `cap_resolve_e`

[The first syscall](02-first-syscall.md) brought us to `syscall_body`, the big switch on the
syscall number. The lock arm does two things, and their split matters.

First it resolves the handle: `cap_resolve_e` checks the index, the generation and the type
(`CapType::CAP_MUTEX`), and returns the `Mutex*`. It asks for no right (`need` is `0`), because
holding the capability is the authority. This runs inside its own short `IrqLock` scope (the
kernel's critical section: it masks this core's interrupts, on Cortex-M by raising BASEPRI).

Then, **after** that scope has closed, it calls `mutex_lock(m)`. Most arms keep one `IrqLock`
across resolve and use. This one cannot, because `mutex_lock` may park, and the parked thread
must read its result with the lock released (Stop 8 explains why). The mutex cannot vanish in
the gap: the caller's own capability holds a reference to it.

The unlock arm is the ordinary shape: one `IrqLock`, resolve, `mutex_unlock(m)`, return. A
non-owner gets `-KOS_EPERM`.

**Try it:** `break kickos::mutex_lock`, `continue`, `bt`:

```
#0  kickos::mutex_lock (m=0x200049ec <kickos::detail::g_instance+572>) at .../kernel/sync/sync.cc
#1  (anonymous namespace)::syscall_body (nr=24, a0=1, ...) at .../kernel/syscall/syscall.cc
#2  syscall_dispatch (nr=24, a0=1, ...) at .../kernel/syscall/syscall.cc
#3  svc_trampoline () at .../arch/arm/armv7m/switch.S
```

`nr=24` is `KOS_SYS_MUTEX_LOCK`, `a0=1` is the handle. The backtrace stops at `svc_trampoline`
because the user frames are on the other stack (the process stack, PSP).

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: invariants](../reference/invariants.md) (`object-access-via-per-thread-cap`)

### Stop 5: the uncontended lock
**Where:** `kernel/sync/sync.cc`, `mutex_lock`, `held_push`; `kernel/include/kickos/sync.h`,
`park_cancel_pending`

`mutex_lock` opens its own `IrqLock`. The first thing it does is ask whether this thread has
been told to die (`park_cancel_pending`); if so it exits here, before it changes anything. Then
the fast path:

```cpp
            if (m->owner == nullptr)
            {
                m->owner = c;
                held_push(c, m);
                return 0;
            }
            if (m->owner == c)
            {
                return -KOS_EDEADLK;
            }
```

A free mutex costs two stores: the owner, and a push onto the owner's held list. No priority is
touched, no queue is scanned. This is the case that dominates real programs, and plain priority
inheritance is chosen partly because it costs nothing here. Locking a mutex you already own is
refused, not counted: the kernel mutex is not recursive (the C library builds recursion on top,
Stop 2).

**Try it:** stopped at the first `kickos::mutex_lock`, `p $m = m`, then `finish`. The value
returned is `0`. Then:

```
(gdb) p *$m
$3 = {owner = 0x20004e90 <kickos::detail::g_instance+1760>, waiters = {head = 0x0, tail = 0x0}, next_held = 0x0}
(gdb) p $m->owner->name
$4 = "main", '\000' <repeats 11 times>
(gdb) p $m->owner->held_list == $m
$5 = true
```

**Why, and the contract:** [Book: Priority inheritance, "Boosting: the lock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 6: the uncontended unlock
**Where:** `kernel/sync/sync.cc`, `mutex_unlock`, `held_remove`, `wq_pop_highest`,
`thread_effective_prio`

`mutex_unlock` checks the caller is the owner, takes the mutex off the owner's held list, and
asks the wait queue for its highest-priority waiter. With nobody waiting, `wq_pop_highest`
returns null, the owner is cleared, and the releaser's priority is **recomputed**:
`thread_effective_prio(c)` takes the maximum of its `base_prio` and every donor still lending to
it. If that differs from its current `prio`, the releaser is lowered and the scheduler runs,
because a lower priority can let a waiting middle-priority thread in. Uncontended, nothing
changed, so it simply returns `0`.

**Try it:** `break kickos::wq_pop_highest if $_caller_is("kickos::mutex_unlock") && q.head == 0`,
`continue`, `finish`. The value returned is `(kickos::Thread *) 0x0`. `next` three times walks
past the recompute without calling `set_prio`, and `p m->owner` is `0x0`.

**Why, and the contract:** [Book: Priority inheritance, "Reverting: the unlock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 7: the contended lock parks the caller
**Where:** `kernel/sync/sync.cc`, `mutex_lock` (the two passes and the park), `wq_block`,
`park_queueless`; `kernel/include/kickos/thread.h`, `Thread::wait_mutex`, `WAIT_MUTEX`

When the mutex has another owner, the caller must wait. Before it parks it lends its priority
down the chain of owners (Stops 9 and 10). The walk follows `wait_mutex()`: for a thread parked
on a mutex it returns that mutex, and for every other kind of wait it returns null, which ends
the walk. It runs twice. Pass 1 only reads: if the chain leads back to the caller, locking would
deadlock, and the call returns `-KOS_EDEADLK` having written nothing. Pass 2 does the boosting.
In `libcrace` all workers share priority 10, so pass 2 stops at its first test
(`t->prio >= c->prio`) and boosts nothing.

Then the park, and after it the resume:

```cpp
            epoch = wq_block(ask)(m->waiters, WAIT_MUTEX, m, nullptr, lock);
        }
        // The wait_result read must be BOTH outside the critical section AND after the
        // barrier, or it returns the pre-block value.
        wq_confirm_resume(c, epoch);
        return static_cast<int>(c->wait_result);
```

`wq_block` is the shared park used by every blocking object. It marks the thread `BLOCKED`,
takes it off the ready list, seats the wait edge (`WAIT_MUTEX`, `m`), appends it to
`m->waiters`, and asks the scheduler to switch away. It returns the thread's `switch_count`
before the switch, called the epoch:

```cpp
    uint32_t wq_block(List& q, WaitKind kind, void* obj, Thread* woken, Held held)
    {
        Thread* c = sched::current();
        uint32_t const epoch = park_queueless(c, kind, obj, held);
        c->wait_queue = &q;
        q.push_back(&c->link);
        // BEFORE the reschedule, which on a stall never returns.
        KOS_TRACE(::kickos::KOS_TR_PARK, KOS_TRACE_ID(c), KOS_TRACE_ID(&q));
        sched::reschedule(woken, held);
        return epoch;
    }
```

The edge and the queue link are written by the same call, inside the same critical section.
Another locker walking the chain a moment later must already see this thread as "waiting on
`m`", or it would stop the walk one hop short.

The queue is not sorted. A thread is appended at the tail, and the waker scans for the highest
priority at pop time. That is what lets a boost change a parked thread's priority with no
re-queue.

**Try it:** on `libcrace`, `break kickos::mutex_lock if m->owner != 0`, `continue`, `next` (so
`c` is set), then save what you see:

```
(gdb) p c
$1 = (kickos::Thread *) 0x20004fe0 <kickos::detail::g_instance+2096>
(gdb) p c->prio
$2 = 10 '\n'
(gdb) p m->owner
$3 = (kickos::Thread *) 0x20005130 <kickos::detail::g_instance+2432>
(gdb) p m->owner->prio
$4 = 10 '\n'
(gdb) p $m = m
(gdb) p $c = c
```

Now `delete`, `break kickos::mutex_unlock if $m->waiters.head != 0`, `continue`. Some other
thread is unlocking; the one you saved is parked:

```
(gdb) p $c->state
$9 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p $c->wait_kind
$10 = kickos::WAIT_MUTEX
(gdb) p $c->wait_obj
$11 = (void *) 0x200049ec <kickos::detail::g_instance+572>
```

All workers have the same name, `racer`, so compare thread addresses, not names. You may find
several threads queued: `p *$m` shows `head` and `tail` far apart.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: invariants](../reference/invariants.md) (the deferred-switch entries)

### Stop 8: the owner unlocks and hands the mutex over
**Where:** `kernel/sync/sync.cc`, `mutex_unlock`, `wq_pop_highest`, `wq_peek_highest`,
`transfer_to`, `wq_confirm_resume`; `kernel/sched/sched.cc`, `sched::wake`

With waiters, unlock does not free the mutex and let them race for it. It gives it directly to
one of them:

```cpp
        held_remove(c, m);
        Thread* w = wq_pop_highest(m->waiters);
        // ...
        transfer_to(m, w, 0);
        uint8_t const np = thread_effective_prio(c);
        if (np != c->prio)
        {
            sched::set_prio(c, np, lock);
        }
        sched::wake(w, lock);
        return 0;
```

`wq_pop_highest` asks `wq_peek_highest`, which walks the queue and keeps the first thread with
the strictly highest `prio`. So among equals the one that waited longest wins (FIFO). The pop
unlinks the winner and **clears its wait edge**. `transfer_to` makes it the owner, writes `0` to
its `wait_result` (a normal grant) and pushes the mutex onto its held list. The releaser's
priority is recomputed over what it still holds. Last, `sched::wake` moves the new owner to
`READY` and lets the scheduler decide whether it runs now.

Everything the waiter needs is written **by the waker**, while the waiter is still parked. A
parked thread never clears its own edge or writes its own result. On this Cortex-M the switch
is deferred: the kernel pends PendSV and the switch happens once the critical section ends. If
the waiter cleared its own edge after waking, there would be a window where it is still parked
but already looks like it waits on nothing.

That deferred switch is also why the waiter's side has a barrier. When `wq_block` returns inside
`mutex_lock`, the thread may not have switched out yet. `wq_confirm_resume` spins until the
thread's `switch_count` has moved past the epoch, which proves a real switch out and back in
happened. Only then is `wait_result` the waker's value.

**Try it:** on `libcrace`,
`break kickos::wq_pop_highest if $_caller_is("kickos::mutex_unlock") && q.head != 0`,
`continue`, `finish`, then `p $w = $`:

```
Value returned is $1 = (kickos::Thread *) 0x20005130 <kickos::detail::g_instance+2432>
(gdb) p m->owner
$3 = (kickos::Thread *) 0x20004fe0 <kickos::detail::g_instance+2096>
(gdb) p $w->state
$4 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p $w->wait_kind
$5 = kickos::WAIT_NONE
```

The waiter is still `BLOCKED`, but its edge is already gone. `next` until the line
`sched::wake(w, lock);`, then:

```
(gdb) p m->owner == $w
$6 = true
(gdb) p $w->wait_result
$7 = 0
(gdb) p $w->held_list == m
$8 = true
```

`next` past the wake: `p $w->state` is `READY`. It does not run yet: it has the same priority as
the releaser, which keeps the CPU until its slice ends.

For the waiter's side, start again with
`break kickos::wq_confirm_resume if $_caller_is("kickos::mutex_lock")`, `continue`:

```
Breakpoint 1.1, kickos::wq_confirm_resume (c=0x20005280 <...+2768>, epoch=1) at .../kernel/sync/sync.cc
(gdb) p c->switch_count
$2 = {v_ = {... _M_i = 2}, ...}
(gdb) delete
(gdb) finish
(gdb) p c->wait_result
$4 = 0
(gdb) p m->owner == c
$5 = true
```

The epoch was 1, the count is 2: the switch happened, so the read is safe.

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Book: Context switching](../book/context-switching-and-the-silicon-contract.md)

### Stop 9: priority inheritance, the boost
**Where:** `kernel/sync/sync.cc`, `mutex_lock` (pass 2); `kernel/sched/sched.cc`,
`sched::set_prio`; `user/apps/common/selftest/main.cc`, `t_mutex_pi`, `pi_low`, `pi_high`,
`pi_med`

Now switch to the `selftest` image. In `t_mutex_pi`, `piLo` (priority 8) locks the mutex, then
wakes `piHi` (20), which preempts it. `piHi` wakes `piMd` (12) and then locks the same mutex. Without inheritance,
`piMd` would now run ahead of `piLo` and keep `piHi` waiting for as long as it liked: that is
**priority inversion**. Pass 2 prevents it:

```cpp
                Thread* t = m->owner;
                int depth = 0;
                while (t != nullptr)
                {
                    if (t->prio >= c->prio)
                    {
                        break;
                    }
                    sched::set_prio(t, c->prio, lock);
                    if (t->wait_mutex() == nullptr)
                    {
                        break;
                    }
                    t = t->wait_mutex()->owner;
                    // ...
                }
```

`sched::set_prio` is the only function that writes an effective priority. On this single-core
build, its job is to keep the ready list consistent: a ready list is filed by priority, so a
`READY` or `RUNNING` thread is taken off, changed, and put back. A `BLOCKED` thread is on no
ready list and its wait queue is not sorted, so the value is simply written.

```cpp
            if (t->state == ThreadState::READY or t->state == ThreadState::RUNNING)
            {
                policy_on_remove(t);
                t->prio = p;
                policy_on_ready(t);
                return;
            }
            // A BLOCKED thread is on no ready list, and neither queue it can be on is
            // prio-ordered: wq_pop_highest rescans at pop, the timer list is deadline-sorted.
            t->prio = p;
```

`base_prio` is never touched. The boost is a loan, recorded only in `prio`.

**Try it:** on `selftest`, `break kickos::sched::set_prio if $_caller_is("kickos::mutex_lock")`,
`continue`. The first boost of the run is this one:

```
Breakpoint 1, kickos::sched::set_prio (t=0x20004fe0 <...+2096>, p=20 '\024', held=...) at .../kernel/sched/sched.cc
(gdb) p t->name
$1 = "piLo", '\000' <repeats 11 times>
(gdb) p t->prio
$2 = 8 '\b'
(gdb) p t->base_prio
$3 = 8 '\b'
(gdb) p t->state
$5 = {v_ = kickos::ThreadState::READY}
(gdb) up
(gdb) p c->name
$6 = "piHi", '\000' <repeats 11 times>
(gdb) p m->owner == t
$8 = true
```

`piLo` is `READY`, not running: `piHi` preempted it. After this call it is ready at 20, so when
`piHi` parks, `piLo` runs next, ahead of `piMd`.

**Why, and the contract:** [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md),
[Reference: Scheduler](../reference/architecture.md#scheduler-the-core-constraint)

### Stop 10: the chain
**Where:** `kernel/sync/sync.cc`, `mutex_lock` (passes 1 and 2); `kernel/include/kickos/thread.h`,
`Thread::wait_mutex`; `user/apps/common/selftest/main.cc`, `t_mutex_chain`, `ch_a`, `ch_b`, `ch_c`

A boost must follow the chain of owners. In `t_mutex_chain`, `chC` (5) holds M2. `chB` (10) holds
M1 and then blocks on M2, which boosts `chC` to 10. Then `chA` (20) blocks on M1. Pass 2 starts
at M1's owner, `chB`: it is below 20, so it is boosted; it is itself parked on M2, so
`wait_mutex()` returns M2 and the walk moves to M2's owner, `chC`, and boosts it too. Without
that second hop, `chD` (15) would run ahead of `chC`, and `chA` would wait behind `chD`.

The walk ends when an owner is not parked on a mutex (`wait_mutex()` returns null), or when an
owner is already at the caller's priority or above. That early stop is safe: every owner further
along was boosted at least that high when its own waiter blocked. A thread parked on a
**semaphore** answers null too, so inheritance stops there; a semaphore has no owner to boost.
Pass 1 follows the same edges first, read-only. If they lead back to the caller, the lock would
deadlock. It is refused before pass 2 has boosted anyone, so a refused lock leaves no boost
behind. A depth bound of `KICKOS_MAX_MUTEXES` backs both walks up.

**Try it:** on `selftest`, stop only at a boost of a thread that is itself waiting on a mutex:

```
break kickos::sched::set_prio if $_caller_is("kickos::mutex_lock") && t->wait_kind == kickos::WAIT_MUTEX
```

```
Breakpoint 1, kickos::sched::set_prio (t=0x20005130 <...+2432>, p=20 '\024', held=...) at .../kernel/sched/sched.cc
(gdb) p t->name
$1 = "chB", '\000' <repeats 12 times>
(gdb) p t->state
$4 = {v_ = kickos::ThreadState::BLOCKED}
(gdb) p ((kickos::Mutex*)t->wait_obj)->owner->name
$5 = "chC", '\000' <repeats 12 times>
(gdb) p ((kickos::Mutex*)t->wait_obj)->owner->prio
$6 = 10 '\n'
(gdb) up
(gdb) p c->name
$7 = "chA", '\000' <repeats 12 times>
```

To see every loan and every return in the whole run, log both directions:

```
break kickos::sched::set_prio if $_caller_is("kickos::mutex_lock")
commands
silent
printf "boost %s: %d -> %d (base %d)\n", t->name, t->prio, p, t->base_prio
continue
end
break kickos::sched::set_prio if $_caller_is("kickos::mutex_unlock")
commands
silent
printf "drop  %s: %d -> %d (base %d)\n", t->name, t->prio, p, t->base_prio
continue
end
continue
```

The first lines it prints:

```
boost piLo: 8 -> 20 (base 8)
drop  piLo: 20 -> 8 (base 8)
boost chC: 5 -> 10 (base 5)
boost chB: 10 -> 20 (base 10)
boost chC: 10 -> 20 (base 5)
drop  chC: 20 -> 5 (base 5)
drop  chB: 20 -> 10 (base 10)
boost odOwn: 8 -> 12 (base 8)
boost mhB: 6 -> 20 (base 6)
drop  mhB: 20 -> 6 (base 6)
```

Read the chain lines: one hop when `chB` blocks, two hops when `chA` blocks, and each thread
handed back down to its own base when it unlocks. Run the same breakpoints on `libcrace` and
nothing prints: equal priorities never boost.

**Why, and the contract:** [Book: Priority inheritance, "Boosting: the lock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 11: the drop on unlock
**Where:** `kernel/sync/sync.cc`, `mutex_unlock`, `thread_effective_prio`;
`user/apps/common/selftest/main.cc`, `t_mutex_multi_held`

On unlock the releaser's priority is not "reset to base". It is recomputed from what is still
lending to it:

```cpp
        uint8_t p = t->base_prio;
        for (Mutex* h = t->held_list; h != nullptr; h = h->next_held)
        {
            Thread const* const w = wq_peek_highest(h->waiters);
            if (w != nullptr and w->prio > p)
            {
                p = w->prio;
            }
        }
```

`thread_effective_prio` goes on to count two more kinds of donor: callers waiting for this
thread's reply, and callers queued on an endpoint it serves. Those are
[call and reply](05-call-and-reply.md)'s, and one function computes all of them, so a mutex
unlock in the middle of serving a call never throws away the caller's loan.

The recompute is what makes holding two mutexes correct. In `t_mutex_multi_held`, `mhB` (6)
holds M1 and M2, and `mhH` (20) waits on M1. When `mhB` releases M2, M1 still has a waiter at
20, so the recompute gives 20 again and `set_prio` is not called at all. That is why the log
above shows one `boost mhB` and one `drop mhB`, the drop coming only at the unlock of M1. A
reset to base at the first unlock would let `mhD` (12) in.

In the waiter case, the drop is followed by `sched::wake` of the new owner, whose scheduling
pass takes over. In the no-waiter case (Stop 6), `mutex_unlock` calls `sched::reschedule`
itself after lowering.

**Try it:** on `selftest`, `break kickos::sched::set_prio if $_caller_is("kickos::mutex_unlock")`,
`continue`. `p t->name` is `"piLo"`, `p t->prio` is `20`, `p p` is `8`, and `p t->state` is
`RUNNING`: the releaser is lowering itself. `p $lo = t`, `delete`, then
`break kickos::wq_confirm_resume if $_caller_is("kickos::mutex_lock")`, `continue`:

```
Breakpoint 2.1, kickos::wq_confirm_resume (c=0x20005130 <...+2432>, epoch=2) at .../kernel/sync/sync.cc
(gdb) p c->name
$4 = "piHi", '\000' <repeats 11 times>
(gdb) p c->prio
$5 = 20 '\024'
(gdb) p c->wait_result
$6 = 0
(gdb) p $lo->prio
$7 = 8 '\b'
(gdb) p $lo->state
$8 = {v_ = kickos::ThreadState::READY}
```

`piHi` got the mutex and the CPU straight away. `piLo` is back at 8 and waits behind `piMd`.

**Why, and the contract:** [Book: Priority inheritance, "Reverting: the unlock path"](../book/priority-inheritance-lending-urgency.md)

### Stop 12: when a waiter or an owner goes away
**Where:** `kernel/thread/park.cc`, `thread_abort_park` (the `WAIT_MUTEX` case);
`kernel/syscall/cap.cc`, `obj_close_protocol` (the `CAP_MUTEX` case);
`kernel/sync/sync.cc`, `mutex_force_unlock`; `user/apps/common/selftest/main.cc`,
`t_mutex_owner_died`

Two exits from the normal story.

A **waiter cancelled** while parked (its thread or task is being killed) is not handed the
mutex. `thread_abort_park` unlinks it from `m->waiters` first, then recomputes the owner's
priority, so the owner no longer carries a loan from a thread that left. Only the direct owner
is recomputed; a boost further along a chain is returned when that link is itself unlocked.

An **owner that dies** holding the mutex must not strand its waiters. When a thread exits, its
capabilities are closed one by one, and `obj_close_protocol` handles the mutex case:

```cpp
                Mutex* m = kernel().mutexes.resolve(e.obj);
                if (m == nullptr or m->owner != closer)
                {
                    return 0; // not the owner: an ordinary refcount close, no protocol
                }
                if (not teardown)
                {
                    return -KOS_EBUSY; // refuse a voluntary close of a mutex you OWN (unlock first)
                }
                // The owner is exiting. Force-unlock BEFORE the ref drop so a
                // waiter is never stranded; the woken lock() caller gets OWNER_DIED.
                mutex_force_unlock(m, closer, held);
                return 0;
```

`mutex_force_unlock` is the unlock without the recompute: it hands the mutex to the highest
waiter with `MUTEX_OWNER_DIED` (`-KOS_EOWNERDEAD`) as the result. The dying thread keeps its
boost for the rest of its teardown. A live owner that tries to close its handle while still
holding is refused `-KOS_EBUSY` instead: the handle is the only way to unlock, so dropping it
would strand the waiters. The C++ `kos::Mutex` destructor runs into exactly that refusal if you
let a locked `kos::Mutex` go out of scope.

**Try it:** on `selftest`, `break kickos::mutex_force_unlock`, `continue`:

```
Breakpoint 1, kickos::mutex_force_unlock (m=0x20004a1c <...+620>, dying=0x20004fe0 <...+2096>, held=...) at .../kernel/sync/sync.cc
(gdb) p dying->name
$1 = "odOwn\000\000\000\000\000\000\000\000\000\000"
(gdb) p dying->prio
$2 = 12 '\f'
(gdb) p dying->base_prio
$3 = 8 '\b'
(gdb) p $m = m
(gdb) finish
(gdb) p $m->owner->name
$5 = "odWt", '\000' <repeats 11 times>
(gdb) p $m->owner->wait_result
$6 = -130
(gdb) p $m->owner->state
$7 = {v_ = kickos::ThreadState::READY}
```

`-130` is `-KOS_EOWNERDEAD`. `finish` lands in `obj_close_protocol`, the caller above.

**Why, and the contract:** [Book: Priority inheritance, "Owner-died"](../book/priority-inheritance-lending-urgency.md),
[Reference: invariants](../reference/invariants.md) (`syscall-return-abi`,
`task-scoped-death-and-a-cancel-that-reaches-every-park`)

### Stop 13: the semaphore, the same substrate without an owner
**Where:** `kernel/include/kickos/sync.h`, `struct Semaphore`; `kernel/sync/sync.cc`, `sem_wait`,
`sem_post`; `kernel/syscall/syscall.cc`, the `KOS_SYS_SEM_WAIT` and `KOS_SYS_SEM_POST` cases

A semaphore is a count and a wait queue: `int count` and `List waiters`. `sem_wait` takes a
token if `count > 0`, returns at once if the caller passed a zero timeout, and otherwise arms a
deadline (`ktime_deadline_arm`) and parks with the same `wq_block`, tagged `WAIT_SEM`. `sem_post`
is the same pop-transfer-wake as the mutex hand-over, with a token in place of ownership:

```cpp
    bool sem_post(Semaphore* s)
    {
        IrqLock lock;
        Thread* w = wq_pop_highest(s->waiters);
        if (w != nullptr)
        {
            w->wait_result = 0; // the token goes straight to the waiter, count stays put
            sched::wake(w, lock);
            return true;
        }
        if (s->count >= KOS_SEM_COUNT_MAX)
        {
            return false; // refuse rather than wrap a signed int (UB)
        }
        s->count++;
        return true;
    }
```

Compare it with Stop 8, line by line. The differences are the whole reason the mutex exists as
a second kernel object:

- A semaphore has **no owner**. Any thread holding the `SIGNAL` right can post, and
  `sem_post` takes its own `IrqLock`, so it may also run in interrupt context. A mutex unlock
  must come from the owner thread.
- With no owner there is nobody to lend priority to. `wait_mutex()` answers null for a
  `WAIT_SEM` park, so the chain walk of Stop 10 stops there.
- With no waiter, a post is **banked** in `count` for a later wait. A mutex has nothing to bank.
- A semaphore's rights are split (`CAP_WAIT`, `CAP_SIGNAL`), checked at resolve. A mutex asks
  for none.

**Try it:** on `libcrace`, `break kickos::sem_post`, `continue`. The first post is a worker
reporting done to main, which is parked in `kos_sem_wait` on the `done` semaphore:

```
Breakpoint 2, kickos::sem_post (s=0x200048e8 <kickos::detail::g_instance+312>) at .../kernel/sync/sync.cc
#1  (anonymous namespace)::syscall_body (nr=6, a0=2, ...) at .../kernel/syscall/syscall.cc
(gdb) p *s
$7 = {count = 0, waiters = {head = 0x20004ea8 <kickos::detail::g_instance+1784>, tail = 0x20004ea8 <kickos::detail::g_instance+1784>}}
```

One waiter: `0x20004ea8` is the `link` inside main's `Thread` (main is at `+1760`, Stop 5).

**Why, and the contract:** [Book: The blocking substrate](../book/the-blocking-substrate-one-wait-wake-primitive.md),
[Reference: Object model, capabilities & IPC](../reference/architecture.md#object-model-capabilities--ipc)
("Synchronization surface")

## Where to next

- [5. Call and reply](05-call-and-reply.md): the other two kinds of priority donor, and the
  endpoint wait queues.
- [11. Multicore](11-multicore.md): `sched::set_prio` when the owner's ready list belongs to
  another core (the `RESEAT` request), and what the kernel lock adds to the `IrqLock`.
- [9. Fault and kill](09-fault-and-kill.md): the teardown that reaches `obj_close_protocol`.
- [Book: Priority inheritance](../book/priority-inheritance-lending-urgency.md) for the full
  argument, and [Book: One kernel lock, many ready queues](../book/one-lock-many-ready-queues.md).

## Exercises

1. **Self-deadlock.** In `selftest`, `t_mutex_deadlock` locks a mutex it already holds. Predict
   which line of `mutex_lock` returns and what `finish` prints. Then find it with a gdb loop
   (a `next` inside `commands` would end the command list, so use `while` instead):

   ```
   break kickos::mutex_lock if m->owner != 0
   while 1
     continue
     next
     if m->owner == c
       finish
       loop_break
     end
   end
   ```

   Dozens of ordinary contended locks go by first. Check your prediction against the value
   `finish` prints, and against `KOS_EDEADLK` in `system/include/kickos/sys/errno.h`. Note the
   `a0` in the frame `finish` lands in: a dynamic handle is an opaque number, not a small index.
2. **Who wins among equals.** In `libcrace`, at a hand-over (Stop 8), save `$h = m->waiters.head`
   before `finish`. Predict whether `&$w->link == $h`. What would change if `wq_peek_highest`
   used `>=` instead of `>`?
3. **Break the chain.** In `t_mutex_chain`, imagine `chB` waits on a semaphore instead of on M2.
   Predict the log of Stop 10, then reason about what `chD` (15) would do to `chA`.
