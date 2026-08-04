<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# A stale handle must not be usable: generations, width, and allocation policy

> Chapter 8.1 introduced the generation counter as the *detector* half of the resolve
> chokepoint and left it there. This chapter is the detector on its own terms: what a
> generation actually buys, why "how many bits?" is the wrong first question, why several
> counters of identical width are still not several equal defenses, and why the *allocation
> policy* of the pool underneath turns out to be worth more than any of the bits. For the
> exact widths, the handle codec and the contract they enforce, link into
> `../reference/architecture.md` ("Object model, capabilities & IPC"),
> `../reference/invariants.md` (`handle-not-pointer-across-boundary`,
> `object-access-via-per-task-cap`), and the code they describe --
> `kernel/include/kickos/slotpool.h`, `kernel/include/kickos/cap.h` and
> `kernel/include/kickos/thread.h`.

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
capability table's own codec and its fixed index/generation split are in
`kernel/include/kickos/cap.h`. The exact contract is in the Reference -- do not infer it
from this chapter.

## The honest part: "how wide should the counter be?" is the wrong question

Here is the reflex the moment someone points at the counter: *make it wider*. Widen it
enough and wrap becomes impossible, and then the guard is a guarantee rather than a
heuristic. It is a very natural thought and it is wrong, and three questions show why. Work
them, because everything downstream in this chapter depends on having actually done it -- and
the third one has a twist that is worth the detour on its own.

**How fast can a slot recycle?** One recycle is a close plus a create -- two syscalls, a
handful of list operations, no I/O and no blocking. On a 16 MHz microcontroller call it 20
microseconds. That is a deliberately unflattering estimate for the defender: a system doing
nothing but churning one slot.

**How many recycles in a service life?** Ten years is about 3.16e8 seconds. At 20
microseconds a recycle, that is about 1.6e13 recycles, or roughly **2^44**.

**How many bits are available?** This is the step with the surprising answer, and it is
worth slowing down for, because the obvious answer is wrong in an instructive way.

The obvious answer is 31. A syscall that mints a handle has to be able to fail, the fleet
convention puts a negated error code in the return register, and a negative return is
therefore already spoken for -- so a handle that comes back *as the return value* must stay
positive in a 32-bit `int`: 31 bits, split between the index field and the generation field.
The index field is not free -- it has to address every slot -- so the generation gets
whatever is left, strictly less than 31.

Now look at where that 31 came from. Not from the register, which is 32 bits wide. Not from
the machine, which has no opinion about sign. It came from a single interface decision --
that one return register would carry both the handle and the error code -- and a different
decision undoes it. A capability-minting syscall takes an out-parameter and returns a
status: `kos_sem_create(int initial, kos_cap_t* out_cap)` returns 0 or a negated error and
writes the handle through the pointer (`user/include/kickos/sys.h`). The handle no longer
shares a register with the failure signal, so it is a full **unsigned 32 bits**, and all of
them are spent: a fixed 16 index bits and 16 generation bits (`KCAP_INDEX_BITS`,
`KCAP_GEN_BITS`), with a live handle allowed to have bit 31 set.

That observation is worth more than the bit it recovered: **a constraint that looks like a
property of the machine is often a property of an interface, and the way to find out is to
ask what would have to change for it to go away.** "The word must stay positive" sounds like
arithmetic. It is a calling convention. Sign is not a budget the hardware imposes; it is a
bit somebody spent on signalling failure, and the moment failure gets a channel of its own
the bit comes back. (It is not free even then -- once every bit of a handle can be live,
"negative" is no longer available as an error test on one, so "no capability" has to become
a *value* the codec provably cannot mint. Chapter 8.1 covers what that costs.)

Now put the recovered bit against the service-life number, which is the whole reason for
recovering it. Even if the index field were zero bits wide and the generation got the entire
32, `2^32` recycles at 20 microseconds each is 85,900 seconds -- **just under 24 hours**.
Not twenty-four years, not twenty-four months. One day of dedicated churn exhausts the
widest counter a 32-bit handle can physically hold. Winning the sign bit doubled the wrap
distance and moved the answer from twelve hours to one day, which is a real improvement and
changes nothing about the conclusion. The gap to ten years is twelve further bits (`2^44 /
2^32`), and a 32-bit word that already spends 16 on an index has no twelve to give: closing
it means a 64-bit handle, a fleet-wide ABI decision rather than a wider field.

