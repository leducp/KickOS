<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# A stale handle must not be usable: generations, width, and allocation policy

> Chapter 8.1 introduced the generation counter as the *detector* half of the resolve
> chokepoint and left it there. This chapter is the detector on its own terms: what a
> generation actually buys, why "how many bits?" is the wrong first question, why several
> counters at the same width are not several equal defenses, and why the *allocation
> policy* of the pool underneath turns out to be worth more than any of the bits. For the
> exact widths, the handle codec and the contract they enforce, link into
> `../reference/architecture.md` ("Object model, capabilities & IPC"),
> `../reference/invariants.md` (`handle-not-pointer-across-boundary`,
> `object-access-via-per-task-cap`), and the code they describe --
> `kernel/include/kickos/slotpool.h` and `kernel/include/kickos/cap.h`.

## The problem: a name that outlives the thing it named

No kernel pointer crosses the user/kernel boundary. A task names a kernel object with a
small integer that the kernel turns back into an object -- an index into a fixed pool. That
is the right design for a dozen reasons (bounded, checkable, forgeable only into the pool,
never a dangling dereference), and it comes with one structural hazard that indices cannot
avoid on their own.

Storage gets reused. Free the object in slot 3, allocate another, and slot 3 is live again
-- with a *different* object in it. Any copy of the old handle that is still lying around
now names a live stranger. Nothing is corrupted, nothing is dangling, every bounds check
passes: the handle points at a valid, live, correctly-typed object. It is simply not the
one the holder means.

Worse, the pools are monomorphic -- one pool per object kind -- so the stranger is
guaranteed to be the *same type* as the object that was freed. The type check at resolve,
which catches so much else, cannot see this at all.

### The interleaving, concretely

Take a semaphore pool, handles that are bare slot indices, and an allocator that hands back
the lowest free slot. A worker creates a semaphore, delegates it to a helper, and later
tears it down; an unrelated logger thread creates one of its own.

```
t0  worker  sem_create()            -> slot 3 claimed, handle = 3
t1  worker  delegates handle 3 to the helper, which stashes it in its own state
t2  worker  closes its own name for it; it was the last one, so slot 3 is freed
t3  logger  sem_create()            -> the lowest free slot is 3, handle = 3
t4  logger  begins using handle 3 for its own, unrelated semaphore
t5  helper  sem_wait(3)             -> resolves; the helper blocks on the LOGGER's semaphore
```

