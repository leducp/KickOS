<!-- SPDX-License-Identifier: CECILL-C -->
# Context switching and the silicon contract

> The conceptual chapter behind the per-ISA context-switch
> sections (Chapter 4). It teaches *what* a context switch must preserve and *why*
> that forces assembly and a precise CPU/silicon contract; each ISA's concrete
> realisation (register set, trap entry, exception-return instruction) is a section
> of Chapter 4. Points into `../reference/invariants.md` for the exact, code-synced
> contract -- this chapter explains, the invariants bind.

## What a context switch actually is

A thread is not a thing the CPU knows about. To the silicon there is only *the
machine state*: the program counter, the stack pointer, the general registers, the
status/flags, and (where present) the floating-point registers and a
privileged/unprivileged mode bit. A "thread" is a saved copy of exactly that state,
plus the memory it is allowed to touch.

A context switch, then, is the act of **freezing one thread's machine state into
memory and thawing another's back into the CPU**, such that the resumed thread
cannot tell it was ever stopped. Every scheduler decision, every blocking
`sem_wait`, every preemption ultimately funnels into this one operation.

The subtlety -- and the reason this chapter exists -- is that the switch runs *on
the very CPU whose state it is rewriting*. It must save the outgoing registers using
those same registers, swap the stack pointer out from under itself, and hand control
to a thread whose stack and privilege differ from the one currently executing. That
is a fundamentally different kind of code from everything else in the kernel, and it
is why it cannot be written in C.

*Further reading: Tanenbaum, Modern Operating Systems, ch.2 (processes/threads and
the dispatcher).*

## Why C cannot express it

C is defined in terms of an *abstract machine*: you get variables, a call stack, and
a calling convention (the ABI) that says which registers a function may freely
clobber and which it must preserve. The compiler is free to use registers however it
likes within those rules. That freedom is exactly what the context switch cannot
tolerate. Four concrete conflicts:

1. **The exact register set, in a pinned order.** The switch must save *all* of a
   thread's live registers -- including the ones the ABI calls "scratch," because a
   preempted thread had live values in them. And the order/offsets it writes them in
   must byte-match the frame that `arch_context_init` fabricates for a brand-new
   thread, or the first switch-in restores garbage. A C compiler guarantees neither
   *which* registers it touches nor *where* on the stack it puts them. (Contract:
   [`switch-frame-matches-init`](../reference/invariants.md).)

2. **Swapping the stack pointer mid-flight.** The switch changes SP from the
   outgoing thread's stack to the incoming thread's. In C, locals, spilled
   temporaries, and the return address all live relative to SP; move it inside a C
   function and the function's own state evaporates. The switch has no such state to
   lose because it keeps everything in named registers by hand.

3. **Special registers and the mode transition.** Selecting the thread stack vs the
   handler stack, reading/writing the privilege bit, and (on a trap) restoring the
   saved status word are done through registers and instructions C has no syntax for
   (`CONTROL`/`PSP` on ARM, `mstatus` on RISC-V, `PS`/windowed registers on Xtensa).
   The privilege a thread resumes at is itself part of the saved context -- a thread
   preempted mid-syscall must resume privileged. (Contract:
   [`npriv-banked-on-switch`](../reference/invariants.md).)

4. **The exception-return is one atomic, hand-chosen instruction.** Returning to a
   thread is not a `return`; it is a specific instruction (`bx lr` with an
   `EXC_RETURN` magic value on ARM, `RTE` on RX, `mret` on RISC-V, an `rfi`-class
   sequence on Xtensa) that atomically restores PC + status + mode. No
   interrupt-takeable window may exist while the return state is staged, or a nested
   trap overwrites it. That atomicity is a property of the exact instruction
   sequence, not something a compiler will preserve. (Contract:
   [`trap-return-atomic`](../reference/invariants.md).)

The one-line way to hold all this: **the context switch is the code that *creates*
the stable register-and-stack context that C code assumes already exists.** It sits
one level below the C abstract machine, so it is written in the machine's own
language. This is not a KickOS quirk -- every operating system has an assembly
context switch for exactly these reasons.

## The silicon contract

