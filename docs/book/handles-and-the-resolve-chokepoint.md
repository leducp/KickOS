<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Naming a kernel object: the handle and the resolve chokepoint

> This chapter teaches the per-task capability handle mechanism -- the *concept* and
> KickOS's design reasoning. For the exact, current object model, link to the code-synced
> Reference: `../reference/architecture.md` ("Object model, capabilities & IPC") and the
> code it describes -- `kernel/include/kickos/cap.h`, `kernel/syscall/cap.cc`,
> `kernel/syscall/syscall.cc`, `kernel/include/kickos/slotpool.h`.

A microkernel spends its life doing things to objects on behalf of tasks that cannot touch
those objects directly: wait on *this* semaphore, post to *that* one, send on *that*
endpoint. So the very first question the kernel must answer, on every such syscall, is:
**which object does this task mean, and is it allowed to mean it?** Everything in this
chapter is about the one place that question gets answered -- the *resolve chokepoint* --
and about the sharp, easily-missed distinction between the part of that answer that keeps
the kernel correct and the part that merely helps an app find its own bugs.

## The problem with a global integer id

Today a KickOS semaphore lives in a per-*kernel* `SlotPool` and is named by an opaque
integer handle that `sem_resolve` validates (`kernel/syscall/syscall.cc`). The handle is
opaque, but it is **ambient**: it is a global name, and any task that can guess or forge
the integer can name the object. `kos_sem_wait(4)` from any thread reaches semaphore 4.
Isolation says a task should touch only what it has been *given*; a global namespace any
task can enumerate is the opposite -- it is ambient authority, and ambient authority
contradicts the isolation the MPU chapters (Chapter 7) work so hard to build. Fencing a
task's *memory* while leaving the *object namespace* open would be half a boundary.

## What a handle is

The M3 answer is a **capability handle**: a per-task, typed, rights-bearing, refcounted
reference to a kernel object.

- **Per-task.** The number `3` is not a global object id; it is an index into *this
  task's* table. Task A's `3` and task B's `3` are unrelated. A task can only name objects
  its table holds an entry for -- it cannot forge a name for an object it was never given.
- **Typed.** An entry records what kind of object it names (semaphore, mutex, endpoint).
  A handle to a semaphore cannot be used where an endpoint is expected.
- **Rights-bearing.** An entry carries a small set of rights bits (WAIT, SIGNAL,
  TRANSFER). Holding a handle is not blanket authority over the object; it is exactly the
  operations the rights permit.
- **Refcounted.** Many tasks may hold handles to one shared object; the object's lifetime
  is governed by how many handles name it, not by any single task (see *Lifecycle*).

The shape is Zircon's `zx_handle_t` more than seL4's CNodes -- a small flat array embedded
in the TCB, no capability-graph boot manifest. Concretely (design sketch, not a contract):

```
enum class CapType : uint8_t { CAP_EMPTY = 0, CAP_SEM, CAP_MUTEX, CAP_ENDPOINT };

struct CapEntry
{
    int32_t  obj;    // the GLOBAL generational object handle this cap names
    uint8_t  type;   // CapType -- checked at resolve
    uint8_t  rights; // WAIT / SIGNAL / TRANSFER -- checked at resolve
    uint16_t gen;    // per-slot cap generation, bumped on close
};
```

## The resolve chokepoint: validate, then use, under one lock

The load-bearing discipline is that **every object-naming syscall resolves the handle
first, and does nothing to any object until it has.** Resolve returns the object pointer or
`nullptr`. On `nullptr` the syscall returns `-1` and **never touches an object** -- no
queue is linked, no counter moved, no memory dereferenced.

You can already read this shape in today's `KOS_SYS_sem_wait`
(`kernel/syscall/syscall.cc`):

```
IrqLock lock;
Semaphore* s = sem_resolve(a0);
if (s == nullptr)
{
    return -1;               // bad handle: object untouched
}
sem_wait(s);                 // use, under the SAME lock
return 0;
```