At t5 the helper is parked on a semaphore it was never given, that nobody will post for its
sake, and that the logger is meanwhile signalling for reasons of its own. No check fired.
This is **ABA**: slot 3 was A (the worker's semaphore), then B (free), then A again (the
logger's) -- and an observer holding only the index cannot distinguish the second A from the
first. The classic instance of it is the lock-free stack whose CAS succeeds on a pointer
that was popped and pushed back while the comparer was not looking; a slot pool is the same
shape with the pointer replaced by an index.

Note what the helper's bug actually was: it kept a handle past the lifetime it was granted.
That is a real bug and it stays a real bug. The question this chapter answers is what the
system does *when* that bug exists, which is always.

## The mechanism: version the slot, put the version in the name

The fix is to make the name carry a second component that changes when the storage is
reused. Each slot gets a monotonically increasing counter; freeing a slot bumps it; a handle
packs the counter alongside the index:

```
handle = (generation << index_bits) | index
```

Resolve splits the word, uses the index to find the slot, and then compares the handle's
generation against the slot's own. A match means the handle was minted for *this* occupancy
of the slot. A mismatch means the slot has been recycled since the handle was minted, and
resolve refuses -- the syscall returns an error and touches no object at all.

Replay the interleaving with this in place. At t2 slot 3's generation goes from 0 to 1. At
t3 the logger's handle is `(1 << index_bits) | 3`, a different integer entirely. At t5 the
helper's stale `3` carries generation 0, the slot holds generation 1, and the wait fails
cleanly instead of aliasing. The helper's bug became visible at the exact moment it was
committed, which is the whole point: **a stale handle now fails loudly rather than
succeeding wrongly.**

This is the shape KickOS uses for every pooled object. The pool, its packing, and the free
path that bumps the counter are in `kernel/include/kickos/slotpool.h`; the per-task
capability table's own codec, including the width arithmetic it is bound by, is in
`kernel/include/kickos/cap.h`. The exact contract is in the Reference -- do not infer it
from this chapter.

## The honest part: "how wide should the counter be?" is the wrong question

Here is the reflex the moment someone points at the counter: *make it wider*. Widen it
enough and wrap becomes impossible, and then the guard is a guarantee rather than a
heuristic. It is a very natural thought and it is wrong, and the arithmetic that shows why
takes about three lines. Work it, because everything downstream in this chapter depends on
having actually done it.

**How fast can a slot recycle?** One recycle is a close plus a create -- two syscalls, a
handful of list operations, no I/O and no blocking. On a 16 MHz microcontroller call it 20
microseconds. That is a deliberately unflattering estimate for the defender: a system doing
nothing but churning one slot.

**How many recycles in a service life?** Ten years is about 3.16e8 seconds. At 20
microseconds a recycle, that is about 1.6e13 recycles, or roughly **2^44**.

**How many bits are available?** A handle comes back to userspace through a syscall whose
negative returns are error codes, so the whole word must stay positive in a 32-bit int:
**31 bits**, to be split between the index field and the generation field. The index field
is not free -- it has to address every slot in the pool -- so the generation gets whatever
is left, and the left-over is strictly less than 31.

Now put the two together. Even if the index field were zero bits wide and the generation got
the entire 31, `2^31` recycles at 20 microseconds each is about **twelve hours**. Not twelve
years, not twelve months. Twelve hours of dedicated churn exhausts the widest counter the
handle word can physically hold.

So: **no affordable width makes wrap impossible.** Not the width you have, not the width you
were about to ask for, not the widest one the encoding permits. Any generation width
defended on the grounds that it is "wide enough for the device's service life" is
self-deception -- the sizing criterion being invoked is not available at any price the
handle word can pay.

### What a width does buy

Discard the wrong criterion and a real one is sitting right behind it. Ask not "will it
wrap?" (it will) but "**when a handle is used after it went stale, what is the chance the
guard misses?**"

A stale handle escapes detection only if the slot's generation has come all the way around
to the value the stale handle happens to carry. Over a long-running system with no
particular correlation between when a handle goes stale and when it gets misused, a
generation of g bits leaves an escape probability of `2^-g` per erroneous use:

| generation bits | erroneous uses caught | escape probability |
| --- | --- | --- |
| 0 | none | 1 |
| 8 | 99.61% | 1 in 256 |
| 16 | 99.9985% | 1 in 65536 |
| 24 | 99.999994% | 1 in 16.7 million |

That is a criterion you can actually apply. It is stated per *erroneous use*, not per year
and not per device, so it does not pretend to know your workload, your uptime, or your bug
rate. Pick a probability, write down why that probability is acceptable for the blast radius
in question, and never write the word "safe".

The reframing is the deliverable here: **the engineering act is choosing a probability and
stating it, not declaring a number safe.** A design document that says "16 bits, which is
plenty" has skipped the act. One that says "16 bits, so roughly one erroneous use in 65536
aliases instead of failing, and the worst such alias is bounded by X" has performed it.

## Not all generations defend the same thing

A system of any size ends up with more than one of these counters, at different layers, and
the second trap is treating them as interchangeable because they are the same mechanism at
the same width. They are not. Three questions separate them, and none of the three is about
bits:

1. **Who can hold a stale handle?** Only the task that made the mistake, or a *peer*?
2. **For how long?** Is retention bounded by something you control, or does the holder keep
   it as long as it likes?
3. **What does a wrap actually reach?** An object the holder already legitimately has, or
   one it was never given?

KickOS carries three such counters, and running the questions over them separates them
sharply even though they are declared at the same width.

**The per-task capability generation** (the one in a capability table slot) guards
use-after-close. Its stale handles live in exactly one place -- the table of the task that
closed the capability -- and resolution takes the rights from whatever entry is seated
*at that moment*. So a wrap hands that task an object it already holds a capability for, at the rights
that entry already carries. The failure is one task misdirecting itself among its own
objects. It is the counter with the *widest* reachability into buggy code and the
*narrowest* consequence.

**The object-pool generation** guards use-after-destroy, and it inverts that. Wrapping it is
rarer, because a pool slot only frees when the last holder closes and the refcount reaches
zero -- there is a real backstop making the free path infrequent. But the consequence, if it
ever bites, is genuinely cross-task: a handle resolving to an object a *different* task
created. The backstop bounds *when* a wrap can bite; it does nothing about *how badly*.

**The thread generation carried inside a reply capability** is the interesting one. A reply
capability names the parked caller by its generational thread handle. That handle is held by
the *server* -- a peer -- and it is held for as long as the server chooses to hold it. You
cannot make it drop the thing. Reachability is cross-task, retention is holder-controlled
and unbounded, and the consequence of a wrap is a reply delivered into a thread slot that
has since been recycled into an unrelated thread. Same width as the others, materially more
exposure than either.

The general lesson, which outlives these three particular counters: **reachability and
consequence can point in opposite directions, so the widest counter is not automatically the
one under the most pressure, and neither is the one that wraps most often.** Before sizing
anything, enumerate the holders and their retention. A counter guarding a mistake a task can
only make against itself is a fundamentally different object from one guarding a handle a
peer holds and that you cannot make it drop -- and if you size them by the same rule you
have sized at least one of them by accident.

This also disposes of a tempting economy. Narrowing a generation to reclaim bytes prices the
saving against *all* the counters that share the encoding, not just the one whose exposure
you were thinking about. The counter with the least visible failure mode is frequently the
one the narrowing hurts most.

## The payoff: allocation policy is part of the guard

Now the part that pays for the chapter, and the reason to have worked the arithmetic above
rather than skipping to a number.

Every analysis so far has quietly assumed something false: that recycles spread evenly over
the pool, so a pool of N slots gets N counters each advancing at 1/N of the total churn.
Whether that is true is decided by one line of code nobody thinks of as a security
parameter -- **which free slot the allocator hands back.**

The obvious allocator scans from the bottom and returns the lowest free slot. First-fit. It
is three lines, it has no state, and it is what almost everyone writes first. Look at what it
does to a steady create/destroy workload -- a task that holds one object at a time, or a
server minting a reply capability per request:

- Create: slot 0 is free, take slot 0.
- Destroy: slot 0's generation goes to 1.
- Create: slot 0 is free again, take slot 0.
- Destroy: slot 0's generation goes to 2.

Every recycle in the system lands on the same slot. Slot 0's counter is driven around its
entire cycle while slots 1 through N-1 sit at generation 0 forever, having never been touched
at all. **First-fit is the worst possible policy for a generation counter.** The pool's
effective wrap distance is not N times one slot's; it is exactly one slot's, and the other
N-1 counters are decoration. It is the pathological case, and it is also the default case,
which is a bad combination.

The fix is next-fit: keep a cursor at the slot the last allocation landed on, and resume
scanning from there rather than from zero. Now the same workload walks the pool -- slot 0,
then 1, then 2, wrapping around at N -- and each slot's counter advances once per lap
instead of once per recycle. The effective wrap distance is multiplied by roughly the pool
size. The cost is **one byte of cursor per pool**, no handle bits, no table bytes, no change
to the encoding, no change to the ABI, and no change to what resolve does.

Compare the two purchases honestly:

| move | cost | effect on effective wrap distance |
| --- | --- | --- |
| widen the generation by k bits | k bits in every handle, possibly a wider handle word and an ABI change | x 2^k, capped by the sign wall |
| first-fit -> next-fit | one byte per pool | x N (the pool size), free |

On a pool of a few dozen slots the policy change buys five bits of generation and does not
spend any. On the pools where the counter is under the most pressure -- the ones churning
one object at a time, which is precisely the workload first-fit degenerates on -- it buys the
most. **Policy bought what no amount of bit-packing could.**

The allocation policy is the part of the guard that lives outside the guard, which is exactly
why it gets missed: the counter is in the header, the packing is in the header, the
comparison is in resolve, and the thing that determines whether any of it works is in a
`for` loop in the allocator that reads like a triviality. The pool's `alloc` is in
`kernel/include/kickos/slotpool.h`; read it as part of the ABA guard, because it is.

The transferable version, worth carrying to problems that have nothing to do with handles:
**when a defense is a counter, find out what drives the counter before you widen it.** A
version field, a nonce, a sequence number, a retry budget -- each is only as strong as the
distribution of the thing incrementing it, and a skewed distribution can erase more bits than
you could plausibly add. The reflex is to widen the field; check the drive first, because
changing what feeds the counter is often cheaper and always more effective than making room
for more of it.

## Where a generation sits among the alternatives

A per-slot generation is one purchase among several, and it is worth knowing the others exist
-- not to rank them, but so that "add a generation" does not become the only thought
available. The design space that real systems occupy:

- **Randomise the handle value instead of sequencing it.** Zircon (Fuchsia) derives handle
  values from a per-process secret rather than from an index and a counter, so a handle is not
  guessable and a recycled slot does not produce a predictably-related value. This trades a
  deterministic detector for an unguessable name, and it needs a source of randomness.
- **Carry no per-slot generation at all, and revoke explicitly.** seL4 tracks derivations in a
  capability derivation tree, so revoking a capability walks the structure and invalidates
  every descendant. Staleness stops being a race to detect and becomes a state the kernel
  maintains -- at the cost of the tree, and of the recursive revoke that walks it.
- **Make a stale reference unforgeable in hardware.** CHERI attaches a validity tag to each
  capability-sized word in memory and registers; the hardware clears the tag on any operation
  that would fabricate a capability. Reachability becomes a hardware property rather than a
  check the kernel remembers to perform, and it requires the silicon.
- **Ship the threat class at width zero.** A POSIX file descriptor has no generation
  whatsoever. `close(3)` then `open(...)` returns `3` again, and a stale `3` held by forgotten
  code reads the new file in silence. Every mainstream system does this, every day. What makes
  it tolerable is not a counter -- it is that an fd is a *per-process* name, so a stale one can
  only confuse a buggy process about its own files.

That last one is the calibration for everything above. A generation counter is a **detector**;
the thing keeping a wrap from being a security event is the **boundary** -- per-task naming,
type, and rights -- which owes nothing to any counter and does not weaken by one bit if the
generation is narrow. Chapter 8.1 makes that split; this chapter is entirely about the
detector, and a detector is graded on probabilities, not guarantees.

## The transferable rule

- A handle that names storage by index inherits ABA. The bug is a holder keeping a name too
  long, and that bug will exist; the design question is only what happens when it does.
- A generation turns a silent alias into a clean refusal, which is a large win and not a
  guarantee.
- Do not size the counter by service life -- that criterion is unavailable at any width the
  handle word can afford. Size it by the escape probability per erroneous use, `2^-g`, and
  state the probability you chose.
- Several counters at the same width are not several equal defenses. Enumerate who can hold a
  stale handle, for how long, and what a wrap would reach. Reachability and consequence do not
  have to agree.
- Before widening the field, look at what drives it. First-fit collapses a pool of N counters
  into one; next-fit restores all N for one byte of cursor. **The cheaper, better move is
  usually to change how slots are chosen, not how wide the name is.**

## Where to go next

- The boundary this detector sits behind, and why the two must not be conflated: Chapter 8.1,
  *[Naming a kernel object: the handle and the resolve chokepoint](handles-and-the-resolve-chokepoint.md)*.
- The refcount discipline that decides *when* a pool slot is freed, and therefore how often a
  pool generation advances at all: Chapter 8.2,
  *[Adding a kernel object type: the additive recipe](adding-a-kernel-object-type-the-additive-recipe.md)*.
- The reply capability whose object word carries the most-exposed of the three counters:
  Chapter 8.5, *[Synchronous call/reply: the reply capability](synchronous-call-and-reply.md)*.
- The exact widths, the handle codec, and the sign wall that bounds it:
  `kernel/include/kickos/cap.h` (`KCAP_GEN_BITS`, `KCAP_INDEX_BITS`,
  `KICKOS_MAX_HANDLES`) and `kernel/include/kickos/slotpool.h`.
- The contract these guards are part of: `../reference/architecture.md` ("Object model,
  capabilities & IPC") and `../reference/invariants.md`.
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (the protection boundary), and
  the lock-free literature on ABA (Michael and Scott's queue, and the tagged-pointer counters
  it uses) for the same problem in its original setting.