"Save the registers and swap the stack" is the same *idea* on every CPU, but the
*contract* -- which registers exist, how a trap is entered, how privilege is
represented, how the return is spelled -- is dictated by the architecture. A port's
job is to satisfy that contract; the kernel above the seam never sees it. The axes
that differ:

- **The register file.** How many general registers, whether floating-point state is
  a separate file or lives in the GPRs, whether there is a link register or the
  return address is on the stack.
- **How a trap/exception is entered.** Some cores push part of the frame in hardware
  on exception entry (ARM stacks 8 registers automatically); others save nothing and
  the software saves everything. This decides how much the assembly must do.
- **How the deferred switch is triggered.** A switch requested from an interrupt must
  not happen until the interrupt and any held critical section finish. Cores provide
  a lowest-priority "do it on the way out" mechanism (ARM PendSV, RX SWINT, a RISC-V
  software interrupt via the CLINT); the switcher pends it and lets it tail-chain.
  (Contract: [`deferred-switch-lowest-band`](../reference/invariants.md).)
- **Floating-point.** Whether FP registers are banked by a mode bit, saved lazily on
  first use, or must be spilled explicitly -- and whether the switch or the compiler
  is responsible. (Contracts: the `fp-*` family in the invariants.)
- **Privilege.** A single unprivileged/privileged bit and how a syscall crosses it.

KickOS holds *one* semantic model across all five ISAs and lets only these
silicon-mandated details differ. When two arches diverge on anything that is *not*
on this list, that is a bug, not a port quirk -- the uniform-fleet thesis.

## The cost of a switch, and where an ISA pays it

The axes above are usually read as correctness knobs -- get the register set and the
return instruction right and the switch *works*. But two of them, **the register
file** and **how a trap is entered**, also set the switch's *price*, and that price
is invisible until the switch becomes the workload.

The decisive split is hardware exception stacking. When a trap is taken, some cores
push part of the frame in hardware before any handler instruction runs; others push
nothing. On a core that stacks the caller-saved half automatically (ARM's Cortex-M
hardware stacks eight words -- the argument/scratch registers, the return address,
the status word), the deferred switcher only has to *software*-save the callee-saved
half -- on ARMv7-M that is nine words out (`r4`-`r11` plus the exception-return
token) and nine words back. On a core that stacks nothing, the trap handler must
software-save the **entire** integer file before it can even look at the cause
register: the full general-purpose set plus the saved PC and status. That is roughly
twenty-eight registers out and twenty-eight back.

Both designs are the *same* design -- a lowest-priority software-interrupt swap
(Contract: [`deferred-switch-lowest-band`](../reference/invariants.md),
[`arch-switch-may-defer`](../reference/invariants.md)). What differs is only who
moves the caller-saved half: dedicated hardware, or the handler in software. The
result is about a threefold difference in the number of words the *software* moves
per switch, on otherwise comparable cores. It is not that one ISA "does the switch in
software and the other in hardware"; both defer the swap to an exception. It is that
one exception mechanism does half the copying for free.

**Why a handoff-heavy workload exposes it.** Most programs do real work between
switches, so the switch cost is amortized to noise. A workload that does almost
nothing *but* switch -- two threads ping-ponging through a semaphore, each `wait`
handing the CPU straight to the other -- puts the switch in the denominator of
throughput. Then a threefold switch-cost gap shows up almost undiluted as a
threefold throughput gap, even when the slower core has the faster clock. This is
just Amdahl's law aimed at the dispatcher: optimize the thing you spend all your time
in. *Further reading: Tanenbaum, Modern Operating Systems, ch.2 (the dispatcher and
dispatch latency).*

**How to make a switch cheap.** Four levers, in the order they usually matter:

1. **Save only what the path actually needs.** A *voluntary* switch (a thread
   blocking in `sem_wait`, sleeping, yielding) reaches the switcher through an
   ordinary function call, so the ABI has *already* spilled every caller-saved
   register the thread still cares about -- by definition, those registers are dead
   across a call. Such a switch needs to preserve only the callee-saved set. A
   *preemptive* switch interrupts arbitrary code and must save everything, because
   nothing spilled the scratch registers. A core with hardware stacking gets this
   split handed to it (the hardware saves the caller-saved half, the switcher saves
   the callee-saved half) -- which is exactly why one unified frame format can be
   both cheap *and* simple there. A core with no stacking that also uses one unified
   frame pays the full-file save on the voluntary path too, where most of it is dead
   registers. The lever is to give the voluntary path its own callee-saved-only save
   and reserve the full-frame trap save for genuine preemption. It is a real trade,
   not a free win: it buys speed by giving up the one-frame-fits-both simplicity --
   one restore path, one fabricated first-resume frame -- that
   [`switch-frame-matches-init`](../reference/invariants.md) rests on. Whether the
   speed is worth the second frame format is a per-fleet judgement, not a default.

