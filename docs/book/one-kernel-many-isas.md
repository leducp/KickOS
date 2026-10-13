<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# One kernel, many ISAs

> This chapter opens Part 4. It asks what a CPU family decides for a kernel, how
> those decisions differ across the families KickOS runs on, and how one kernel
> keeps the same behaviour over all of them. It teaches the general idea; the code
> of each port is walked in the Journey's architecture chapters, and the exact
> contract of the arch seam is in `../reference/porting.md`.
>
> *Further reading: Tanenbaum, Modern Operating Systems, ch.1 (section 1.3, the
> computer hardware review: processors, kernel and user mode, interrupts) and ch.12
> (operating system design, its discussion of hiding the hardware).*

## Why a kernel cares which CPU it runs on

Most of a kernel is ordinary C. A ready queue, a priority, a timeout list and a
table of handles look the same on every processor. A small part of the kernel
touches things C has no words for: the registers that make up a running thread,
the instruction that enters the kernel and the one that leaves it, the bit that
says "privileged", the timer that ends a sleep, and the unit that refuses a store.
The CPU family defines all of them.

That definition is the **ISA**, the *instruction set architecture*: the contract
between software and every CPU of one family. It fixes the instructions, the
registers, the privilege modes, and what happens on an interrupt or a trap. Two
chips with the same ISA share the kernel's code for these parts; two chips with
different ISAs cannot.

The ISA is not the whole machine. A chip adds clocks, peripherals and memory
around the core, and Part 5 covers those. The line between the two moves from one
family to the next. On a Cortex-M, the interrupt controller (the NVIC) and a small
timer (SysTick) are part of the architecture, so the same code drives them on every
Cortex-M chip. On RISC-V, the timer and the device interrupt controller belong to
the platform, and a chip may choose its own. A kernel has to know where that line
sits for each family before it knows what a port must supply.

The f411 example shows the stakes. `printf("hello\n")` goes through newlib, then
KickOS's `_write`, then a syscall stub whose last step is `arch_syscall`. On the
STM32F411 that stub executes `svc #0`. Build the same program for a RISC-V board
and the stub executes `ecall`; for an x86_64 PC, `syscall`; for a Renesas RX,
`int #1`. Everything above `arch_syscall` is the same source on every target. Only
the last instruction, and what the hardware does when it runs, changes.

## What an ISA fixes for a kernel

Seven questions cover what a kernel must learn from an ISA. Each comes with the
f411's answer as a reference point.

**The register file, and so what a context is.** To the CPU, a thread is a set of
register values: program counter, stack pointer, general registers, status word,
and floating-point registers if any. Saving and restoring that set is the context
switch. The ISA decides how big the set is, and whether part of it is hidden: Xtensa
LX6 shows 16 of its 64 registers through a sliding window, so a thread's context
includes windows not yet written back. The f411's Cortex-M4F has 16 core registers
and a separate FPU file.

**Privilege levels.** Isolation needs a level where the kernel reaches everything
and one where an application cannot. The ISA decides how many levels exist and how
code moves between them. On the f411, thread code is privileged or not by one bit
(`CONTROL.nPRIV`), and handlers always run privileged. A core with no levels cannot
enforce isolation, however its kernel is written.

**How a trap enters and how it returns.** The ISA decides where a trap jumps (one
entry, or a table indexed by cause), how the kernel learns the cause, and which
instruction resumes the interrupted code. That instruction must restore the program
counter, status and privilege together, with no gap for another interrupt. On the
f411 a trap goes through the vector table, and the return is a branch to a magic
value in `lr` (`EXC_RETURN`).

**Who stacks what.** Something has to save the interrupted state before the handler
uses the registers. Some ISAs push part of it in hardware, others nothing; some
switch to a kernel stack, others keep the interrupted stack pointer. The f411 pushes
eight words (more if the thread used the FPU) onto the thread's own stack.

**The interrupt controller.** Something has to enable, mask, latch and prioritise
device lines. The ISA may define it (the NVIC), leave it to the platform (RISC-V),
or offer a few fixed levels and let the chip route lines onto them (Xtensa's
interrupt matrix). A RISC-V platform usually splits it in two: the **CLINT**
(Core-Local Interruptor) raises each core's timer and software interrupts, and the
**PLIC** (Platform-Level Interrupt Controller) prioritises device lines and routes
each to a core. On the f411, USART2 raises NVIC line 38 and TIM2 line 28.

