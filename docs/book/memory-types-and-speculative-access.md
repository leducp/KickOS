<!-- SPDX-License-Identifier: CECILL-C -->
# The CPU reads ahead: memory types and speculative access

> Why the memory map is a contract with the *core* about what it may touch on its
> own initiative -- not only a permission list for the program. A modern core
> issues accesses the program never asked for; the map's job is to tell it which
> addresses are safe to touch that way. Get the *type* of a region wrong -- even
> with the permissions right -- and the core can wedge with no fault to point at.
> This chapter builds on Chapter 3.5 (the silicon contract a port must satisfy)
> and sits beside Chapter 7.3 (read the hardware honestly) and Chapter 7.5
> (protection follows the CPU). It points into `../reference/architecture.md`
> (Memory domains -- the background region and the region-set contract) and
> `../reference/boards.md` (per-chip memory-map quirks) for the exact contract.

## The CPU does not wait to be asked

A simple mental model of a CPU is that it touches memory only when the program
tells it to: this load, that store, the next instruction fetch. On any core with
a cache and a prefetch unit that model is false. To keep its pipeline fed, the
core issues accesses *ahead* of provable need -- it prefetches instructions past
a branch it has not resolved, pulls cache lines it predicts it will want, reads
around an access to fill a whole line. These are **speculative** accesses:
memory traffic the architecture permits the core to generate on its own
initiative, before -- or even without ever -- the program committing to needing
the result.

This is true even of an *in-order* core, which is worth stressing because
in-order is often read as "does exactly what the program says, in order." It
retires *instructions* in order, but it still prefetches and fills cache lines
speculatively; the ordering guarantee is about results becoming visible, not
about which addresses the bus sees. So "the program never dereferences that
address" is not a guarantee the core never *accesses* it.

*Further reading: Hennessy and Patterson, Computer Architecture: A Quantitative
Approach, ch.2 (memory hierarchy, prefetching) -- why a core reads ahead to hide
latency; Tanenbaum, Modern Operating Systems, ch.3 (the memory system the OS
must describe to the hardware).*

## Why Normal memory is fair game and Device memory is not

Speculation is only safe where an access has **no side effect**. Reading a word
of RAM twice, or reading a word you turn out not to need, changes nothing:
memory is idempotent under reads. Reading a peripheral register is the opposite
-- a read can pop a FIFO, clear an interrupt flag, advance a state machine. A
core that speculatively read such a register would corrupt device state for a
read the program never actually performed.

So architectures let the memory map tag each region with a **memory type**, and
that tag is a contract with the core about what it may do on its own:

| Type | Read side effects? | Core may speculate / reorder / cache? |
|---|---|---|
| **Normal** | none | **yes** -- prefetch, cache, reorder, read-around all allowed |
| **Device** | possible | no speculation, accesses kept in order to the region |
| **Strongly-ordered** | possible, strict | no speculation, no reordering, no early completion |

The names are ARM's; every architecture has the same two-or-three-way split
under some spelling. The rule the core follows is uniform: **Normal means "do
whatever you like here," Device and Strongly-ordered mean "touch this only when
and exactly as the program says."** RAM and ROM are Normal; the peripheral
aperture is Device. Typing a region is therefore not a permission (who may
touch it) but a *behavioral* statement (how the core is allowed to touch it).

## The fault that isn't: an unbacked Normal window

Here is where a subtle map bug turns into a lockup with no diagnostic.

Give the core a large address window typed **Normal**, but back only part of it
with real memory. The classic shape is execute-in-place from external flash: the
architecture's default map types a whole gigabyte-scale external-memory window as
Normal, while the board populates only the first few megabytes. Everything above
the populated span is Normal-typed address space with **no responder** on the
bus behind it.

Now run code near the top of the populated region. The core, doing exactly what
Normal permits, prefetches *past* the real memory into the unbacked span. That
speculative access goes out to the bus and reaches a slave that was never wired
up -- so it **never returns a response**. And because the core retires in order,
the current, already-fetched, perfectly valid instruction cannot retire behind
the outstanding access. The pipeline stalls. Forever.

