<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# An atomic buys definedness, not atomicity

> Why a word that two contexts share becomes an atomic even though the load and
> the store it compiles to do not change; what `volatile` does and does not
> promise; and where the honest dividing line runs, which is read-modify-write,
> an instruction pair on part of this fleet and a library call on the rest. It
> sits beside Chapter 0.6 (what is under `#include`, and what a freestanding
> toolchain leaves you) and Chapter 3.8 (the ISR that posts what a thread will
> read). It points into [`../reference/style.md`](../reference/style.md) for the
> house rule and the exact surface,
> [`../reference/invariants.md`](../reference/invariants.md) for the fields whose
> serialisation is a critical section rather than an atomic, and
> [`../design-m5-smp.md`](../design-m5-smp.md) for the multicore primitive, which
> is a property of the silicon and not of the language.

## The field two contexts share

The shape is everywhere in a kernel. A console ring has a head the producer
thread advances and a tail the transmit ISR advances; each index has exactly one
writer and one reader, and the reader is in the other context. A statistics
counter is bumped by an ISR and read by a thread that prints it. A flag is set
once at boot and polled later. In every case the field is a single naturally
aligned word, and the two sides never run at the same instant on a single core.

The traditional answer on a microcontroller is `volatile`, and it is worth being
precise about what that keyword actually promises, because it promises real
things. A volatile access is a side effect the implementation must perform: it
cannot be elided, it cannot be cached in a register across the accesses around
it, and it cannot be reordered with respect to *other volatile accesses* in the
same thread. That is exactly the contract MMIO needs, which is what the keyword
was for.

What it does not promise is anything at all about a second context. The language
gives `volatile` no inter-thread visibility rule, no ordering against
non-volatile accesses, and no place in the memory model where a race becomes
well defined. A volatile store may be reordered with the plain stores that
surround it, so a reader that infers something about ordinary data from having
seen the volatile flag is relying on a guarantee nobody made. `volatile` answers
a different question, and on a single-word field it answers it in a way that
*looks* right, which is the dangerous kind of wrong.

The language does have a mechanism whose entire job is this question, and the
useful fact for a bare-metal kernel is that it is reachable there:
`std::atomic<uint32_t>` compiles under `-ffreestanding` with `__STDC_HOSTED__`
equal to 0 on a Cortex-M0+. `<atomic>` is part of the header subset a
freestanding implementation provides, so reaching for it on a target with no
operating system underneath is a move *toward* the standard, not a hosted luxury
being smuggled in.

## What the atomic actually buys

Here is the claim that gives this chapter its title, and it is the one a reader
is most likely to have backwards.

On every backend this tree targets, a 32-bit naturally aligned load is one
instruction and the matching store is one instruction. There is no tearing to
prevent. Nothing can interleave inside a single access, so no reader can ever
observe half of a 32-bit write. A relaxed atomic load of that same field emits
the *same instruction*. Turning the field into an atomic changes no code.

So the thing being bought is not atomicity, which the hardware already gave you.
It is **definedness**. A plain object written in one context and read in another
is a data race, a data race is undefined behaviour, and undefined behaviour is
precisely the licence an optimiser needs to do the three things that break this
code:

- **Hoist the value into a register** across a loop that reads it, because if no
  defined execution can change it, one read is as good as many. A poll loop then
  spins forever on a stale copy.
- **Elide the store**, or sink it past the point where the other side was
  supposed to see it, because a value nothing in this thread reads back is dead.
- **Split or synthesise an access**, reading the word twice, or narrowing it to
  the byte the code actually tested.

An atomic removes the licence. It states in the type that this object is
concurrently accessed, which is the fact the optimiser has to be told, and then
it hands you an *ordering* you can name and reason about with the ordinary
accesses around it. `volatile` blocks the first two rewrites as a side-effect
rule about the abstract machine, and blocks none of the reasoning about
neighbouring data. That is the whole of the difference, and it is why the change
is not a performance trade: the same instruction, with the race removed from the
program's meaning.