**The timer.** A tickless kernel needs a clock that only moves forward and a
one-shot timer that fires at a deadline. The ISA may provide both, one, or neither.
The f411 wakes the next sleeper with SysTick, which the architecture provides, and
reads time from TIM2, a chip timer.

**The protection unit.** Something has to refuse an application's store to memory
it does not own: a region MPU (a few address ranges with permissions), page tables
(a virtual-to-physical translation with permissions per page), or nothing. The ISA
also sets the polarity, whether an access no rule matches is allowed or refused. The
f411's PMSAv7 MPU refuses such an access from unprivileged code and allows it for
privileged code.

Chapter 3.5, *[Context switching and the silicon
contract](context-switching-and-the-silicon-contract.md)*, takes the first four as
the axes of the context-switch contract and explains why that code is assembly.
This chapter keeps all seven in view, across the families.

## The same questions, different answers

KickOS runs on Arm M-profile cores (ARMv6-M, ARMv7-M and ARMv8-M Mainline), on
AArch64, on 32-bit and 64-bit RISC-V, on x86_64, on Renesas RXv3 and on Xtensa LX6.
The set grows, and [`../reference/porting.md`](../reference/porting.md) is where it
is listed; the families here are the examples this chapter compares.

The first table is about entering and leaving the kernel.

| Family | Levels | System call | Saved by hardware on a trap | Return |
|---|---|---|---|---|
| ARMv7-M, ARMv8-M | thread privileged or not (`CONTROL.nPRIV`), handler always privileged | `svc` | eight words on the thread's stack | `bx lr` with `EXC_RETURN` |
| ARMv6-M | the same, but the privilege bit is optional: the Cortex-M0 has none | `svc` | eight words on the thread's stack | `bx lr` with `EXC_RETURN` |
| AArch64 | EL0 user, EL1 kernel | `svc` | nothing pushed; PC and state in `ELR_EL1` and `SPSR_EL1`, stack switched to `SP_EL1` | `eret` |
| RISC-V 32 | U user, M machine | `ecall` | nothing pushed, no stack switch; PC in `mepc` | `mret` |
| RISC-V 64 | U user, S supervisor | `ecall` | nothing pushed, no stack switch; PC in `sepc` | `sret` |
| x86_64 | ring 3 user, ring 0 kernel | `syscall` | `syscall` pushes nothing and keeps the user's stack pointer | `iretq` |
| RXv3 | user and supervisor (`PSW.PM`) | `int #1` | PC and PSW, on the interrupt stack | `rte` |
| Xtensa LX6 | none | a plain function call | PC and status into special registers | `rfe`, or `retw` for a switch |

The second table is about the hardware around the core.

| Family | Critical section | Interrupt controller | One-shot timer | Protection |
|---|---|---|---|---|
| ARMv7-M, ARMv8-M | `BASEPRI`, a priority threshold | NVIC | SysTick | MPU: PMSAv7, or PMSAv8 on v8-M |
| ARMv6-M | `PRIMASK`, all or nothing | NVIC, two priority bits | SysTick | optional MPU, or none |
| AArch64 | the interrupt mask in `DAIF` | GIC | the generic timer, `CNTP_CVAL_EL0` | page tables, a root per task |
| RISC-V 32 | `mstatus.MIE` | CLINT for timer and software interrupts, a platform controller for devices | CLINT `mtimecmp` | PMP, which refuses what no entry matches |
| RISC-V 64 | `sstatus.SIE` | the same split | `stimecmp` | page tables (Sv39) |
| x86_64 | the interrupt flag (`cli`) | a software controller, rung through a local APIC vector | local APIC timer, calibrated at boot | page tables, a root per task in `CR3` |
| RXv3 | `PSW.IPL`, a priority level | ICU | two compare-match timers | RX MPU, which checks user mode only |
| Xtensa LX6 | `PS.INTLEVEL`, raised with `rsil` | interrupt matrix onto 32 CPU interrupts at fixed levels | `CCOMPARE0`, an equality match | none driven |

Read the tables by column, not by row. Every family answers every question; what
changes is the answer. A critical section is a priority threshold on one core and
an all-or-nothing switch on another. A timer is an absolute comparator on most
cores, but on Xtensa it matches only on equality, so a deadline written too late is
missed until the counter wraps, and the port has to guard against that. Protection
is a region MPU on the small cores and a page table on the large ones, and its
polarity flips between Arm and RISC-V.

## Who stacks what

Of the seven questions, the fourth shapes a port the most. Its answer decides what
the trap entry must do before it can trust anything, and how much a context switch
costs.