The cruelty is that **nothing faults**. A permission violation faults; a bad
type on *backed* memory faults; but an access that never *completes* has nothing
to fault on -- there is no error response, just silence. The program counter
sits frozen on a valid instruction, no fault status is set, and only an
unrelated interrupt (whose fetch path is backed) can break the core out. The
symptom -- a thread's first instruction never retiring, revived only by a
preempting IRQ -- reads as impossible until you see that the killer is a
speculative access the program never issued, to an address it never named.

The lesson in one line: **an unbacked region typed Normal is a trap, because the
core is entitled to touch it and the bus is entitled to never answer.** The
permission bits are irrelevant; the bug is the *type* over *unbacked* space.

## The design rule: the map describes what is really there

The fix is not clever, it is honest bookkeeping: **the memory map must describe
the silicon, not the architecture's default address space.**

- **Bound real memory as Normal.** Type exactly the populated span (the real
  flash image, the real RAM) as Normal, cacheable, with the permissions it
  actually needs -- this is what makes it fast and speculatable, which you *want*
  for code and data.
- **Wrap everything else in that window as Device or Strongly-ordered, and
  execute-never.** The unpopulated remainder of an external-memory aperture, and
  any aperture with no device behind it, must be typed so the core will never
  speculate into it. Device typing alone stops the speculative prefetch;
  execute-never stops an instruction fetch from ever being aimed there;
  no-access on top means even a committed access is rejected cleanly (a fault you
  can see) rather than sent to a silent bus.

The default map the architecture hands you is a *generic* description of the
address space the ISA can address -- not a description of your board. Trusting it
is trusting that every gigabyte the ISA can name is safe to prefetch, which is
exactly the assumption an unpopulated aperture breaks.

## The MPU corollary: you cannot lean on a permissive background

This is where the memory-type contract collides with memory protection, and why
it belongs in Part 7.

An MPU is usually run with a *permissive privileged background*: rather than
spend scarce region descriptors describing the kernel's own access, the kernel
leans on a default map that grants privileged code the run of the address space
(on ARM this is the `PRIVDEFENA` background region; see
`../reference/architecture.md`, Memory domains). That default map is precisely
the architecture's generic map -- the one that types the whole external window
Normal. So a kernel that leans on the permissive background **inherits the
mis-typing**: privileged code, and any thread that falls through to the
background, runs with the unbacked aperture typed Normal, and is one speculative
prefetch away from the stall above.

The consequence is a real constraint on the design, not an incidental one: **on
a core that speculates, the kernel cannot simply lean on a permissive background
map -- the background must first be corrected to type real memory as Normal and
everything else as Device.** The way to do that without giving up the
region-budget win of the background is a set of *chip-invariant* regions that
re-type the true memory map, programmed once and sitting *beneath* the
per-thread grants (so a legitimate grant still overrides them; Chapter 7.5 is
the timing half of why the grant/background ordering matters). The per-thread
protection story is unchanged; what changes is that the background it rests on
must now tell the truth about what is really on the bus.

## The general silicon-contract lesson

Step back from the MPU and the flash aperture and the lesson is the one Chapter
3.5 opened with: a port's job is to **satisfy the contract the silicon dictates**,
and the memory map is part of that contract. It is easy to read the map as a
permission list aimed at the *program* -- who may read what. It is also, and
independently, a behavioral contract aimed at the *core* -- what it may do on its
own initiative, before the program asks. Speculation is the core exercising the
second contract, and a region's type is the clause that governs it.

So the same honesty Chapter 7.3 demands about what the hardware can *enforce*,
this chapter demands about what the hardware will *do unbidden*: read the core's
speculation rules, describe the real memory map to it, and never assume the
architecture's default address space matches the board. A permission bug faults
and tells you where. A memory-type bug over unbacked space does not fault at all
-- which is exactly why the map has to be right before anything runs on it.

## Where to go next

- The contract a port must satisfy, of which the memory map is one clause:
  Chapter 3.5, *Context switching and the silicon contract*.
- What the protection unit can and cannot enforce, and reading the hardware
  honestly: Chapter 7.3, *Peripheral isolation and the hardware ceiling*.
- The ordering between the chip-invariant background and the per-thread grants,
  and why a resource keyed to the current context switches at the physical
  switch: Chapter 7.5, *Protection follows the CPU, not the scheduler's intent*.
- The exact contract: `../reference/architecture.md` (Memory domains -- the
  background region and the region-set contract) and `../reference/boards.md`
  (per-chip memory-map quirks).