And the width actually spent is not 32, it is 16. So a single slot's counter comes back
around after 65536 frees *of that slot* -- **about 1.3 seconds** of dedicated churn. Hold on
to that number; the last section of this chapter is about what decides whether it is 1.3
seconds or 1.3 seconds times the pool size.

So: **no affordable width makes wrap impossible.** Not the width you have, not the width you
were about to ask for, not the widest one the encoding permits, and not the one you get by
winning an argument with a calling convention. Any generation width defended on the grounds
that it is "wide enough for the device's service life" is self-deception -- the sizing
criterion being invoked is not available at any price a 32-bit handle can pay.

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
the second trap is treating them as interchangeable because they are the same mechanism.
They are not. Three questions separate them by *exposure*, and none of the three is about
bits:

1. **Who can hold a stale handle?** Only the task that made the mistake, or a *peer*?
2. **For how long?** Is retention bounded by something you control, or does the holder keep
   it as long as it likes?
3. **What does a wrap actually reach?** An object the holder already legitimately has, or
   one it was never given?

Then a fourth, which the arithmetic above has just made interesting: **what would widening
this particular counter cost?** It is tempting to expect three different answers -- three
counters at three layers, three encodings, surely three different amounts of slack. There is
no slack anywhere. Each of the three below splits a 32-bit word into 16 index bits and a
16-bit generation, and each of them spends that word entirely. Not one has a spare bit to
hand its generation, so on all three "widen it" means a wider handle *word*: a fleet-wide ABI
decision, never a field edit.

That uniformity matters, and it is not a coincidence to wave past, because the exposure is not
uniform at all: equal widths, wildly unequal exposure. So whatever set the width, it was not
the hazard analysis; it was the size of the word the handle travels in, and a word is 32 bits
for reasons that have nothing to do with ABA.

KickOS carries three such counters, and running the questions over them separates them
sharply.

**The per-task capability generation** (the one in a capability table slot) guards
use-after-close. Its stale handles live in exactly one place -- the table of the task that
closed the capability -- and resolution takes the rights from whatever entry is seated
*at that moment*. So a wrap hands that task an object it already holds a capability for, at
the rights that entry already carries. The failure is one task misdirecting itself among its
own objects. It is the counter with the *widest* reachability into buggy code and the
*narrowest* consequence. It spends a whole 32-bit word -- 16 index bits and 16 generation
bits (`KCAP_INDEX_BITS`, `KCAP_GEN_BITS`) -- and the split is fixed fleet-wide rather than
derived from a board's table size, so the same logical capability prints the same value on
every target and a per-board RAM decision cannot renumber the ABI. Escape probability, one in
65536 per erroneous use.

**The object-pool generation** guards use-after-destroy, and it inverts the exposure.
Wrapping it is rarer, because a pool slot only frees when the last holder closes and the
refcount reaches zero -- there is a real backstop making the free path infrequent. But the
consequence, if it ever bites, is genuinely cross-task: a handle resolving to an object a
*different* task created. The backstop bounds *when* a wrap can bite; it does nothing about
*how badly*. This is also the handle furthest from any userspace convention: it is
**kernel-internal**, because a create stores the pool handle inside a capability table entry
and hands userspace the *capability* handle instead (`kernel/syscall/syscall_obj.cc`). That
freedom buys it no extra room -- the same 16 and 16, the same full word -- but it does show up
in the code, in a place worth stopping at. The pool passes its handles around in a *signed*
`int`, and neither `free` nor `resolve` is permitted to test that sign: both cast to
`uint32_t` first, because a slot recycled past its 32768th free mints handles with bit 31 set,
and a `handle < 0` guard would from then on silently refuse to release live slots. What keeps
a stray `-1` from resolving is not the sign but the **reserved all-ones index** the pool never
seats, so no handle a live slot can mint carries it (`kernel/include/kickos/slotpool.h`).
Escape probability, again one in 65536 per erroneous use.