## Relaxed is enough until the value speaks for other data

If the ordering costs nothing, which ordering does such a field need? For an
independent counter or a standalone flag, **relaxed** is the right answer and not
a shortcut. A reader either sees the old value or the new one, and both are
values the field genuinely held. Nothing else is being inferred, so there is
nothing for a stronger ordering to protect.

Relaxed stops being sufficient at an exact moment: when the value **implies
something about other data**. "Head has advanced" meaning "the bytes below head
are written" is a claim about memory the atomic does not name. That implication
is what acquire and release exist for: the release store publishes the writes
that precede it, the acquire load consumes them. A relaxed pair carries the
index and carries no promise about the payload.

Two consequences follow, and they point in opposite directions. A field that
means only itself never needs more than relaxed, however many contexts read it.
A field that means something about its neighbours needs the stronger ordering
*and* it needs the reader to be doing the matching half, which is a design
decision at both ends and not a keyword sprinkled on the declaration.

## Read-modify-write is the dividing line, and it is ISA-split

Load and store are uniform across this fleet. Read-modify-write is not, and this
is the single most surprising thing about atomics on a mixed-ISA target.
Measured with this tree's own toolchains, for `fetch_add` on a 32-bit atomic:

| Backend | How `fetch_add` is emitted |
|---|---|
| armv7m (Cortex-M4, Cortex-M7) | inline, exclusive load and store pair |
| rv32imac | inline, `amoadd.w` |
| Xtensa LX6 | inline |
| host (x86-64) | inline |
| armv6m (Cortex-M0+) | a call to `__atomic_fetch_add_4` |
| RXv3 | a call into the same library |

The two libcall rows are not a slow path, they are a **link** failure waiting to
happen. A freestanding link carries no libatomic. So one source file compiles
cleanly for every board in the fleet and then fails to *link* on two of them,
with an unresolved symbol as the only clue. The compiler accepted it; the ISA it
was aimed at simply has no instruction to do it; and the boards where this bites
are the small ones, which are rarely the first thing anyone builds.

It is worth knowing what that library call would do if it were available,
because it explains why the missing library is a mercy. An implementation that
cannot perform the operation in hardware performs it under a **lock table**:
a small array of locks, chosen by hashing the object's address. That is correct
for threads and it is not interrupt-safe. On a single core, an ISR whose atomic
hashes to a bucket the interrupted thread is already holding can never acquire
it, and the thread can never release it because the ISR will not return. The
deadlock has no timeout and no diagnostic. So the answer to "can an atomic just
fall back on a mutex" is: not on THAT mutex. What makes the lock table unusable
is not that it is a lock, it is that the lock is chosen by hashing an address, so
an ISR can collide with a bucket the thread it interrupted already holds. A lock
the kernel picks deliberately -- the one already covering the field, or a
dedicated cross-core one -- has no such collision, because the code that takes it
knows what else takes it. Generic fallback is the bug; a chosen lock is a design.

That is also why the read-modify-write stays out of the surface, and the reason is
worth stating precisely, because the obvious one is wrong. It is not that an RMW
cannot be built: bracket it with the lock and it exists on every backend. It is
that the cheapest CORRECT mechanism is different on each one -- an inline
instruction pair where the ISA has exclusives, a hardware spinlock on a part whose
peripheral offers a test-and-set, plain interrupt masking where there is only one
core to exclude. A facility whose implementation differs per ISA belongs behind a
seam, the same way a memory-protection backend does, rather than in a header that
pretends one mechanism fits.

And the case that motivates reaching for an RMW usually dissolves on inspection.
A counter with a single writer does not need one: a load, an add and a store is
the same work, and a plain word on every backend. A counter with two writers
needed a lock anyway, which is the next section.

## Why the obvious guard is the wrong one

Given an ISA split, the reflex is to make the compiler police it, and the
standard appears to offer exactly the right trait. It does not. Measured for a
32-bit atomic, `is_always_lock_free` is:

| Backend | `is_always_lock_free` for a 32-bit atomic |
|---|---|
| armv7m | 1 |
| rv32imac | 1 |
| Xtensa LX6 | 1 |
| host | 1 |
| armv6m | 0 |
| RXv3 | 0 |

A static assertion on that trait refuses armv6m and RXv3, which are precisely
the boards whose plain load and store are single inline instructions. The trait
is not lying: it is an **all-operations** guarantee, and read-modify-write is the
operation that fails it, so a type that only ever loads and stores is reported
unusable because of an operation it never performs.

The lesson generalises past this trait. A conformance question has to be asked
about the operations the code actually uses, and there is no standard trait for
"a plain load and a plain store are single instructions." A width bound stands in
for it, and the honest thing is to say at the declaration that it *is* a proxy,
so the next reader does not mistake it for a guarantee the language made. The
exact bound and its wording live with the type in
`system/include/kickos/sys/atomic.h`.

## Sixty-four bits is a different problem

Width changes the answer, and not gradually. A relaxed 64-bit atomic load is a
call to `__atomic_load_8` on **every** backend here, the host aside, including
Cortex-M4 where the 32-bit form is fully inline. There is no 64-bit single-copy
access to reach for, so the whole argument from the second section collapses: the
hardware never gave you atomicity in the first place, and the libcall brings back
both the missing library and the lock table.

A cross-context 64-bit field is therefore a *different design problem* from a
32-bit one, not the same problem one size up. The shape that answers it is a
critical section around the whole read and the whole update, which is what the
software extension of a 32-bit hardware cycle counter to 64 bits already needs
for an unrelated reason (see `cycle64-wrap-extend-atomic` in
[`../reference/invariants.md`](../reference/invariants.md), where the catch-wrap
read has to be indivisible against a concurrent reader). Where such a field
stays plain, `volatile` is doing its actual job of forbidding elision, and the
declaration should say which of its jobs is meant.

## A field with two writers is a lock problem

The strongest simplification in this whole area is to notice what an atomic is
*not* for. A field with two real writers is not made correct by being atomic. It
is made correct by being serialised, and what serialises it is a critical
section. Once one is held, the field inside it needs no atomic at all: the lock
already provides indivisibility and ordering, and marking the field atomic
restates a property somebody else is guaranteeing, which invites a later reader
to conclude the lock is redundant.

The corollary is that a site that seems to need a read-modify-write is usually
reporting a missing lock. Two writers with no lock have a bug that the atomic
would narrow but not remove, because the interesting update almost never fits in
one operation. `console-publish-prime-atomic` in
[`../reference/invariants.md`](../reference/invariants.md) is the canonical case:
publishing the ring's head and enabling the transmit interrupt must happen
*together*, atomic against the ISR's own drain-and-disable, or a wakeup is lost.
No per-field atomic reaches that, because the thing that must be indivisible is a
pair of actions on two different objects.

On a multicore part the primitive that provides such a section is a property of
the silicon rather than of the language, and it differs across parts that look
alike: a hardware spinlock block on one, architectural exclusives on another, a
compare-and-swap instruction on a third. [`../design-m5-smp.md`](../design-m5-smp.md)
works that space out per part, including the case of two cores whose ISA has no
exclusives at all. The point to carry here is only the direction of the
dependency: what a lock can be made of is dictated by the part, and the language
level cannot paper over a part that has nothing to build one from.

## Put the ordering in the type, not in the discipline

If relaxed is the ordering nearly every such field wants, there is a trap in the
plain spelling. On a raw atomic, a bare `=` and a bare conversion to the value
type both mean sequentially consistent, which emits a barrier. Correctness by
convention therefore means spelling the relaxed order at *every* access, and a
single omission is silent: the code still works, it just pays a fence. Measured
barriers per function, an ordering-carrying wrapper against a bare assignment on
a raw atomic:

| Backend | Wrapper | Bare `=` on a raw atomic |
|---|---|---|
| armv6m | 0 | 2 |
| armv7m | 0 | 2 |
| rv32imac | 0 | 2 |
| Xtensa LX6 | 1 | 3 |
| host | 0 | 1 |

Xtensa's relaxed form already emits one ordering instruction of its own, hence
the offset column; on the host the sequentially consistent store becomes a locked
exchange.

Carrying the ordering as a **type parameter** removes the class of mistake
instead of documenting it. The order is stated once, at the declaration, where a
reviewer can audit every cross-context field in a file by reading its
declarations; there is no per-access spelling to forget, and no way to override
the declared order at a call site. The type is
`kickos::Atomic<T, Order>` in `system/include/kickos/sys/atomic.h`, and
[`../reference/style.md`](../reference/style.md) states the rule it exists to
enforce.

One row of that table teaches something larger than atomics. **RXv3 emits no
barrier even for the sequentially consistent form**, so on that backend the
wrapper and the naive spelling are byte-identical. A port that can never exhibit
a bug is not evidence that the bug is absent; it is a place where the measurement
is blind. Any claim that a whole fleet is free of some cost has to name the
backend on which the cost would not have shown up, or it is a claim about the
sample rather than about the code.

## One spelling for two languages

A last wrinkle appears where a struct is part of an interface a C consumer must
also name, as the headers under `user/include/kickos/sys/` are. Such a struct
cannot hold a C++ class template, so the field needs a spelling C understands,
and the C spelling is `_Atomic`.

That is not a C++ keyword. Compiling a header that declares `_Atomic(uint32_t)`
in a struct succeeds under C11 and is rejected under C++20. It succeeds again
under C++23, which adds `<stdatomic.h>` and makes `_Atomic(T)` a function-style
macro there, so a single declaration serves both languages. The accessors are
portable across the gap already: `atomic_load_explicit` and
`atomic_store_explicit` compile in both languages today, with the ordering named
at each call, which is exactly the discipline the type parameter exists to
retire on the C++ side. The shared-header case therefore pays the per-access
spelling because a shared header is the one place it cannot be avoided.

## The rule

The hardware already made a 32-bit aligned access indivisible, so an atomic is
not there to prevent tearing. It is there to remove a data race from the
program's meaning, because the race, not the instruction, is what lets the
compiler hoist, elide or split the access. That is why the change costs nothing
where it applies: relaxed load and store are the same instruction `volatile`
compiled to.

The cost lives entirely in the operations the hardware does *not* have. A
read-modify-write is inline on part of this fleet and a library call on the rest,
and the library the call needs is absent from a freestanding link, so the failure
is a link error on the smallest boards. The standard trait that looks like the
right guard reports the whole operation set and so refuses those same boards for
an operation they never use. A 64-bit field is a libcall everywhere and is a
different problem. And a field with two writers was always a lock problem: the
critical section is what serialises it, and once there is one, the atomic states
nothing the lock was not already providing.

The counter case has a name in this tree, because a load-add-store pair is the one shape
that recurs: a helper carries it, and the helper's contract is the single writer rather
than any atomicity. See [`../reference/style.md`](../reference/style.md) for the spelling.

## Where to go next

- What a freestanding toolchain provides and what it does not, which is why
  `<atomic>` being in the subset matters: Chapter 0.6, *What's under #include:
  the C library and the C++ runtime*.
- The ISR-to-thread handoff these fields serve, and why the record has to be a
  latch rather than a shared flag: Chapter 3.8, *A masked interrupt is latched,
  not lost*.
- The exact rule, the surface the wrapper exposes, and where `volatile` remains
  correct: [`../reference/style.md`](../reference/style.md).
- The fields whose indivisibility is a critical section rather than a type:
  [`../reference/invariants.md`](../reference/invariants.md).
- The multicore primitive a critical section would be built from, per part:
  [`../design-m5-smp.md`](../design-m5-smp.md).