Two properties make this a *chokepoint* and not merely a check:

1. **It is the only door.** There is no path from a syscall argument to an object pointer
   that bypasses resolve. Add rights, add types, add object kinds -- they are all enforced
   inside this one function (`sem_resolve` today; `cap_resolve` under M3), so there is one
   place to get right and one place to audit.
2. **Resolve and use happen under the same continuous `IrqLock`.** The pointer resolve
   hands back is only valid while the lock is held; releasing it between resolve and use
   would let a concurrent close/destroy free the slot underneath a validated pointer. The
   comment on `KOS_SYS_sem_wait` states exactly this invariant, and it is a *precondition*
   of resolve under M3: the caller holds the lock and uses the result under it.

## WRAP the global pools, do not replace them

Under M3 the per-task table does **not** point straight at a `Semaphore*` or own the
object. It wraps: a cap entry stores a *global object handle*, and resolve is two-level --
validate the cap entry in this task's table, then hand its stored global handle to the
unchanged global `SlotPool`, which re-validates in the usual way.

The invariant that forces this is worth stating plainly, because conflating its two halves
is the design error:

> **Object liveness is a GLOBAL property. Capability possession is a PER-TASK property.**

A semaphore shared by three tasks has *one* liveness fact and *one* refcount. Each task has
its *own* named, rights-scoped reference to it. If the cap pointed straight at the object
(REPLACE), the object's liveness would have to live inside a per-task table -- but no single
per-task table owns a shared object, so you would either lose the generation guard or
duplicate liveness N ways. WRAP keeps the single global liveness authority (`SlotPool`,
untouched) and gets its ABA generation guard for free. Two independent guards fall out, and
each catches a different mistake:

- **cap-gen** (per-task, in the table slot): catches use-after-**close**. The task closed
  cap 3, the slot was reused for a different object, and the old handle value must not
  resolve.
- **object-gen** (global, in the `SlotPool`): catches use-after-**destroy**. The existing
  guard, verbatim.

## The key insight: the detector has two consumers with different stakes

Here is the crux of the whole mechanism, and the thing most easily gotten wrong. The `-1`
that resolve produces on a bad handle is consumed by **two** parties, and they depend on it
in completely different ways.

**Consumer 1 -- kernel integrity. Unconditional. Load-bearing.** Resolve gates every
dereference the kernel is about to perform. A stale, forged, wrong-type, or
insufficient-rights handle can **never** cause the kernel to operate on the wrong object:
never link a TCB onto a wait queue it does not belong on, never dereference freed memory,
never reach an object the caller holds no cap to. This holds *regardless of what userspace
does next*. The kernel returned `-1` and touched nothing; the kernel is fine whether the
caller checks the value, ignores it, or sets it on fire.

**Consumer 2 -- application correctness. Conditional. Only if the app checks.** The same
`-1` also tells the *app* "the handle you named is bad." A correct app branches on it. A
buggy app that ignores it misbehaves -- but, crucially, only within its **own** authority,
because the handle only ever named the app's own objects.

The cautionary example is real and shaped the M3 design. Suppose a worker calls a blocking
wait, ignores the return value, and proceeds:

```
kos_sem_wait(h);   // returns -1 because h did not resolve in THIS task
// ... worker runs on, believing it blocked, but it never did
```