**The thread generation carried inside a reply capability** is the interesting one. A reply
capability names the parked caller by its generational thread handle. That handle is held by
the *server* -- a peer -- and it is held for as long as the server chooses to hold it. You
cannot make it drop the thing. Reachability is cross-task, retention is holder-controlled
and unbounded, and the consequence of a wrap is a reply delivered into a thread slot that has
since been recycled into an unrelated thread. It is the most exposed of the three by every one
of the first three questions -- and by the fourth it is not one bit better funded than the
other two: 16 index bits, a 16-bit generation, a full word, and its own reserved all-ones
index to keep "no thread" unmintable (`kernel/include/kickos/thread.h`).

Two interfaces press on that word, and both of them had to be pushed back for it to stay
whole. The first is the calling convention, and it is worth seeing what it costs in the
concrete rather than the abstract, because the failure is uglier than the arithmetic suggests.
Spend the whole word on index plus generation and the handle goes negative on the far side of
its own wrap; a caller reading `< 0` as failure then cannot tell a live handle from an errno.
Run the numbers on one slot churned in a loop: past its 32768th recycle every handle it mints
has bit 31 set, so over 40000 spawn/exit cycles against that single slot **7232 of the handles
handed back are negative, the first at cycle 32769** -- and a caller testing the sign would
refuse to cancel a thread the kernel had just created for it, as a bad handle. Note what the
width alone would not have told you: the wrap is not a distant event. One slot recycled 32768
times is seconds of work, and nothing about it is exotic. So a thread handle travels in an
out-parameter, exactly like a capability handle, and the minting call returns a status: **a
value that spends its whole word cannot also carry a failure signal.**

The second is the packing, and the pair is the instructive part. A reply capability names its
caller in one 32-bit entry field, and the thread handle fills that field exactly -- while the
reply protocol also needs the caller's 8-bit call sequence stored in the same entry, or a
late reply from a previous call could not be told from a live one. Take those eight bits out
of the handle's field and they come out of the *generation*, from the top, silently, with
nothing at runtime to report it. So they are not taken from there. They ride in the spare bits
beside the entry's type and its rights, each of which uses three of its eight
(`KCAP_REPLY_SEQ_LO_BITS`, `KCAP_REPLY_SEQ_HI_BITS`), and the handle keeps its own field
whole. Worth extracting, because it recurs: **when a field is exactly full, look for unspent
bits in its neighbours before you conclude the width is the constraint.** The reply entry was
never short of space; it was short of *packing*.

The general lesson gets two edges. The first is what the three exposure questions were for:
**reachability and consequence can point in opposite directions, so the widest counter is not
automatically the one under the most pressure, and neither is the one that wraps most
often.** Before sizing anything, enumerate the holders and their retention. A counter
guarding a mistake a task can only make against itself is a fundamentally different object
from one guarding a handle a peer holds and that you cannot make it drop.

The second edge is arithmetic rather than judgement, and it cuts the other way: **rank the
three by exposure and you have learned nothing about what any of them can be paid.** The
capability generation, whose stale handles never leave one task's own table, and the thread
generation inside a reply capability, held by a peer for as long as that peer likes, sit at
the two ends of the exposure scale -- and they are the same sixteen bits in an equally full
word. The width was settled by the size of a machine word, and kept whole against a return
convention and a packing decision; not one of the three parties to that was answering a
question about ABA. Sized by the hazard, the room would not have come out equal.

So a sizing pass has two halves that are easy to run together and must not be: work out what
each counter is *exposed* to, and separately work out what each counter can actually be
*paid*. Here the second half returns the same answer three times -- a full word, no slack,
widening means a 64-bit handle -- and that answer is the useful result, not a disappointing
one, because it says the width is not the lever. The rest of this chapter is about what is.

