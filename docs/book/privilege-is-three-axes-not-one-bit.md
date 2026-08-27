<!--
SPDX-License-Identifier: CECILL-C
Copyright (c) 2026 Philippe Leduc
-->
# Privilege is three axes, not one bit

> A Chapter-7 companion about what the word *privileged* actually names, and why a
> kernel that spells it as one boolean has fused three separable powers into one
> flag. It takes the three apart, names which mechanism carries which power, says what
> a privileged thread *is* (a Linux kthread, and an uncomfortable one for a
> microkernel), and ends on the consequence that catches test authors: a thread on the
> bypass side of the unit cannot prove anything about the unit. Its two mechanism
> siblings cover what runs *inside* the fence once it is up:
> [`exceptions-and-rtti-under-memory-protection.md`](exceptions-and-rtti-under-memory-protection.md)
> (what a throw/catch touches at runtime) and
> [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md)
> (the writable floor and the linker split). It binds to
> [`../reference/architecture.md`](../reference/architecture.md) ("User/kernel
> separation", "Memory domains", "Object model, capabilities & IPC"),
> [`../reference/invariants.md`](../reference/invariants.md) (the
> `privilege-escalation-gated` / `syscall-restores-resting-priv` family), and
> `kernel/include/kickos/cap.h`.

## The problem: one word, three powers

Ask what a privileged thread may do and you get three answers that sound like one
answer:

1. **CPU mode.** It may execute instructions the architecture reserves for the
   privileged level, and read/write the control registers that go with them.
2. **Memory posture.** It reaches memory the protection unit would deny to anyone
   else -- in practice the kernel's own code and data, and every other task's.
3. **Authority.** It may ask the kernel to do things the kernel will not do for
   anyone else: hand out RAM, mux a pin, retune the core clock, bind an interrupt
   line, take over the console, end the system, and create another thread like
   itself.

These are three different questions about three different mechanisms, and a single
`bool privileged` on the thread control block answers all three at once. That the
three usually travel together on a conventional MCU is a coincidence of the usual
hardware, not a law:

- **Xtensa LX6 has no ring split at all.** Axis 1 does not exist on that core. Axis
  3 still does, because an authority check is kernel logic and needs no hardware
  support whatsoever.
- **A chip with no protection unit** makes axis 2 vacuous while axes 1 and 3 stay
  perfectly real.
- **A thread holding the right authority bits** can have axis 3 without either of the
  other two -- which is the whole point of the authority word below.

So the first job is to take the three apart and say which mechanism carries which.
Everything else in this chapter follows from that split.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (user vs kernel mode as
the protection boundary) and ch.9 (protection domains and the principle of least
authority).*

## Axis 1: CPU mode, and where it lives

A thread's CPU mode is not a variable the kernel consults; it is part of the machine
state the context switch restores, exactly like a register (Chapter 3.5,
*[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*).
Every port encodes it in the frame `arch_context_init` fabricates for a brand-new
thread, and restores it on the first switch-in:

| Port | Where the mode lives in the fabricated frame |
|---|---|
| armv7m | `ctx.npriv` **and** `ctx.resting_npriv` (`arch/arm/armv7m/arch_armv7m.cc`) |
| armv6m | the same two fields (`arch/arm/armv6m/arch_armv6m.cc`), read by `switch.S` at fixed offsets |
| rv32imac | `mstatus.MPP` in the fabricated frame -- M-mode for privileged, U (0) otherwise (`arch/riscv/rv32imac/arch_rv32imac.cc`) |
| rxv3 | `PSW_THREAD_USER` vs `PSW_THREAD_KERNEL` in the fabricated PSW (`arch/rx/rxv3/arch_rxv3.cc`) |
| lx6 (Xtensa) | nowhere: the core has no ring split, so `arch_context_init` takes the flag and discards it (`arch/xtensa/lx6/arch_xtensa.cc`) |
| host sim | nowhere either: `arch_context_init` discards it too, and the posture comes from axis 2 alone (`arch/sim/sim.cc`) |

Two things fall out of "it lives in the frame", and both are load-bearing.

**A per-thread constant is free; a mid-life change is not.** Deciding the mode once,
at `thread_create`, costs nothing on any port -- the machinery to carry it already
exists because the switch has to restore it anyway. *Changing* a running thread's
mode is a different kind of change: it has to update the saved state that the next
switch-in will restore, in assembly, on every port, and it opens a window during
which the thread's mode and its region set disagree. This is why a privilege posture
is a property of a thread's first frame rather than a transition it performs.

**Resting mode and transient mode are different things.** The armv7m frame carries
*two* fields, and the second one is the tell. A thread that traps into the kernel
runs kernel code at the privileged level for the duration of the trap -- so its
saved mode while it is blocked mid-syscall is *privileged*, and it must resume that
way or the continuation faults. What it returns to when the syscall finishes is its
**resting** mode, held separately and restored by the trap epilogue (invariant
`syscall-restores-resting-priv`). Every thread therefore has a resting mode it owns
and a transient mode the trap path lends it; Chapter 3.9,
*[The syscall path](the-syscall-path-trap-dispatch-return.md)*, is where that lending
happens. The host sim makes the distinction unusually visible: it has no CPU mode at
all, so it models the transient raise as a per-context counter, incremented on
syscall entry and decremented on the way out, deliberately per-context so it
survives a blocking switch.

## Axis 2: memory posture, composed once from the flag

The region set an MPU (or the sim's `mprotect`) enforces is assembled in
`thread_create` (`kernel/thread/thread.cc`) and keyed on the same flag:

- **Privileged**: the whole arena, plus the backend's permissive background covering
  code, kernel data and stack. One region suffices.
- **Unprivileged**: app code (RX) plus app static data (RW, no-execute), the
  domain's granted region(s), and the thread's own stack -- assembled
  explicitly, because an unprivileged thread has no background default.

A word on that stack, because it is easy to read more into it than it promises.
Because the set is *per thread*, a sibling in the same task faults on it: the region
is in one thread's set and not the other's. That denial is real, and it is a
property of *this* mechanism rather than of the operating system. Where memory
protection is a page table instead of a descriptor list, per-thread tables would buy
nothing the family asks for, so a task's stacks are mapped once for the whole task
and a sibling reaches them. What portable code may rely on is the weaker statement
that holds on both: **a thread-scoped grant guarantees access to its holder.** The
same distinction the thread-local carve makes -- naming, not isolation -- and
Chapter 7.7, *[Whoever stacks the trap frame owns the bounds
check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md)*, is where it starts
to matter.

Composed *once*, from a flag that does not change afterwards, which is the axis-1
argument arriving on the memory side: there is no moment at which the set has to be
rebuilt, so there is no half-rebuilt set to reason about. *When* that set is loaded
into the hardware is its own subtlety, and it is Chapter 7.5,
*[Protection follows the CPU, not the scheduler's intent](protection-follows-the-cpu-not-the-schedulers-intent.md)*.
The exact region-set contract is `../reference/architecture.md` ("Memory domains");
why the permissive privileged background is not free even for the kernel is
Chapter 7.6, *[The CPU reads ahead](memory-types-and-speculative-access.md)*.

There is a second, quieter edge to this axis, and it is easy to miss because it is
not about what the thread's own instructions may touch. When the kernel dereferences
a pointer a caller supplied, it does so *privileged*, so it must first check that the
caller could have reached that memory itself -- the confused-deputy floor (Chapter
7.1, *[Alignment across the syscall boundary](alignment-across-the-syscall-boundary.md)*).
That check begins by asking whether the caller is privileged and, if so, admits the
range wholesale. So the flag decides both what the thread may touch directly and what
the kernel will touch on its behalf unchecked. Two mechanisms, one flag, same axis.

## Axis 3: authority, which is not a mode at all

The third power has nothing to do with what instructions a core will execute or what
addresses a unit will admit. It is a policy question the kernel asks itself at a set
of gates in the syscall surface, each guarding an act with fleet-wide consequences
(the gates are enumerable: every one is a `cap_check_authority` call site):

| Authority | Guards |
|---|---|
| `AUTH_MEMORY` | carving RAM from the shared arena; granting an MMIO window at spawn; self-granting a carved range into the caller's own region set |
| `AUTH_PINMUX` | configuring a pin's function in the shared mux block |
| `AUTH_PSTATE` | retuning the core clock, which retimes every thread's deadlines |
| `AUTH_IRQ` | binding an interrupt line's dispatch, and arming a controller line |
| `AUTH_SYSTEM` | ending the system: shutdown and reboot |
| `AUTH_CONSOLE` | taking over the console from the kernel |

Where the cut falls between those rows is a design decision and not a listing
order. Two of them look mergeable and are not. Retuning the clock and ending the
system are both "acts on the whole machine", but a CPU-governor service needs the
first and must never have the second, so folding them would hand a thread whose
whole job is a frequency table the power to halt the board. Publishing the console
is separate for the mirror-image reason: the thread that hands the UART to a
userspace driver is, in a system that boots from one bring-up body, the same thread
that will later end the system when it returns -- and it should be able to give up
the console the moment the driver exists while keeping the one authority its own
exit path needs. A bit that cannot be dropped independently of another is a bit
whose holder keeps a power it stopped needing.

Nothing in that list needs a CPU mode. A gate is an `if` in the kernel, and the
kernel is already the thing being asked. So how should the answer be stored?

**Keep it in the flag.** What that buys is honest and small: one bit, no lookup, no
new type. What it costs is everything a bit cannot express. It is all-or-nothing (a
thread that needs to mux one pin at boot gets clock retuning and the console too);
it lasts exactly as long as the thread does; it cannot be narrowed, handed on in
part, or inspected as anything other than "yes to everything".

**A bare flag word on the thread.** One bit per authority on the TCB instead of one
bit for all of them, checked per gate. This buys narrowing, and narrowing is genuinely
most of the win. What a *bare* word does not buy is any rule about how the bits
travel: they are ambient properties of a thread, so nothing says a child may not be
created holding more than its parent, nothing says the set only ever shrinks, and every
gate is free to read the word its own way and get it subtly wrong in one place out of
nine.

**A capability.** The system already names authority-bearing things per task, with a
rights field, a narrow-only delegation rule, and one resolve chokepoint (Chapter 8.1,
*[Naming a kernel object](handles-and-the-resolve-chokepoint.md)*). Adopting that
machinery would inherit those rules -- already written, already audited -- rather than
inventing a second set beside it.

KickOS takes the second, under the third's rules, and the section below is about why
that is a synthesis rather than a compromise: the discipline is the valuable half of
the capability answer, and it turns out not to need the table. First, what happens to
the flag.

## What carries what, after the split

| Axis | Carried by | Decided | Can it be dropped? |
|---|---|---|---|
| CPU mode | the fabricated first frame (per ISA) | once, at thread creation | no |
| Memory posture | the thread's composed MPU region set | once, at thread creation | no |
| Authority | an authority word on the thread | at creation, narrowable | yes |

`Thread::privileged` survives the split with a narrowed meaning. It still selects the
memory posture, it is still the confused-deputy bypass, and it stays the home for one
authority that is deliberately **not** a capability: **may spawn a privileged child**.
That exemption deserves its reason stated, because it looks like an inconsistency:

> A capability to create privileged threads is equivalent to holding every authority,
> forever, and to being able to hand that on.

A thread that can create a privileged child can create one that does anything at all,
including creating more of them -- so narrowing such a capability is meaningless, and
delegating it is unbounded escalation with extra steps. There is nothing to gain from
making it nameable, and a great deal to lose. So it stays where it cannot be handed
around: a property of the creating thread, checked directly (`thread_spawn` refuses an
unprivileged caller with `-KOS_EPERM`; invariant `privilege-escalation-gated`).

## What a privileged thread IS: a kthread

The clearest way to hold all of the above is a comparison a Linux reader already has.
**A privileged thread in KickOS is structurally a kthread**: a thread of execution
that lives inside the kernel's protection domain, scheduled by the ordinary scheduler,
on the ordinary run queue, at an ordinary priority, blocking on the ordinary
primitives. It is not a special scheduling class and it is not "the kernel" in any
sense the scheduler recognises. The *only* things that distinguish it from a user
thread are the two axes above -- which memory it may touch, and what it may ask for.

That framing is worth having because it tells you exactly what the flag buys and what
it does not. It buys reach: such a thread can call into kernel data structures, poke a
peripheral register directly, and pass the kernel a pointer to anything. It buys
nothing at all in scheduling, timing, or capability, and it does not make the thread
trusted in any way the code can check -- it makes the thread *unchecked*, which is a
different thing.

### The uncomfortable part, stated rather than defended

Having kthreads at all sits uneasily with the microkernel premise. The whole point of
the architecture is to minimise what runs inside the kernel's protection domain:
drivers, filesystems and network stacks are unprivileged userspace servers reached by
IPC precisely so that a bug in one of them is contained. Every privileged thread is a
hole in that claim, and the honest summary is the maintainer's own: *we can do it,
probably a wrong idea for a microkernel though.*

This is a compromise, not a design goal, and it is worth naming as one. A privileged
thread is an escape hatch for bring-up work that is not expressed as an
authority the kernel can grant -- a peripheral block that must be ungated before any
driver can reach it, a register that the silicon makes reachable only from the
privileged level. Each one is a piece of the isolation story not yet expressed as
authority, sitting in the kernel's domain in the meantime.

### But at least one is forced by hardware

The nuance that keeps this honest in both directions: **one kthread is forced by the
silicon, and the rest are choices.**

The `idle` thread is privileged and holds no capabilities. It runs no application
code, and it exists to execute exactly one instruction -- the one that stops the core
until an interrupt arrives -- which is privileged on real members of the fleet:

- **RXv3 `WAIT` is a privileged instruction, and this one is categorical.** The RX
  Family ISA manual lists the privileged instructions as `RTFI`, `MVTIPL`, `RTE` and
  `WAIT`, and states that executing one in user mode "produces a privileged instruction
  exception" (*RX Family RXv3 Instruction Set Architecture User's Manual: Software*,
  section 1.4.3 *Privileged Instruction*; the `WAIT` page is headed "WAIT (privileged
  instruction)"). User mode is `PSW.PM` = 1 (bit 20, section 2.2). There is no bit that relaxes
  this, so on RX an unprivileged idle simply cannot execute the instruction it exists to
  execute.
- **RISC-V `WFI` is weaker than that, and the earlier wording here overstated it.** The
  privileged spec says WFI "is available in all privileged modes, and optionally
  available to U-mode" (section 3.3.3), and `mstatus.TW` is what intercepts it: with `TW`=0
  "the WFI instruction may execute in lower privilege modes when not prevented for some
  other reason", and with `TW`=1 a WFI in any less-privileged mode that does not complete
  within a bounded time raises an illegal-instruction exception (section 3.1.6.6). KickOS runs
  M-mode kernel + U-mode threads with **no S-mode**, and in that configuration a U-mode
  WFI is *permitted* whenever the platform leaves `TW`=0. (Had S-mode been implemented,
  U-mode WFI would trap; section 3.1.6.6 says so explicitly.) On top of that, "a legal
  implementation is to simply implement the WFI instruction as a NOP" (section 3.3.3), so even
  where it executes it need not actually idle the core.

So the two are not the same kind of fact, and it is worth not flattening them: RX
**forbids** it, RISC-V merely declines to **guarantee** it. What the fleet actually rests
on is the weaker, portable claim -- a U-mode idle cannot rely on WFI across these parts,
because whether it sleeps, no-ops or traps is a per-platform choice.

An unprivileged idle would therefore have to trap into the kernel on every iteration just
to sleep the core, which inverts the point of the cheapest loop in the system. So `idle`
stays a kthread on hardware grounds, and it is a well-behaved one: it holds no
authority, touches no application memory, and is the smallest possible
body of code to have on the wrong side of the boundary. Every *other* privileged
thread is a choice somebody made, and choices are the ones worth auditing.

## The consequence: the privileged population is fixed at boot

Put the spawn rule together with the exemption and a strong, non-obvious property
falls out.

Creating a privileged thread requires already being one, and that authority is
deliberately not a capability -- so it cannot be delegated, narrowed, or acquired.
Therefore **once the first application thread is created unprivileged, no privileged
thread can come into existence for the rest of the system's life.** The privileged
population is exactly what the boot path created; it can shrink when one of them
exits, and it can never grow.

That is a much better property than it first looks. It means the set of code running
inside the kernel's protection domain is enumerable *from the boot path alone* -- you
read `kmain`, you have the complete list, and nothing the application does at any
later point can add to it. No dynamic escalation to reason about, no "and then this
service is started privileged" to discover at run time. The hole set is a static
property of the image.

It also makes the direction one-way, which is the right direction to be one-way in:
confinement, once entered, is not exitable.

## The authority word: shape and seat

The mechanism for axis 3, concretely.

**A byte on the thread, in padding that already existed.** The authority word is one
`uint8_t` on the TCB, sited in alignment padding the struct was carrying anyway. It
costs zero bytes per thread and zero capability-table slots. That arithmetic matters
more than it sounds at the bottom of the fleet, where a whole per-task table is seven
handles: spending one of them on authority would be spending it on every board, in
every thread, for the life of the system, to hold eight bits -- and the parts that can
least afford it are exactly the ones a bring-up task runs on.

**Why not a capability, with the capability machinery right there.** A capability
*names* something. Its table entry is eight bytes because it has to carry a
generational handle into an object pool, a type, a rights byte and a generation, and
those fields exist to answer one question: is the thing this entry names still the
thing it named when you were given it? Authority asks no such question. It names no
object, so there is no handle. Nothing can free it out from under its holder, so there
is no refcount and no generation to bump. Delete the fields that do not apply and what
is left is a rights byte with nothing behind it -- eight bits in an eight-byte costume,
paying a well-known index on every board to hold what a spare byte on the thread holds
for nothing.

And the fields that do not apply are not merely idle, which is the sharper half of the
argument. A structure carries its own hazards, and delegation is where they bite:
spawn copies a source entry's handle and type into the child verbatim, filtered by a
rights bit, because for a real capability that copy *is* delegation. An authority entry
travelling that path would arrive in the child as a seat the parent never granted, so
the design would have to grow a refusal at that one site and then keep it -- a rule to
be re-derived every time the delegation loop is touched, guarding against a copy that
is only meaningful for the types it is not. With the word off the table there is no
entry, so there is nothing for the copy to copy. **The cheapest way to close a forgery
path is to not have the field.**

That generalises into a test worth running before putting anything in a capability
table: does this thing name something whose liveness the holder could otherwise get
wrong? If it does not, the table is charging you its costs -- an index, a copy path, a
type arm in every switch -- and handing back none of its properties.

**The discipline travels without the table.** What made the capability answer
attractive was never the eight bytes. It was three rules, and each applies to a plain
field unchanged:

- *A value, not an identity.* The word is data, so there is an operation that takes
  some of it away. A boolean has nothing to take.
- *Monotone in both directions.* A spawn may seat a word on a child, and the kernel
  refuses any bit the parent does not itself hold; a narrow applies `word &= mask`,
  which can only clear. Authority shrinks on the way down and on the way forward, and
  never widens on either -- the same rule a delegated rights mask obeys.
- *One chokepoint.* Every gate is a single call, so the answer is computed in one
  place; see below.

The one thing the table would have supplied for free is a *name* -- a handle userspace
can pass in. Narrowing needs one, so the ABI keeps a **pseudo-handle**: a constant that
names the authority word and nothing else, chosen as a value the handle codec provably
cannot produce, so no real capability can ever be mistaken for it and it can never be
mistaken for one. One constant, and a call site that narrows an authority reads exactly
like a call site that narrows a capability.

**Separate fields, separate numbering.** Object rights and authorities are different
enums in different structures. That is a small thing that pays every time either set
grows: nothing has to keep the two families disjoint, and no addition to one has to be
checked against the other. Sharing a byte with the object rights is the tempting
arrangement and the wrong one -- three of its bits are already spent (`CAP_WAIT`,
`CAP_SIGNAL`, `CAP_TRANSFER`), so the authority set would be capped at five for the
life of the type, and the sixth would have to come from merging two, fusing precisely
the powers that were worth separating. What bounds the width here instead is the byte
itself and the spawn parameter that carries a mask to a child; the exact width is
[`../reference/architecture.md`](../reference/architecture.md).

**Class granularity, forced by arithmetic rather than taste.** The alternative was a
capability per muxable pin, per clock, per line. The larger parts in the fleet have on
the order of a hundred muxable pins against a sixteen-slot table ceiling. It does not
fit, and it does not nearly fit, so one bit per authority *class* is not a
simplification anyone chose for elegance.

**Non-delegable, and not by a rule anyone has to remember.** An authority word is not
in a child's table because it is in nobody's table: there is no entry for a delegation
to copy, and no rights bit that could have made one copyable. A child's word comes from
exactly one place -- the mask on the spawn that creates it -- and the kernel is its only
writer. Structural impossibility is worth more than a correct refusal, because a
refusal is a line of code that a later reader can find redundant and delete.

**One question at every gate.** Every gate asks
`cap_check_authority(caller, AUTH_x)`, which is true when the caller is privileged
**or** holds that bit. "Privileged implies every authority" is therefore stated once,
inside that function, instead of once per call site. Two things fall out of that. The
set of authority decisions in the kernel is enumerable by grepping one name, which is
the only way a claim like "these are all the gates" gets checked rather than asserted
-- and a count that was right the day it was written is exactly what lets the next
decision hide. And a privileged caller takes the same arm it would have taken with no
authority bits in the system at all, so every gate is exercised by the whole fleet from
the moment it is written, instead of lying unexercised until the first thing that needs
the other arm.

## Why this is worth doing at all: revocation

Confining a bring-up thread's *memory* is worth something, but it is not the prize.
The prize is that **an authority can be dropped.**

An authority carried in a boolean is a property of the thread for the thread's whole
life. A bring-up body muxes the board's pins once, during setup, and then has no
legitimate need for `AUTH_PINMUX` ever again -- but with a flag it keeps it, through
the entire run of the application, including every path through application code that
has since gone wrong. There is no operation that takes it away, because there is
nothing to take away: the bit *is* the thread's identity.

An authority carried in a word is a value. Dropping one is a mask applied to a single
byte, which is a small operation precisely *because* the authority was made into data.
The holder names its own word by the pseudo-handle and hands in a mask; the mask can
only clear bits. The kernel refuses the corresponding gate from that instant, and it
refuses it uniformly, because every gate was already reduced to the same question asked
in the same place.

That narrowing call is itself **ungated**, which looks like a hole and is the only
coherent choice: an authority required in order to drop authorities could never be
given up, since spending it would be the one thing it exists to permit. A gate is
there to stop a thread doing something it should not have the power to do, and no
threat model objects to a thread holding *less*. Monotone-downward operations do not
need permission.

The narrow-only rule that makes this safe is not machinery of its own either -- it is
the same monotone rule the object capabilities use. A spawn may seat an authority word
on the child, and the kernel refuses any bit the parent does not itself hold:
authority narrows on the way down and never widens, exactly like a delegated rights
mask. One rule, both families, checked in one place.

And that is the difference the whole exercise buys. Without a droppable authority, an
unprivileged application thread can still ask the kernel for every one of those acts,
which would make confining it a memory-isolation change and nothing more.

## The consequence for anything you want to prove

The axes above have a blunt corollary for validation, and it is the trap that gives
this chapter its practical half: **a thread on the bypass side of the protection unit
cannot prove anything about the protection unit.**

Consider a full-C++ smoke test -- throw, catch, unwind a local destructor, grow a
`std::vector`, `dynamic_cast`, `typeid` -- printing `ALL PASS` under a boot banner
reading `mpu     enforce`. The tempting headline writes itself: *full C++ under MPU
enforcement, proven.* The headline is wrong, and not because any check is faulty --
every assertion can be sound -- but because of *where the code ran*.

`main()` is not the reset entry point. The reset vector runs the kernel's own
bring-up, which creates the idle thread and the system's first application thread and
starts the scheduler; that first thread runs the app and library constructors and
then calls the app's `main`. So `main` executes inside a thread whose privilege was
decided by the boot path, on axes 1 and 2, before a line of application code ran --
and if that thread is privileged, every load and store `main` performs is on the
bypass side of the unit. Such a pass shows only that the runtime *coexists* with a
protection unit that happens to be switched on. It shows nothing about isolation,
because not one of those accesses was ever protection-checked.

The general trap -- "the MPU is on" mistaken for "the MPU protects" -- is not a C++
one. It applies to any claim that a memory boundary is enforced. And notice what
axis 1 says about the fix: the privilege of the thread your test body runs in is not
something the test body can inspect by reading a banner. It was set in a fabricated
frame at creation. If you want confinement, you create a thread that has it.

### Two independent things must both hold

"Memory protection works" is really two claims, and a rigorous test has to establish
each one separately. Passing one does not imply the other.

**1. Confined execution -- run it where accesses are actually checked.** The code
under test must run in an **unprivileged** thread, so its every load and store is
subject to the protection unit. In KickOS that is
`kos::thread::spawn(..., privileged = false)` -- the default. `cxxtest` does exactly
this: rather than run the body inline, it spawns `cxx_worker` unprivileged
(`user/apps/common/cxxtest/main.cc`) and the throw/catch/unwind/RTTI/STL all execute
there, under the unit, reaching only the worker's granted regions. `selftest`
(`user/apps/common/selftest/main.cc`) is built the same way: its workers are spawned
unprivileged, and its `main` is a pure orchestrator that spawns, joins on a semaphore
and asserts -- it never does the thing it is testing.

What "unprivileged" means concretely is the axis-1 table at the top of this chapter.
On every ISA that has the distinction, the kernel and its trap handlers deliberately
sit on the bypass side, because that is the level that can reach kernel memory and
program the protection unit at all. So a test running in a kernel-domain thread is
testing the side that was never fenced. Only the spawned, unprivileged worker is
behind the fence.

**2. Proving the negative -- a wild access must FAULT, not silently succeed.**
Confined execution is necessary but not sufficient. A test that only performs
*allowed* accesses -- reads and writes inside its own grants -- can pass **even if
the protection unit is misconfigured or absent**, because nothing it does was
supposed to fault. `ALL PASS` from a well-behaved confined worker tells you the
runtime *fits inside* its grants. It does not tell you the **boundary bites**. For
that you must assert that an **out-of-grant** access **traps**.

KickOS proves this with a dedicated binary, `mpu_fault`
(`user/apps/common/mpu_fault/main.cc`). An unprivileged domain-A thread writes its own
granted region (must succeed), then writes domain B's region (must fault). The kernel
reports `MPU FAULT` and shuts down; the ctest asserts that the fault marker appeared
-- and negatively asserts that the "cross-domain write completed" wording did **not**
(that wording is the no-op-unit path on privilege-only boards). It is a *separate*
binary precisely because proving the negative *ends the process*: you cannot both
assert-the-trap-fired and continue a test suite in the same image.

A subtle detail in `mpu_fault` shows how strict "confined" has to be: the worker
receives its two region pointers through its **thread argument** (a struct placed in
region A, which it is granted), never through a file-scope global. Under real
enforcement an unprivileged thread has **no access to the kernel's `.data`/`.bss`** --
so a globals-based version of this test could fault reading its own setup before it
ever attempted the cross-domain write, and you would misread that as the boundary
biting when it was really the setup misfiring. The only memory the worker touches is
its code (granted RX), region A (granted), and its own stack. That discipline is what
makes the trap it does hit *unambiguous*.

`selftest` carries the positive companion to `mpu_fault`'s negative: `domain_share`
(two unprivileged threads granted the *same* region read/write it and see each other's
stores), the confused-deputy check, and the unprivileged IRQ-as-event driver that reads
only its granted MMIO. What it deliberately does *not* carry is an arm asserting
something about a privileged caller, and the reason is the axis split itself: with the
privileged population fixed at boot to the one thread that runs no tests, a test needing
a privileged caller has nobody to run as. The claim it would have made is made from the
other side instead, by an app that watches an unprivileged worker trap on a cross-domain
write. When a test cannot exist by construction, replacing it with a weaker one is worse
than admitting the shape of the gap.

Resist the temptation to call that worker, or any thread, the system's *least
privileged*. There is no such thread, and the phrase is this chapter's own error in
miniature: a superlative over "privilege" presumes the three axes collapse into one
ordering, and they do not. The thread the system starts userspace with is the sharpest
demonstration available. It runs unprivileged on axis 1, exactly like every thread it
goes on to spawn, and it holds *every* authority bit on axis 3, necessarily so: a spawn
may only narrow the authority word it passes down, never widen it, so the root of the
tree has to start holding everything any descendant will ever need. Ranked on the
CPU-mode axis that thread is indistinguishable from a leaf worker; ranked on the
authority axis nothing in the system outranks it. Any single ordering claiming to place
it has quietly picked one axis and discarded the other two.

Together the positive (allowed accesses succeed from the checked side) and the negative
(a disallowed access traps) are what make "protected" a claim and not a hope.

### Coexistence vs confinement -- do not conflate them

Running a full-C++ test image with the unit switched on establishes something real,
but weaker than the headline: the runtime -- libstdc++/libsupc++/newlib, the
EH-table-homing layout, the boot-time frame registration on DWARF arches -- **boots
and runs to completion with enforcement active**. That is *runtime/unit coexistence*:
the enforcement machinery does not break the runtime, and the tables and writable
floor are homed such that a full-C++ image links and runs on an enforcing board at
all. What it is *not* is proof that an unprivileged thread is *confined*.

So the two are stated separately, because neither implies the other:

- **Confined execution** -- the code under test runs unprivileged, so its accesses
  are actually checked, and it stays inside its grants. A spawned unprivileged
  `cxx_worker` establishes this: the fence is up and the confined code lives inside
  it.
- **Coexistence** -- the runtime and the enforcement are active at the same time and
  nothing breaks. A privileged run establishes this and nothing more; it says nothing
  about confinement, because the privileged side is never fenced.

And enforcement itself -- that the boundary *bites* -- is a third, independent claim
that does not lean on the C++ test at all: it rests on `mpu_fault` (the negative), on
`selftest`'s positives, and on the unprivileged peripheral drivers that reach only
their granted MMIO.

## The transferable rules

Three, in the order they bite:

- **Separate the axes before you reason about privilege.** Ask which of CPU mode,
  memory posture and authority a given claim is really about. A design that answers
  all three with one flag cannot express "may configure a pin, may touch nothing
  else", which is the shape almost every bring-up task actually has.
- **Put authority in something you can take away.** A power stored as an identity
  lasts as long as the identity. A power stored as data can be narrowed, and the
  narrowing is small precisely because it is data. The one exception worth making is
  the authority whose narrowing would be meaningless -- the power to create more
  privileged threads -- and that one is better left un-nameable than made delegable.
- **Do not run code you want to prove confined in a thread you did not create
  unprivileged.** Run it in a spawned unprivileged worker, and then *separately* prove
  the boundary bites with a test whose whole job is to make a wild access **fault**.
  Two tests, two claims: a confined worker doing only allowed things shows the runtime
  fits inside its grants; a confined worker doing one forbidden thing shows the grant
  boundary is enforced. Neither implies the other, and neither is implied by a banner
  that says `mpu enforce`. "The unit is on" is a statement about configuration. "The
  unit protects" is a statement you have to *earn*.

## Where to go next

- The user-to-kernel-and-back arc, where the transient privilege of a trap is lent
  and returned: Chapter 3.9,
  *[The syscall path: trap, dispatch, return](the-syscall-path-trap-dispatch-return.md)*.
- The saved state that axis 1 lives in: Chapter 3.5,
  *[Context switching and the silicon contract](context-switching-and-the-silicon-contract.md)*.
- What a confined throw actually touches at runtime (the four pieces of memory, the
  three EH models):
  [`exceptions-and-rtti-under-memory-protection.md`](exceptions-and-rtti-under-memory-protection.md).
- Where the writable floor comes from and how the linker splits kernel from app:
  [*Where your RAM goes*](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md).
- Why the permissive privileged background is not free even for the kernel:
  Chapter 7.6, *[The CPU reads ahead](memory-types-and-speculative-access.md)*.
- The capability machinery axis 3 borrows its discipline from: Chapter 8.1,
  *[Naming a kernel object](handles-and-the-resolve-chokepoint.md)*, and what actually
  goes in that table, one type at a time: Chapter 8.2,
  *[Adding a kernel object type](adding-a-kernel-object-type-the-additive-recipe.md)*.
- The exact contracts: [`../reference/architecture.md`](../reference/architecture.md)
  ("User/kernel separation", "Memory domains", "Object model, capabilities & IPC") and
  [`../reference/invariants.md`](../reference/invariants.md) (the syscall/privilege
  section).
