<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Naming a kernel object: the handle and the resolve chokepoint

> This chapter teaches the per-thread capability handle mechanism -- the *concept* and
> KickOS's design reasoning. For the exact, current object model, link to the code-synced
> Reference: `../reference/architecture.md` ("Object model, capabilities & IPC") and the
> code it describes -- `kernel/include/kickos/cap.h`, `kernel/syscall/cap.cc`,
> `kernel/syscall/syscall.cc`, `kernel/include/kickos/slotpool.h`.

A microkernel spends its life doing things to objects on behalf of threads that cannot
touch those objects directly: wait on *this* semaphore, post to *that* one, send on *that*
endpoint. So the very first question the kernel must answer, on every such syscall, is:
**which object does this caller mean, and is it allowed to mean it?** Everything in this
chapter is about the one place that question gets answered -- the *resolve chokepoint* --
and about the sharp, easily-missed distinction between the part of that answer that keeps
the kernel correct and the part that merely helps an app find its own bugs.

## The problem with a global integer id

The simplest scheme that satisfies "no kernel pointer crosses the boundary" is a single
kernel-wide pool of objects, named by an index into it. It is opaque, it is bounded, and it
is checkable -- and it is **ambient**. The name is global, so any thread that can guess or
forge the integer can name the object: `kos_sem_wait(4)` from anywhere reaches semaphore
4. Isolation says a caller should touch only what it has been *given*; a global namespace
anyone can enumerate is the opposite -- it is ambient authority, and ambient authority
contradicts the isolation the MPU chapters (Chapter 7) work so hard to build. Fencing a
caller's *memory* while leaving the *object namespace* open would be half a boundary.

## What a handle is

The answer is a **capability handle**: a per-thread, typed, rights-bearing, refcounted
reference to a kernel object.

- **Per-thread.** The number `3` is not a global object id; it is an index into *this
  thread's* table. One thread's `3` and another's `3` are unrelated. A thread can only name
  objects its own table holds an entry for -- it cannot forge a name for an object it was
  never given. Read *thread* strictly here, because KickOS also has a `Task`, a group of
  threads sharing one memory domain, and the two scopes deliberately do not coincide: two
  threads in one task share every byte of their granted memory and still hold entirely
  separate capability tables. Memory is the thing a group is formed to share; the right to
  name a kernel object is not, and it is handed to a thread at spawn.
- **Typed.** An entry records what kind of object it names (semaphore, PI mutex, IPC
  endpoint, interrupt binding, reply). A handle to a semaphore cannot be used where an
  endpoint is expected.
- **Rights-bearing.** An entry carries a small set of rights bits (WAIT, SIGNAL,
  TRANSFER). Holding a handle is not blanket authority over the object; it is exactly the
  operations the rights permit.
- **Refcounted.** Many threads may hold handles to one shared object; the object's lifetime
  is governed by how many handles name it, not by any single holder (see *Lifecycle*).

The shape is Zircon's `zx_handle_t` more than seL4's CNodes -- a small flat array embedded
in the TCB, no capability-graph boot manifest. That the array lives in the *thread* control
block is not an implementation detail to skim past; it is the scope, spelled in the layout.
The entry a table holds carries the four properties above:

```
struct CapEntry
{
    int32_t obj;                    // the GLOBAL generational object handle this cap names
    uint8_t type   : KCAP_TYPE_BITS;   // CapType -- checked at resolve
    uint8_t seq_lo : KCAP_REPLY_SEQ_LO_BITS;
    uint8_t rights : KCAP_RIGHTS_BITS; // WAIT / SIGNAL at resolve; TRANSFER at the delegate site
    uint8_t seq_hi : KCAP_REPLY_SEQ_HI_BITS;
    uint16_t gen;                   // per-slot cap generation, bumped on close
};
```

Read the four properties, then read the two fields that are not any of them. Type and
rights each need three bits and are given a byte each, and the five and three bits left
over are not padding: one capability type, the one-shot reply capability of Chapter 8.5,
needs somewhere to keep the call sequence that tells a late reply from a live one, and
its `obj` field is already full to the last bit with a thread handle. Those spare bits
are where that sequence goes. Chapter 8.7 works the case through, because the general
move is worth having: when a field is exactly full, look for unspent bits in its
neighbours before concluding that the width is the constraint. For every other capability
type the two are simply unused, which is why the entry still reads as the four properties
and eight bytes total.