This also disposes of a tempting economy, wherever an encoding really is shared. Where two
fields sit in one word -- a call sequence beside a type, or beside a rights mask -- widening
either narrows the other, so bits "spent" on sequence numbers are bits a wider rights mask
cannot have, and the assertion that catches the collision has to sit beside the *type* width
rather than beside the packing. Price such a change against every field in the word, not just
the one you were thinking about; the field with the least visible failure mode is frequently
the one that pays.

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

There are two ways out, and neither of them costs a handle bit. They are the same rule stated
twice: **never hand back the slot you just released.**

The first is a **next-fit cursor**. Remember the slot the last allocation landed on, resume
the scan from there rather than from zero, and -- the part that is easy to get wrong -- do not
rewind the cursor when a slot is freed. Now the same workload walks the pool, slot 0, then 1,
then 2, wrapping around at N, and each slot's counter advances once per lap instead of once
per recycle. The cost is **one cursor per pool**, no handle bits, no change to the encoding,
no change to the ABI, and no change to what resolve does.

The second is a **FIFO free list**. Thread the free slots onto a list, take the head to
allocate, and put a released slot back at the **tail**. The slot handed out next is then
always the one that has been free the longest, so with F free slots each counter advances
once per F mints. The cost is a list head plus the links -- and the links can live in the dead
slots themselves, which have nothing else to say while they are dead. Note how narrow the
margin for error is: inserting at the *head* instead, which reads just as naturally, hands the
just-released slot straight back and concentrates every mint on one counter. That is first-fit
again, by another route, and the ordering is the only thing standing between the two.

Compare the purchases honestly:

| move | cost | effect on effective wrap distance |
| --- | --- | --- |
| widen the generation by k bits | k bits in every handle, possibly a wider handle word and an ABI change | x 2^k, capped by whatever the handle word has left |
| first-fit -> a next-fit cursor | one cursor per allocator | x N (the number of slots), free |
| first-fit -> release at the list tail | one list head; the links ride inside the dead slots | x F (the number of free slots), free |

On a pool of a few dozen slots either policy buys five bits of generation and spends none. On
the pools where the counter is under the most pressure -- the ones churning one object at a
time, which is precisely the workload first-fit degenerates on -- it buys the most. **Policy
bought what no amount of bit-packing could.**

### Which allocator does what, and why the distinction matters

The principle is general. Its *application* is per-allocator, and the allocators sitting under
a capability handle do not all reach for the same instrument -- nor do they all reach. "The
system spreads its recycles" is the wrong thing to take away. Read each one.

The **object pool** spreads by position. `alloc` begins its scan at a stored cursor and walks
the ring once, and `free` deliberately does *not* rewind the cursor to the slot it just
released -- aiming it back at the freshly-freed slot is exactly the first-fit concentration
the cursor exists to prevent (`kernel/include/kickos/slotpool.h`).

The **per-task capability table** spreads by age. Its free dynamic slots are a circular list
threaded through the object words of the dead entries themselves; a mint takes the head, and
closing a capability returns its slot to the tail (`kernel/include/kickos/cap.h`,
`kernel/syscall/cap.cc`). Same goal as the cursor, different instrument, and a different bill.
The mint is a head read rather than a scan, which is worth having where a client mints a reply
capability into a *server's* table on the IPC path; the links cost no bytes of their own; and
the head is per-task state, two bytes in every TCB rather than one field in one pool header --
cheap either way, but a different line in a different budget.

Then the **thread pool**, which reclaims the *lowest* exited slot. A workload that keeps one
worker alive at a time -- spawn, run, exit, spawn again -- comes back to that same slot every
cycle, so the thread generation's effective wrap distance is one counter's and not the pool's:
the seconds of dedicated churn the arithmetic above worked out, whatever the pool's width. And
this is the counter a peer holds for as long as it likes, so of the three it is where that
shortening lands hardest.