At one end, a Cortex-M pushes eight registers in hardware onto the interrupted
thread's stack, then switches to a separate handler stack; the switch saves only
the other half of the context. At the other end, RISC-V pushes nothing and switches
nothing. The first handler instruction runs on whatever stack pointer was live,
which for an application is a value the application chose, so the handler must
decide whether that pointer is safe before it writes a single word through it, then
save every register itself. KickOS's RV32 port first swaps to a trusted per-hart
stack held in `mscratch` and checks the thread's pointer from there. x86_64's
`syscall` keeps the user's stack pointer too. AArch64 pushes nothing but always
lands on the kernel's own stack pointer, and RXv3 pushes two words onto its
interrupt stack.

Chapter 3.5 prices the difference. Chapter 7.7, *[Whoever stacks the trap frame
owns the bounds check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md)*,
explains why whoever writes the frame must also check the stack it writes to.

## How a kernel keeps one semantics

**Why.** A kernel that runs on many families can drift: each port fixes its own
problems, and a priority, a timeout or a fault comes to mean something slightly
different on each board. Chapter 1, *[Overview and
principles](overview-and-principles.md)*, states KickOS's answer: the same semantics
on every target, and a divergence is a bug, not a port quirk. This section is about
the mechanism that makes that hold.

**How one could do it.** The options form a range.

- **Conditional code in the kernel.** The scheduler and the syscall layer carry
  `#ifdef` blocks per family. This is quick for the second port and unreadable by the
  fifth, and nothing stops a fix for one family from changing the behaviour of
  another.
- **A kernel per family.** Each family gets its own copy of the parts that differ,
  and the copies drift by construction.
- **A hardware abstraction layer.** The kernel calls a fixed interface, and each
  family implements it below. Linux keeps its per-family code under `arch/`, and
  Windows NT named the idea a HAL. The question that decides the quality of such a
  layer is what its interface names: if it names mechanisms ("pend PendSV", "write
  `mtimecmp`"), the kernel still knows which family it runs on; if it names concepts
  ("switch to this context", "fire at this deadline"), it does not.

**What KickOS chose, and why.** The third option, with the interface held to
concepts. The seam is one header, `arch/include/kickos/arch/arch.h`. The kernel in
`kernel/` calls it and names no ISA, no register and no descriptor format. The
members an ISA must answer are few and map onto the seven questions:

- the context: `arch_context_init` builds a new thread's first frame, `arch_start`
  enters the first thread, and `arch_switch` switches from one context to another;
- privilege and traps: `arch_syscall` is the user-side trap stub;
- the critical section: `arch_irq_save` and `arch_irq_restore`;
- time: `arch_clock_now` reads the clock, and `arch_timer_arm` sets the next deadline;
- protection: `arch_mpu_apply` on a region MPU, the `arch_aspace_*` members on page
  tables.

Many other members exist, for consoles, caches, multicore and chip services, but
most of them have a default under `arch/common/` that a port keeps unless its
hardware needs something else. A port therefore writes what its hardware changes
and nothing more. The seam grows only when a new *capability* appears that no member
can express (address translation is the example), never because a new processor
arrived. *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)* develops that
rule.

A concept-level seam leaves each port free to use its hardware's best mechanism,
as long as the kernel above sees the same contract. The context switch shows this
best. The contract, `arch-switch-may-defer` in
[`../reference/invariants.md`](../reference/invariants.md), says the register swap
may not have happened yet when `arch_switch` returns, and the kernel must not
assume it has. Two strategies meet that contract:

- **Always deferred.** `arch_switch` records the target and pends a
  lowest-priority software interrupt, and that interrupt performs the swap once
  every critical section and every other handler has finished. The Arm M-profile
  ports use PendSV, RXv3 uses SWINT, and RV32 uses a machine software interrupt it
  raises against itself.
- **Immediate in a thread, deferred in a handler.** Called from thread code,
  `arch_switch` swaps right away. Called from an interrupt handler, it only records
  the target, and the handler's exit path returns into the new thread instead of the
  old one. AArch64, RV64, x86_64 and Xtensa work this way.

The kernel's scheduler is the same C on all of them. It asks for a switch, and the
swap happens at a safe point that each family chooses.