### The handle a thread holds, and how it gets there

The handle itself is an **unsigned 32-bit** word (`kos_cap_t`), and every bit of it is
spent: the low `KCAP_INDEX_BITS` are the index into the thread's table, the high
`KCAP_GEN_BITS` are the generation, and the split is fixed fleet-wide rather than derived
from a board's table size, so the same logical capability prints the same value on every
target.

That it is *unsigned*, with no sign bit held back, is a consequence of how the handle
travels, and the two facts have to be read together. A minting syscall does not return the
handle: it returns a **status** -- 0 or a negated error code -- and writes the handle
through an **out-parameter**, as in `kos_sem_create(int initial, kos_cap_t* out_cap)`
(`user/include/kickos/sys.h`). With failure carried in the return value, the handle never
has to reserve a bit to signal it, so a live handle may have bit 31 set and `h < 0` is *not*
an error test on one. Chapter 8.7 works through what that extra bit is worth.

It does cost something, though, and this is the part worth internalising: once every bit
pattern can be a live handle, a sentinel can no longer be "a negative number". "No
capability" -- what a full table refuses with, and what a message arrival carries when there
is no reply capability to hand over -- has to be a *value the codec provably cannot mint*.
That is bought with a capacity rule rather than with arithmetic: one index value (all ones)
is reserved and never seated as a table slot, so every word carrying it as its index is
unmintable at any index/generation split. `KOS_CAP_NONE` and the authority pseudo-handle
`KOS_CAP_AUTHORITY` both stand on that one property (`kernel/include/kickos/cap.h`). The
general shape recurs whenever a value space is widened to its limit: the room for
out-of-band answers has to be carved out deliberately, because it is no longer left over.

## The resolve chokepoint: validate, then use, under one lock

The load-bearing discipline is that **every object-naming syscall resolves the handle
first, and does nothing to any object until it has.** Resolve returns the object pointer or
`nullptr`. On `nullptr` the syscall returns a negated error code and **never touches an
object** -- no queue is linked, no counter moved, no memory dereferenced. The refusal is
also classified, because the two ways to fail are not the same news for the caller:
`cap_resolve_e` reports `KOS_EBADF` for a handle that does not name a live object of the
right type, and `KOS_EPERM` for one that names it but lacks a required right
(`kernel/syscall/cap.cc`).

The shape, as `KOS_SYS_SEM_WAIT` has it (`kernel/syscall/syscall.cc`):

```
IrqLock lock;
int err = 0;
Semaphore* s = static_cast<Semaphore*>(
    cap_resolve_e(sched::current(), handle, CapType::CAP_SEM, CAP_WAIT, &err));
if (s == nullptr)
{
    return -err;             // EBADF or EPERM: object untouched
}
sem_wait(s);                 // use, under the SAME lock
return 0;
```

Two properties make this a *chokepoint* and not merely a check:

1. **It is the door.** Add rights, add types, add object kinds: they are enforced inside
   this one function, so there is one place to get right and one place to audit. A right
   that names an *operation on the object*, WAIT or SIGNAL, is checked at resolve, and a
   syscall arm that wants an object pointer has no other way to obtain one.

   TRANSFER is the exception that proves the rule rather than a wart in it. It does not
   name an operation on the object at all; it names permission for the *entry* to be
   copied into a child's table. Resolve never performs that copy, so there is nothing at
   resolve for a TRANSFER check to gate -- no call site asks resolve for it -- and the
   check lives where the copy happens, in the spawn path's grant loop. Read the rule
   precisely and the two are one rule: **a right is checked at the chokepoint for the
   operation it names.** Every right whose operation is "reach this object" funnels
   through resolve; a right whose operation is "duplicate this name" has its own single
   site. Getting this backwards -- adding a TRANSFER test to resolve for symmetry's sake --
   would put a check on a path that can never violate it, which is how a field starts
   being checked twice and enforced nowhere.

   One arm does open-code the sequence, and it is worth knowing which and why, because a
   reader who believes the check exists in exactly one function will not think to audit
   it. Attaching an interrupt line to a semaphore does not want the semaphore *pointer*:
   an ISR may never resolve a capability (the last section of this chapter says why), so
   the binding has to store the semaphore's **global** handle, which resolve does not hand
   back. So that arm looks the entry up directly and then performs the same checks in the
   same order under the same lock: live entry, right type, live object, then
   `CAP_SIGNAL` or `KOS_EPERM`. That is the property to hold onto. The chokepoint is a
   *sequence* that nothing is allowed to skip, and the single function is how it is
   normally reached, not the whole of the rule. An arm that needs the entry rather than
   the object owes the reader the same steps, spelled out, and owes the auditor a reason
   to look at it.