If `h` failed to resolve, the wait did nothing and the worker runs **unblocked** -- a
silent race, entirely inside the app's own logic. The kernel is not confused for a moment;
the *app* is. Under KickOS's original plan this was easy to trigger, because the app fleet
shared semaphores through file-scope global integers (a child using main's handle value),
which resolve to `CAP_EMPTY` in a per-task table. The design's fix is deterministic
placement: a fresh table has cap-gen 0 in every slot, so `handle == index`, and delegated
caps land at well-known indices the child knows a priori -- no discovery, no shared global.
The point for this chapter: the hazard is an *app-correctness* hazard, and the design
addresses it at the app/ABI layer. Kernel integrity was never in question either way.

## Separate the boundary from the detector

This is the payoff. Look again at what resolve checks, and split it into two piles that do
genuinely different jobs:

**The capability BOUNDARY (kernel-integrity gate).** Bounds (is the index in range?),
liveness (is the entry non-empty?), type (is it the object kind the syscall expects?), and
**rights** (does the entry permit this operation?). These four decide whether the kernel is
*allowed* to reach this object at all. They are load-bearing, and they are **completely
independent of any generation counter.** A zero-width generation would not weaken the
boundary one bit; rights and per-task scoping are what confine authority.

**The use-after-free DETECTOR (defense in depth).** The generation counters -- cap-gen and
object-gen -- catch a handle that *would have resolved* to a slot that has since been
recycled. This is a **bug detector**, not a boundary. It is what turns a use-after-close
from silent aliasing into a clean `-1`.

Confusing these two is the classic error: treating generation *width* as if it were the
strength of the isolation boundary. It is not. Widening the generation makes the detector
miss fewer bugs; it does nothing for the boundary, because the boundary was never made of
generation bits.

## ABA, the pigeonhole, and what "proper" means

Why is generation only a detector? Because **no finite scheme that reuses storage can
never alias.** Pigeonhole: a fixed number of slots plus a fixed-width handle can encode
only finitely many `(index, generation)` pairs, so an unbounded sequence of
allocate/free cycles must eventually repeat one. A repeated pair is a stale handle that
resolves to a fresh object -- the ABA problem. You cannot design it away; you can only push
the wrap point past the system's operational lifetime and ensure that a wrap, if it ever
happened, breaches nothing.

The clarifying anchor is the humble POSIX file descriptor. An fd has **no generation at
all**. `close(3); open(...)` returns `3` again, and a stale `3` held by some forgotten
code silently reads the new file. Every mainstream OS ships this, every day, and it is
considered fine -- because an fd is a *per-process* name, so a stale fd can only confuse a
buggy process about its *own* files. It cannot forge authority or reach another process.

KickOS's per-task cap table is the same containment plus a generation guard the fd lacks --
so it is **strictly stronger than a file descriptor.** A wrapped stale cap handle indexes
the *same task's* table and can, at worst, resolve to a different object *that task
legitimately holds right now*. It cannot forge authority, cannot cross to another task,
cannot escalate. So "proper" here means precisely two things, and generation width is
relevant to only the first:

1. **Cannot wrap within the system's operational lifetime** -- push the ABA window out
   far enough that a reuse cycle never completes in practice.
2. **Breaches nothing if it did** -- guaranteed by the per-task scope and rights boundary,
   independent of any counter.

## The width choice, concretely

KickOS uses a **16-bit** cap generation. That is at parity with the object pool's existing
`uint16_t` guard (`slotpool.h`), and it costs **zero extra bytes** -- it fits in what would
otherwise be padding in the 8-byte `CapEntry`. Sixteen bits gives a 2^16 window per slot
before a self-inflicted ABA could recur, which comfortably clears bar (1) for an MCU's
operational lifetime; bar (2) holds by construction regardless.

The theoretical no-wrap upgrade, if it is ever wanted, is a **single global monotonic
birth-stamp**: a 32- or 64-bit allocation counter stamped into each object when it is
born, with the handle carrying the stamp. A monotonic counter of sufficient width does not
recur within any real lifetime. It is deferred to a future codec-unification pass and is
gated by a **handle-width decision** -- a full 32-bit stamp cannot coexist with index bits
inside a 32-bit handle without going to a 64-bit handle, and that is a cross-arch ABI
change, not a tweak.

One tempting "fix" is explicitly **not** a solution: per-slot **retirement** -- leaking a
slot once its generation is about to wrap. On a fixed pool that just bleeds the pool: every
retired slot is capacity gone forever, and a long-running system starves. Retirement trades
a probabilistic detector miss for a guaranteed resource leak. It is not on the table.

## Lifecycle: refcount and destroy-on-last-close

Because possession is per-task but the object is shared, the refcount is a **global**
property of the object slot, not of any cap. Under M3 the userspace-facing lifecycle op
shifts meaning:

- **Today:** `sem_destroy(handle)` means "destroy this semaphore now" (quiescent-only).
- **M3:** close means "drop **my** handle" -- decrement the object's refcount and empty
  this task's entry. The object frees only at the **last** close, when the refcount hits
  zero. Closing your cap while another task still holds one relinquishes only your own
  name; it does not destroy the object.

This is Zircon's `zx_handle_close` semantics, and it makes close the single lifecycle op
for every cap type (a cap knows its own type). It is a genuine behavioral shift, not just
renamed plumbing -- see `../reference/architecture.md` ("Object model, capabilities & IPC")
and `kernel/syscall/cap.cc` for the refcount, the leak-don't-strand rule for parked waiters,
and the exit-teardown path.

## Low barrier: no capability manifest (the anti-CapDL discipline)

A word on usability, because it is a hard constraint (see the Book's README and Chapter 1).
seL4-style systems make you author a capability graph up front (CapDL). KickOS deliberately
does not. At spawn, the runtime wires a **sane default cap set** into the child's table in
kernel code; a plain app writes **no** capability manifest and needs none. Explicit
delegation is available (a parent hands a child specific caps, with rights narrowed, never
widened), deterministic, and never required. The usability benchmark stays *write a `main`,
that's it.*

## The ISR fail-safe variant

There is one place the chokepoint runs where returning `-1` is impossible: an interrupt
handler. Today `irq_sem_post` (`kernel/syscall/syscall.cc`) re-resolves the stored handle
on **every fire** from ISR context, and on `nullptr` it **silently drops the post**:

```
void irq_sem_post(void* arg)
{
    int handle = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    Semaphore* s = sem_resolve(handle);
    if (s != nullptr)
    {
        sem_post(s);
    }
    // nullptr: drop the post. Never post to a WRONG object.
}
```

This is the same detector, used where an error return has nowhere to go. A torn-down
binding (its semaphore since freed) degrades to **"no post"** -- never **"wrong post."**
The kernel would rather miss a wakeup than link a fresh, unrelated object onto an interrupt
that a stale handle happens to alias. Under M3 the binding stores the resolved **global**
handle (an ISR never resolves a *cap* -- `current()` in interrupt context is some random
interrupted thread's table), so `irq_attach` resolves the cap once at attach time, requires
`CAP_SIGNAL`, and stores the global handle. The ISR keeps today's fail-safe behavior
verbatim (design record section 4, finding M5).

## The transferable rule

Name objects per-task, not globally, so a task can only mean what it was given. Funnel
every use through one resolve that validates before it dereferences, and use the result
under the lock that resolve required. Then keep two ideas apart in your head:

- **bounds + liveness + type + rights = the boundary.** It keeps the *kernel* correct, it
  is unconditional, and it owes nothing to any counter.
- **generation = the detector.** It helps an *app* catch its own use-after-close, it is
  probabilistic under pigeonhole ABA, and widening it only lowers the odds of a missed,
  self-inflicted, single-task-scoped alias.

A file descriptor ships with the boundary and *no* detector, and the world runs on it.
KickOS ships both -- so the day a generation wraps, the worst case is one buggy task
confusing two of its own objects, and the kernel never so much as blinks.

## Where to go next

- The exact object model, the rights/refcount contract, and the B1 delegation ABI:
  `../reference/architecture.md` ("Object model, capabilities & IPC").
- The generational pool the guard rides on, and its ABA mechanics: `slotpool.h`.
- The chokepoint as it exists today: `sem_resolve` / `KOS_SYS_sem_wait` in
  `kernel/syscall/syscall.cc`.
- The isolation this completes on the memory side: Chapter 7, *Memory protection (M2)*.
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (the protection boundary)
  and the capability-systems literature (Dennis and Van Horn; the seL4 and Zircon handle
  models) for the lineage of per-task, rights-bearing object references.