The system call follows the same pattern. The contract,
`syscall-in-priv-thread-context`, is that the kernel's dispatch runs privileged, in
the calling thread's own context, so a call such as a semaphore wait can block and
resume like ordinary code. The Arm M-profile, RV32 and RXv3 ports meet it with a
trampoline: the trap handler rewrites its return so that it lands in a short
privileged routine running as the calling thread, which calls the dispatcher.
AArch64, x86_64 and RV64 call the dispatcher straight from the trap entry, on the
calling thread's own kernel stack. Xtensa, with no privilege to cross, calls it as a
function. Above the seam, `syscall_dispatch` is one function, and a call returns the same
result on every target.

**Where the hardware falls short.** Uniform semantics cannot create hardware that
is not there. A Cortex-M0 ignores the privilege bit; an ESP32 gives KickOS no
protection unit it drives. KickOS does not hide this, and it does not lower every
target to the weakest one. The missing feature is stated once, per chip, in the chip
description (the nRF51's says `privilege: false`), the build derives what follows
from it (no privilege ring, so no fault isolation), and the tests check that each
board delivers what it claims. A declared ceiling is part of the semantics; a silent
difference is the bug.

Protection gets the same treatment. A region MPU and page tables are different
mechanisms, but the kernel uses one address-space abstraction over both (Part 7);
the Journey's [MMU chapter](../journey/10-the-mmu.md) puts one kernel call side by
side on the two.

**How it is done in the code.** Each ISA variant has one directory:
`arch/arm/armv6m`, `arch/arm/armv7m` (which also serves ARMv8-M Mainline; only the
MPU backend changes), `arch/arm64/armv8a`, `arch/riscv/rv32imac`,
`arch/riscv/rv64imac`, `arch/x86/x86_64`, `arch/rx/rxv3` and `arch/xtensa/lx6`. In
each, `switch.S` holds the context switch and, wherever there is a privilege level
to cross, the syscall stub `arch_syscall` and its kernel entry;
its `context.h` (on armv7m, `arch/arm/armv7m/include/kickos/arch/context.h`) defines the `struct arch_context` the kernel never
looks inside. Chips sit below, in `arch/<family>/chip/`. The host simulator,
`arch/sim`, implements the same seam, so the semantics can be tested with no board.

## Where to go next

**In this part.**

- *[Porting a new ISA: RISC-V](porting-a-new-isa-riscv.md)*, the worked example,
  takes one port through from the seam: what a port must decide, the single-frame
  deferred switch, the syscall trampoline, bring-up on an emulator, and PMP's
  refuse-by-default polarity.

**The concepts this part builds on.**

- Chapter 0.3, *[Interrupts and traps](interrupts-and-traps.md)*: vector tables,
  interrupt controllers, masking.
- Chapter 3.5, *[Context switching and the silicon
  contract](context-switching-and-the-silicon-contract.md)*: what a switch must
  preserve, the contract axes, and what a switch costs on each kind of core.
- Chapter 3.9, *[The syscall path](the-syscall-path-trap-dispatch-return.md)*: one
  trap followed through dispatch and back.
- Chapter 7.4, *[Privilege is three axes, not one
  bit](privilege-is-three-axes-not-one-bit.md)*: why "unprivileged" can mean less
  than it says.
- Chapter 7.7, *[Whoever stacks the trap frame owns the bounds
  check](whoever-stacks-the-trap-frame-owns-the-bounds-check.md)*: the stack-pointer
  check, family by family.

**The code, one family at a time (the Journey).** Each chapter replays the reference
walk on the Cortex-M4F ([Power-on](../journey/01-power-on.md), [The first
syscall](../journey/02-first-syscall.md)) and shows what changes:

- AArch64 and page tables: [10, The MMU](../journey/10-the-mmu.md), then
  [11, Multicore](../journey/11-multicore.md);
- ARMv6-M: [12](../journey/12-armv6m.md); ARMv8-M: [13](../journey/13-armv8m.md);
- RISC-V, RV32 and RV64: [14](../journey/14-riscv.md);
- x86_64: [15](../journey/15-x86_64.md);
- RXv3: [16](../journey/16-rx.md);
- Xtensa LX6: [17](../journey/17-xtensa.md).

**The contract (the Reference).**

- [`../reference/porting.md`](../reference/porting.md): the seam, member by member,
  the list of arch backends and chips, and each backend's own section.
- [`../reference/invariants.md`](../reference/invariants.md): the rules every port
  keeps, among them `arch-switch-may-defer`, `deferred-switch-lowest-band`,
  `switch-frame-matches-init`, `trap-return-atomic` and
  `syscall-in-priv-thread-context`.