2. **Resolve and use happen under the same continuous `IrqLock`.** The pointer resolve
   hands back is only valid while the lock is held; releasing it between resolve and use
   would let a concurrent close/destroy free the slot underneath a validated pointer. This
   is a *precondition* of resolve, not a courtesy: the caller holds the lock and uses the
   result under it.

## WRAP the global pools, do not replace them

The per-thread table does **not** point straight at a `Semaphore*` or own the object. It
wraps: a cap entry stores a *global object handle*, and resolve is therefore **two-level**.
Level one validates the cap entry in this thread's table -- index within the thread's own run,
entry non-empty, cap generation matching, type as expected, rights sufficient (`cap_lookup`,
then the type and rights tests in `cap_resolve_e`). Level two hands the entry's stored
global handle to the object pool, which re-validates it in its own terms -- index in range,
slot still in use, object generation matching (`kernel/include/kickos/slotpool.h`). Either
level can refuse, and a refusal at either one produces the same untouched object and a
negated error.

Two levels, and not one, because they answer two different questions: level one asks *may
this thread name this?*, level two asks *is the thing still there?* A recycled table slot can
hold an entry naming a perfectly healthy object -- level one catches that and level two
would wave it through. The converse, a live capability naming a destroyed object, is what
level two is for, and it is unreachable for a reason that lives nowhere near
resolve: a holder's own capability *pins* a reference, so while any holder's entry names an
object the refcount cannot reach zero. That is a property of the lifecycle discipline below,
not of the resolve, and level two is what stops the resolve from depending on it.

The invariant that forces this is worth stating plainly, because conflating its two halves
is the design error:

> **Object liveness is a GLOBAL property. Capability possession is a PER-THREAD property.**

A semaphore shared by three threads has *one* liveness fact and *one* refcount. Each thread
has its *own* named, rights-scoped reference to it. If the cap pointed straight at the object
(REPLACE), the object's liveness would have to live inside a per-thread table -- but no single
per-thread table owns a shared object, so you would either lose the generation guard or
duplicate liveness N ways. WRAP keeps the single global liveness authority (`SlotPool`,
untouched) and gets its ABA generation guard for free. Two independent guards fall out, and
each catches a different mistake:

- **cap-gen** (per-thread, in the table slot): catches use-after-**close**. The thread closed
  cap 3, the slot was reused for a different object, and the old handle value must not
  resolve.
- **object-gen** (global, in the `SlotPool`): catches use-after-**destroy**. The existing
  guard, verbatim.

## The key insight: the detector has two consumers with different stakes

Here is the crux of the whole mechanism, and the thing most easily gotten wrong. The refusal
resolve produces on a bad handle is consumed by **two** parties, and they depend on it in
completely different ways.

**Consumer 1 -- kernel integrity. Unconditional. Load-bearing.** Resolve gates every
dereference the kernel is about to perform. A stale, forged, wrong-type, or
insufficient-rights handle can **never** cause the kernel to operate on the wrong object:
never link a TCB onto a wait queue it does not belong on, never dereference freed memory,
never reach an object the caller holds no cap to. This holds *regardless of what userspace
does next*. The kernel returned an error and touched nothing; the kernel is fine whether the
caller checks the value, ignores it, or sets it on fire.

**Consumer 2 -- application correctness. Conditional. Only if the app checks.** The same
error code also tells the *app* "the handle you named is bad." A correct app branches on it.
A buggy app that ignores it misbehaves -- but, crucially, only within its **own** authority,
because the handle only ever named the app's own objects.