2. **Reprogram only what changed.** When the switch also reloads a
   memory-protection unit, doing it unconditionally taxes every switch -- including
   the common case of two threads in the same protection domain, whose region set is
   identical. Reload the protection registers only when the incoming region set
   actually differs from what is programmed, and skip the ordering fence when nothing
   changed. (This lever only exists once protection is enforced on the switch; see
   Chapter 7.5.)

3. **Save floating-point lazily.** Where FP state is a separate register file gated
   by a mode bit, leave it disabled and let the first FP instruction trap in the
   state -- so a thread that never touches FP carries no FP frame. (Contracts: the
   `fp-*` family in [`../reference/invariants.md`](../reference/invariants.md).) The
   lever exists only where there *is* an FP file; a soft-float ISA with no FP
   extension already pays nothing here.

4. **Keep the frame in fast memory.** The switch touches the saved frame twice --
   once to save, once to restore. If a thread's stack lives in memory with wait
   states or bus arbitration, every one of those accesses pays them, and the
   full-file save multiplies the penalty by the register count. This is second-order
   behind lever 1 (fewer words is fewer accesses), but it is the residual that
   distinguishes two chips sharing the *same* ISA switch code: same register count,
   different memory system.

The one-line way to hold this: **a switch costs as many words as it moves in
software, which is set by how many the hardware moves for free on trap entry.** You
make it cheap by saving only what the path needs (callee-saved on a voluntary
switch), reprogramming only what changed, and keeping the frame in fast memory --
and you cannot make a no-stacking core as cheap as a hardware-stacking one *without*
giving up the single-frame simplicity, which is a design choice, not an oversight.

## Startup: the same argument, at the other end

The first instructions after reset have the mirror-image problem. Before the C
runtime exists there is **no valid stack pointer and no initialised `.data`/`.bss`**,
so C code literally cannot run yet: a function call needs a stack, and any global it
touches is uninitialised. So the reset entry must, in assembly, set the stack pointer
and hand off to C as soon as possible.

KickOS keeps that assembly to the bare minimum: on the chips brought up for
memory-protection work (K64F, XMC, ESP32-C6) the `Reset_Handler` is *C* -- the
assembly is only "set SP, jump here" -- and the C handler does the rest (init the
data ranges, run the C++ constructors, call `arch_init`, enter `kmain`). That is why
a cross-cutting change like the app-data range table (Chapter 7) could be dropped
into a shared C function every chip calls, with no assembly churn.

Some startups are irreducibly larger, and the reason is always the silicon:

- **Xtensa LX6 (ESP32).** The windowed-register ABI *requires* register-window
  overflow/underflow exception handlers, and the interrupt vectors must sit at fixed
  addresses with exact spill/fill encodings -- the CPU jumps to those fixed vector
  slots expecting that precise code. That is architecture-mandated assembly, and it
  is verbose because there are many fixed vector entries.
- Vector tables in general are fixed-layout data the hardware indexes on a trap; they
  live where the silicon says they live.

So the house style falls out of one rule: **C wherever the architecture permits,
assembly only for the parts the silicon dictates** -- the reset SP setup, the fixed
exception vectors, the windowed handlers, and the context switch itself.

## Where to go next

- The exact, binding contracts: `../reference/invariants.md` (the "Context switch &
  FP save/restore," "Deferred switch," and "Syscall, privilege & the user/kernel
  boundary" sections).
- Each ISA's concrete realisation -- register set, trap entry, timer, IRQ controller:
  Chapter 4, *Per-ISA guided tour*. A fully worked port is
  [`porting-a-new-isa-riscv.md`](porting-a-new-isa-riscv.md).
- The privilege/isolation half of a thread's context (which memory it may touch):
  Chapter 7, *Memory protection*.
