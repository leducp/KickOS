<!-- SPDX-License-Identifier: CECILL-C -->
# Context switching and the silicon contract

> **Status: DRAFT.** The conceptual chapter behind the per-ISA context-switch
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