The cautionary example is worth spelling out. Suppose a worker calls a blocking wait,
ignores the return value, and proceeds:

```
kos_sem_wait(h);   // returns -KOS_EBADF because h did not resolve in THIS thread
// ... worker runs on, believing it blocked, but it never did
```

If `h` failed to resolve, the wait did nothing and the worker runs **unblocked** -- a
silent race, entirely inside the app's own logic. The kernel is not confused for a moment;
the *app* is. The way this bug arrives in practice is an app sharing a handle *value*
through a file-scope global -- a child reading the integer main happened to get -- which in
the child's own table names an empty slot. Note that grouping the two threads into one task
does not rescue it: they share the global, they share the memory the global sits in, and
they still do not share the table the value indexes. What removes the temptation is
deterministic placement: a fresh table carries generation 0 in every slot, so `handle ==
index` there, and delegated caps land at indices the child knows a priori -- no discovery,
no shared global. The point for this chapter: the hazard is an *app-correctness* hazard, and
it is addressed at the app/ABI layer. Kernel integrity was never in question either way.

## Separate the boundary from the detector

This is the payoff. Look again at what resolve checks, and split it into two piles that do
genuinely different jobs:

**The capability BOUNDARY (kernel-integrity gate).** Bounds (is the index in range?),
liveness (is the entry non-empty?), type (is it the object kind the syscall expects?), and
**rights** (does the entry permit this operation?). These four decide whether the kernel is
*allowed* to reach this object at all. They are load-bearing, and they are **completely
independent of any generation counter.** A zero-width generation would not weaken the
boundary one bit; rights and per-thread scoping are what confine authority.

**The use-after-free DETECTOR (defense in depth).** The generation counters -- cap-gen and
object-gen -- catch a handle that *would have resolved* to a slot that has since been
recycled. This is a **bug detector**, not a boundary. It is what turns a use-after-close
from silent aliasing into a clean refusal.

Confusing these two is the classic error: treating generation *width* as if it were the
strength of the isolation boundary. It is not. Widening the generation makes the detector
miss fewer bugs; it does nothing for the boundary, because the boundary was never made of
generation bits.

## ABA, the pigeonhole, and what "proper" means

Why is generation only a detector? Because **no finite scheme that reuses storage can
never alias.** Pigeonhole: a fixed number of slots plus a fixed-width handle can encode
only finitely many `(index, generation)` pairs, so an unbounded sequence of
allocate/free cycles must eventually repeat one. A repeated pair is a stale handle that
resolves to a fresh object -- the ABA problem. You cannot design it away; you can only make a
wrap unlikely per erroneous use, and ensure that a wrap, when it happens, breaches nothing.

The clarifying anchor is the humble POSIX file descriptor. An fd has **no generation at
all**. `close(3); open(...)` returns `3` again, and a stale `3` held by some forgotten
code silently reads the new file. Every mainstream OS ships this, every day, and it is
considered fine -- because an fd is a *per-process* name, so a stale fd can only confuse a
buggy process about its *own* files. It cannot forge authority or reach another process.

KickOS's per-thread cap table is the same containment plus a generation guard the fd lacks,
scoped one level finer than the process, so it is **strictly stronger than a file
descriptor.** A wrapped stale cap handle indexes the *same thread's* table and can, at
worst, resolve to a different object *that thread legitimately holds right now*. It cannot
forge authority, cannot cross to another thread, cannot escalate. So "proper" here means
precisely two things, and generation width is
relevant to only the first:

1. **A wrap is improbable per erroneous use** -- a stale handle escapes only if the slot's
   counter has come all the way round to the value that handle carries, so a g-bit
   generation misses with probability `2^-g`.
2. **Breaches nothing if it does** -- guaranteed by the per-thread scope and rights boundary,
   independent of any counter.

Note what bar (1) is *not*. It is not "cannot wrap within the device's service life". That
sounds like the natural criterion and it is not purchasable: a recycle is a couple of
syscalls, so the counter can be driven around its whole cycle in seconds of dedicated churn
regardless of how wide it is made, and no width a 32-bit handle can afford changes that.
Chapter 8.7 works the arithmetic and shows why the escape probability is the criterion that
survives it -- which is also why the honest form of a width decision is a probability you
state, never the word "safe".

