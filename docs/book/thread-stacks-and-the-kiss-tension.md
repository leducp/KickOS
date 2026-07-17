<!-- SPDX-License-Identifier: CECILL-C -->
# Thread stacks: who owns the memory?

> **Status: DRAFT.** A worked example of a design where BOTH trivial answers are
> wrong -- one burns RAM, the other leaks it -- and the "proper" answer risks
> over-engineering. The lesson is how KickOS keeps it simple: constrain the problem
> until the correct solution *is* the simple one. Points into
> `../reference/architecture.md` (Object model / Memory domains) for the contract.

## The problem

Every thread needs a stack: a chunk of RAM for its call frames, locals, and (on a
trap) its saved context. A thread is cheap to *describe* -- a small TCB (Chapter 2)
-- but its stack is the real memory cost, kilobytes each. So the whole question of
"how expensive is a thread" is really "who owns the stack, and when is it freed?"

On a hosted OS this is a non-question: threads get stacks from a general heap backed
by virtual memory, paged in on demand, freed on exit. KickOS has neither a heap that
frees nor an MMU. Its RAM allocator is a **bump allocator** -- hand out the next
bytes, never take them back (Chapter 2). That single constraint is what makes stack
ownership interesting, because the obvious answers both fail.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (thread implementation)
and ch.3 (memory management -- why a general free-list allocator is the thing an RTOS
tries hardest to avoid).*

## Trivial answer #1: a static pool -- and why it burns RAM

The simplest thing that could work: give the kernel a fixed array,
`stacks[MAX_THREADS][STACK_SIZE]`, and hand slot *i* to the *i*-th thread. It is
deterministic (no allocation can fail at spawn), it reclaims (an exited thread's slot
is reused), and it is a dozen lines.

It is also **deadly on a small part**. The array is `MAX_THREADS * STACK_SIZE` of
RAM reserved *unconditionally* -- whether or not those threads ever exist. On a
16 KiB microcontroller a pool for 8 threads of 2 KiB is the entire chip. Worse, it
double-charges the careful user: a thread that brings *its own* stack (see below)
still owns its pool slot, sitting empty. You pay for the maximum you configured, not
the maximum you use -- the classic static-provisioning tax, and it is regressive: it
hurts most exactly where RAM is scarcest.

## Trivial answer #2: allocate on demand, never free -- and why it leaks

So flip it: don't pre-reserve anything. When a thread spawns without a stack, bump-
allocate `STACK_SIZE` from the arena. Zero waste for threads you never create.

But the arena never frees, so a thread that **exits** cannot return its stack. Every
spawn/exit cycle leaks `STACK_SIZE`. A server that occasionally spawns a worker and
lets it finish slowly drains the arena until nothing is left. And because spawning is
not a privileged operation, an *unprivileged* thread can do it in a loop -- a
denial-of-service that starves every other domain of RAM. "Simple" here bought a
security hole.

(There is a tempting patch -- let apps *reuse* a stack buffer across spawns -- that
is subtly unsafe on its own: a higher-priority thread waiting on the exiting worker
can preempt it *mid-exit* and start reusing the buffer while the outgoing context
save is still writing to it. Correct stack reuse needs the kernel's "the exited
thread is provably off-CPU" guarantee, which an app semaphore does not provide. See
the invariant `exit-parks-for-deferred-switch`.)

## The over-engineering trap

The textbook fix for "allocate and free arbitrary sizes without leaking" is a general
allocator: a free list with coalescing, size classes, the works. It would solve the
stack problem -- and drag in everything an RTOS spends effort *avoiding*:
fragmentation (so worst-case free memory becomes unpredictable), non-deterministic
allocation time, and a large, bug-prone piece of trusted code in the kernel. Reaching
for it here would trade a RAM bug for a complexity-and-determinism bug. Not KISS.

## The resolution: constrain the problem, not grow the solution

The key observation: kernel-default stacks are **all exactly one size**
(`USER_STACK_SIZE`). The hard parts of a general allocator -- variable sizes,
coalescing, fragmentation -- exist only because blocks differ. Remove that, and what
remains is trivial:

- A **single-size-class free list**. The free block *is* the list node: thread the
  `next` pointer through the dead stack's own memory. No metadata, no coalescing, no
  fragmentation (every block is interchangeable).
- **Allocate:** pop the free list; if empty, bump one fresh block from the arena. So
  the arena high-water rises only when demand exceeds every previously-freed block.
- **Reclaim:** push the block back -- but only at the moment the kernel *reclaims the
  thread slot*, which is the point where the scheduler already guarantees the exited
  thread is off-CPU (`exit-parks-for-deferred-switch`). Harvesting there, not at
  exit, is what makes writing the list pointer into the dead stack safe.

The result is what a caller intuitively expects:

```
spawn spawn spawn exit spawn
  1     2     3    3     3      <- arena high-water (peak concurrency, not total spawns)
```

You pay for the most threads *alive at once*, and never more. Nothing leaks (an
unprivileged spawn/exit loop just recycles the same block). Allocation is O(1) and
fragmentation-free. It is a few lines -- because the constraint (one size class) did
the work the general allocator's machinery would have.

The general freeing allocator is still the right tool for *arbitrary* allocations
(`ram_alloc`), and remains future work (M4). But the kernel does not need it to own
stacks well -- and shipping the small, scoped version instead of the big, general one
is the KISS move.

## And the escape hatch: userspace owns its stacks

The kernel owning stacks at all is itself a convenience, not a necessity. The only
stacks the kernel *must* own are its own two threads -- **idle** and **root/main** --
which are static, build-time-sized buffers. Every other stack is a userspace concern:
an app can hand `spawn` its own buffer and control the memory entirely. The
`KOS_STACK_DEFINE(name, size)` helper makes that correct-by-construction -- it aligns
the buffer so it is a valid MPU region under enforcement (a pow2 base, Chapter 7),
which a raw array would not be. Apps that want deterministic, self-owned memory use
it; apps that just want a thread use the convenient path and get the free-list.

## The lesson

Two simple designs, two opposite failures: static over-provisions, on-demand leaks.
The instinct to reach for a general allocator would have over-corrected into
complexity an RTOS can't afford. The way out was not a cleverer allocator but a
*smaller problem*: default stacks are one size, so the allocator for them is a
one-size free list -- simple *because* the problem was constrained. When a design
feels stuck between "too wasteful" and "too complex," the move is often to narrow
what the code must handle until the simple solution becomes the correct one.