It is also the one place where declining to spread is a purchase rather than an omission, and
the price is worth reading, because it is what makes the principle land differently here. This
pool keeps no free-slot bitmap: liveness is the TCB's own state, and slots come from a bump
allocator whose high-water mark never falls. Taking the lowest exited slot holds that mark
down, and two sweeps are bounded by it rather than by the pool's declared width -- one of them
the reclaim's own, which runs with interrupts masked. A cursor preferring fresh slots would
push the mark to the full width almost at once and make that masked loop always cost the
maximum. So the wrap distance here is spent on a shorter interrupt-masked window. Spreading
recycles is close to free in the two allocators above and is not free in this one, which is
why matching the neighbour's policy on sight would be the wrong instinct.

What that does and does not mean has to be said plainly, because a short wrap distance is a
detector with poorer odds and not a hole. A reply is not admitted on the thread generation
alone: the entry also carries the caller's call sequence, and a reply whose sequence does not
match the one the parked thread is waiting on is refused -- an independent guard on the same
path, and the reason those eight bits were worth somebody's packing. Behind both, Chapter 8.1's
split is doing its job: bounds, liveness, type and rights are what confine authority, and they
owe nothing to any counter. The two facts have to be stated together rather than one standing
in for the other: how far a counter goes is a question about *detection rates*, and it answers
nothing about *confinement*.

The allocation policy is the part of the guard that lives outside the guard, which is exactly
why it gets missed: the counter is in the header, the packing is in the header, the comparison
is in resolve, and the thing that decides whether any of it works is a cursor one allocator
declines to rewind, an end of a list another chooses to insert at, and a scan direction a
third never had reason to revisit. Every one of those reads like a triviality. Read each
allocator under a handle as part of the ABA guard, because that is what they are.

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
- Do not size the counter by service life -- that criterion is unavailable at any width a
  32-bit handle can afford. Size it by the escape probability per erroneous use, `2^-g`, and
  state the probability you chose.
- When a width looks like a hard limit, find out what actually imposes it. A sign bit lost to
  an errno-carrying return is an *interface* cost, not a machine one, and moving the failure
  signal to its own channel gives the bit back. Ask what would have to change for the
  constraint to go away; sometimes the answer is one function signature.
- Several counters running the same mechanism are not several equal defenses, and equal widths
  are no evidence that they were sized. Enumerate who can hold a stale handle, for how long,
  and what a wrap would reach -- then, separately, what each one's encoding could actually pay
  for. Exposure and encoding are set by different things and routinely disagree.
- Before widening the field, look at what drives it. First-fit collapses N counters into one;
  a cursor that is never rewound, or a release that goes to the tail of a free list, restores
  all N for a couple of bytes of state. **The cheaper, better move is usually to change how
  slots are chosen, not how wide the name is** -- and check per allocator, because the policy
  is a property of each one, not of the mechanism they share.

## Where to go next

- The boundary this detector sits behind, and why the two must not be conflated: Chapter 8.1,
  *[Naming a kernel object: the handle and the resolve chokepoint](handles-and-the-resolve-chokepoint.md)*.
- The refcount discipline that decides *when* a pool slot is freed, and therefore how often a
  pool generation advances at all: Chapter 8.2,
  *[Adding a kernel object type: the additive recipe](adding-a-kernel-object-type-the-additive-recipe.md)*.
- The reply capability whose object word carries the most exposed of the three counters, and
  the call sequence that guards the same path independently: Chapter 8.5,
  *[Synchronous call/reply: the reply capability](synchronous-call-and-reply.md)*.
- The exact widths and the handle codec: `kernel/include/kickos/cap.h` (`KCAP_GEN_BITS`,
  `KCAP_INDEX_BITS`, `KICKOS_MAX_HANDLES`), the reply entry's call-sequence packing
  (`KCAP_REPLY_SEQ_BITS`), the capability table's free list (`KCAP_FREE_NONE`), and the two
  pools -- `kernel/include/kickos/slotpool.h` and `kernel/include/kickos/thread.h`.
- The contract these guards are part of: `../reference/architecture.md` ("Object model,
  capabilities & IPC") and `../reference/invariants.md`.
- Further reading: Tanenbaum, *Modern Operating Systems*, ch.1 (the protection boundary), and
  the lock-free literature on ABA (Michael and Scott's queue, and the tagged-pointer counters
  it uses) for the same problem in its original setting.