## The width choice, concretely

The cap generation is **16 bits**, at parity with the object pool's `uint16_t` guard
(`kernel/include/kickos/slotpool.h`), and it costs **zero extra bytes** -- it occupies what
would otherwise be padding in the 8-byte `CapEntry`. Sixteen bits means one erroneous use in
65536 aliases instead of failing; bar (2) holds by construction regardless of that number.
The field width and the storage width are deliberately equal, because the two sites that
bump the counter do so *unmasked*: one bit narrower and the counter would overflow past what
the codec can encode, and the slot would mint handles that could never resolve.

The upgrade that would actually retire the wrap, rather than lengthening it, is a **single
global monotonic birth-stamp**: one allocation counter stamped into each object at birth,
with the handle carrying the stamp. A monotonic counter never repeats a value, so there is no
cycle to come around. What it costs is the thing that makes it a real decision rather than a
free win: a stamp wide enough to be monotonic for a device lifetime cannot share a 32-bit
word with an index field, so it forces a 64-bit handle -- a fleet-wide ABI change, not a
tweak. That is the trade a design either pays for or declines; it is not a detail.

One tempting "fix" is explicitly **not** a solution: per-slot **retirement** -- leaking a
slot once its generation is about to wrap. On a fixed pool that just bleeds the pool: every
retired slot is capacity gone forever, and a long-running system starves. Retirement trades
a probabilistic detector miss for a guaranteed resource leak.

## Lifecycle: refcount and destroy-on-last-close

Because possession is per-thread but the object is shared, the refcount is a **global**
property of the object slot, not of any cap. That forces a choice about what the
userspace-facing lifecycle call *means*, and the two readings are not compatible:

- **Destroy** -- "tear this object down now." The caller speaks for the object. Whoever else
  holds a name for it finds that name pointing at nothing, so destroy is only safe while the
  object is quiescent, and "quiescent" is a property no single holder can check.
- **Close** -- "drop **my** name for it." The caller speaks only for itself: decrement the
  refcount, empty this thread's entry, and free the object only at the **last** close, when
  the count reaches zero. Closing while another thread still holds a cap relinquishes your
  own name and nothing more.

Close is the one that composes, and it is what a per-thread table implies: if possession is
per-thread, then relinquishing possession must be per-thread too. Destroy would let any holder
invalidate every other holder's name -- ambient authority reintroduced through the lifecycle
door, after the handle table was introduced to remove it. So `kos_handle_close` is the single
lifecycle op for every cap type, which works because a cap knows its own type; this is
Zircon's `zx_handle_close` semantics.

The one wrinkle worth knowing is a naming one: `kos_sem_destroy` still exists as a
source-compatibility **alias** of `kos_handle_close` (`user/include/kickos/sys.h`). The
spelling survived; the semantics above are what it does. A reader who assumes the name
means what it says will expect the object gone after the call, and it is not gone while
another holder remains.

See `../reference/architecture.md` ("Object model, capabilities & IPC") and
`kernel/syscall/cap.cc` for the refcount, the leak-don't-strand rule for parked waiters, and
the exit-teardown path.

## Low barrier: no capability manifest (the anti-CapDL discipline)

A word on usability, because it is a hard constraint (see the Book's README and Chapter 1).
seL4-style systems make you author a capability graph up front (CapDL). KickOS deliberately
does not. At spawn, the runtime wires a **sane default cap set** into the child's table in
kernel code; a plain app writes **no** capability manifest and needs none. Explicit
delegation is available (a parent hands a child specific caps, with rights narrowed, never
widened), deterministic, and never required. The usability benchmark stays *write a `main`,
that's it.*

## Where the chokepoint cannot run: the ISR

There is one context in which the whole discipline above is unavailable, and it is
instructive precisely because it is. An interrupt handler has nowhere to return an error to,
and -- more decisively -- it cannot resolve a *capability* at all. A capability handle is an
index into the current thread's table, and in interrupt context the current thread is
merely whichever one the interrupt happened to land on. Resolving a stored cap handle there
would read a stranger's table.

So the cap resolve is hoisted out of ISR context in both of the shapes KickOS offers, and
comparing them is the point of this section, because they answer the same impossibility
differently.

**Resolve once, re-resolve the global per fire.** Binding a semaphore to a line
(`kernel/syscall/syscall.cc`) resolves the *capability* once at bind time, checks it carries
the signal right, and stores the **global object handle** in the binding. The ISR then
re-resolves that global against the object pool on every fire, and if the object is gone it
**silently drops the post**. The generation check still runs; only its failure branch
changes, because an interrupt has nowhere to return an error to. A torn-down binding degrades
to "no post", never "wrong post".

**Resolve once, carry the pointer.** Claiming an interrupt line as a capability
(`kernel/irq/irq.cc`) instead hands the first-level ISR the *address* of the binding it will
post to, so the ISR does no lookup at all: mask the line, post, return. That buys the ISR's
latency back, and it moves the hazard from a stale handle to a dangling pointer.

The pair is the lesson. A check you cannot perform must be replaced either by a cheaper
check whose failure direction you have chosen deliberately, or by a structure in which the
check is unnecessary -- never by a check performed badly. The second is faster and the first
needs no ordering discipline; which is right depends on whether you can afford the pool walk.

Carrying a pointer moves the hazard rather than deleting it, and the two halves of that
mechanism are what close it:

- **Detach before free.** The binding's slot cannot return to its pool while the dispatch
  table still holds its address, so releasing a binding restores the line's default handler
  and masks the line *first*, and only then frees the slot. The ordering is the entire
  guarantee; reversed, a firing line would post into a recycled slot.
- **A null object, not a null slot.** Every line's dispatch entry is always a valid
  function. A line with no driver holds a default handler that masks the line and counts the
  event, so an interrupt arriving on an unowned line cannot dereference anything and cannot
  be silently lost either.

The lesson generalises past interrupts: a torn-down binding degrades to **"no post"** and
never to **"wrong post."** The kernel would rather miss a wakeup than link a fresh, unrelated
object onto an interrupt that a stale name happens to alias. Where an error return has
nowhere to go, pick the failure direction deliberately and make it the only one reachable.

## The transferable rule

Name objects per-thread, not globally, so a thread can only mean what it was given. Funnel
every use through one resolve that validates before it dereferences, and use the result
under the lock that resolve required. Then keep two ideas apart in your head:

- **bounds + liveness + type + rights = the boundary.** It keeps the *kernel* correct, it
  is unconditional, and it owes nothing to any counter.
- **generation = the detector.** It helps an *app* catch its own use-after-close, it is
  probabilistic under pigeonhole ABA, and widening it only lowers the odds of a missed,
  self-inflicted, single-thread-scoped alias.

Then resolve in the layers the facts come in. Capability liveness and object liveness are two
different facts about two different things, so the resolve that checks them is two-level, and
the level that answers *may this thread name it* cannot also answer *is it still there*.

A file descriptor ships with the boundary and *no* detector, and the world runs on it.
KickOS ships both -- so the day a generation wraps, the worst case is one buggy thread
confusing two of its own objects, and the kernel never so much as blinks.

## Where to go next

- The exact object model, the rights/refcount contract, and the delegation ABI:
  `../reference/architecture.md` ("Object model, capabilities & IPC").
- The generational pool level two rides on, and its ABA mechanics:
  `kernel/include/kickos/slotpool.h`.
- The chokepoint itself: `cap_lookup` / `cap_resolve_e` in `kernel/syscall/cap.cc`, and a
  caller of it in `KOS_SYS_SEM_WAIT` (`kernel/syscall/syscall.cc`).
- What the generation is worth, why its width is not the criterion it looks like, and why the
  three counters in the system do not share a bit budget: Chapter 8.7,
  *[A stale handle must not be usable](stale-handles-generation-width-and-allocation-policy.md)*.
- The isolation this completes on the memory side: Chapter 7, *Memory protection*.
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (the protection boundary)
  and the capability-systems literature (Dennis and Van Horn; the seL4 and Zircon handle
  models) for the lineage of scoped, rights-bearing object references.
