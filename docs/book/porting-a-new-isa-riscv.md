<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Porting an RTOS to a New ISA: a RISC-V Worked Example

> *KickOS Book -- a per-ISA teaching chapter: how a small preemptive
> kernel meets a processor it has never seen. RISC-V RV32IMAC (the
> ESP32-C6, plus QEMU's `virt_rv32` machine) is the worked example, but the shape of
> the argument is the same for any ISA.*

**SCOPE, stated because RISC-V is not one port here.** This chapter is about **RV32IMAC in
MACHINE MODE, with PMP as the protection unit**. The project also carries a 64-bit RISC-V
port, and the two are kept separate rather than merged: machine mode against PMP is a
different set of answers from supervisor mode against page tables, and they share the shape
of the argument and almost nothing else. Everything below names the 32-bit port's CSRs
(`mstatus`, `mtvec`, `mcause`, `mepc`) and its PMP entries; read it for the METHOD, and take
the 64-bit port's own answers from the [Reference](../reference/porting.md).

Most OS textbooks teach the *concepts* -- a context switch saves registers, a
system call changes privilege, a timer drives preemption -- on a single idealized
machine. This chapter does the opposite: it fixes the concepts and asks **what a
real, unfamiliar CPU forces you to decide.** The answer is surprisingly small.
KickOS meets every processor through one seam, `arch.h`, that names *concepts*
(switch, critical section, timer, syscall, MPU) and never *mechanisms*. Porting
is the act of implementing that seam -- and discovering the three or four places
where the new ISA genuinely differs.

## 1. The seam, and why it is small

`arch.h` is deliberately ISA-neutral. It says "switch the running context from
`from` to `to`" -- not "pend PendSV," not "write the CLINT msip register." The
litmus test the project holds itself to: *a new ISA must fit this seam with no
signature changes.* RISC-V passed it unchanged, as the other non-ARM families did.

What that test does and does not claim is worth separating, because reading it as "the seam
never changes" is wrong. It claims that ARRIVING ON A NEW PROCESSOR costs no seam member:
switching, masking, timing and trapping are concepts every CPU has, so a port implements
them and adds nothing. It does not claim the seam is closed. The seam grows when a new
CAPABILITY CLASS arrives that no existing member can express -- a different event from a new
ISA, and a much rarer one. Address translation is the case in point: a paging backend has to
be able to say which physical frame a virtual address resolves to, and no signature about
regions, timers, switches or interrupts can answer that, so the question is a member of the
seam in its own right. The distinction to hold onto is that a member is added for a
CAPABILITY, never for a processor. [`../reference/porting.md`](../reference/porting.md) is
the enumeration of what the seam holds and which backends answer each member.

Why does this matter pedagogically? Because it isolates *what is fundamental*
(the concept) from *what is incidental* (the mechanism). When you port, you are
not re-inventing scheduling or synchronization -- those live in ISA-independent C
above the seam. You are answering a fixed questionnaire:

1. How do I **save and restore** a thread's registers, and where?
2. How does a **trap** (interrupt or exception) enter the kernel?
3. How does a thread **defer a switch** so it happens at a safe point?
4. How does a **system call** cross from user to kernel and back?
5. What is a **critical section** (how do I mask interrupts)?
6. Where does the **clock/timer** come from?

RISC-V answers all six with a handful of instructions. The interesting part is
the *design choices* -- because the ISA usually permits several, and only some
compose well with a preemptive kernel.

## 2. The trap: one door, many reasons

RISC-V has a single machine-mode trap vector, `mtvec`. In *direct* mode every
trap -- timer interrupt, software interrupt, an `ecall`, a fault -- enters the same
handler, which reads `mcause` to learn *why*. (There is a *vectored* mode too;
KickOS uses direct because one well-understood entry point is easier to reason
about than a table of them.)

So the very first thing the handler does, before it knows anything, is **save the
full register context** onto the running thread's own stack, then demux:

```
trap_entry:            # arch/riscv/rv32imac/switch.S
    csrrw sp, mscratch, sp   # swap to a trusted per-hart trap stack
    ...                      # from U-mode: bounds-check the interrupted sp,
                             # adopt it when valid, else route to the reporter
    addi sp, sp, -128  # carve a frame on the (validated) thread stack
    sw   ra, ...(sp)   # save every GPR (except x0, sp, gp, tp) ...
    csrr t0, mepc      # ... plus the return PC ...
    csrr t0, mstatus   # ... and the status word
    csrr t0, mcause
    bltz t0, .Lintr    # mcause[31]=1 -> interrupt; else an exception (ecall/fault)
```

Two subtleties worth pausing on. First, **`gp` and `tp` are not saved.** The
global pointer is a link-time constant shared by every thread; the thread pointer
is unused (no thread-local storage). Saving invariants wastes cycles, a small
lesson in knowing your ABI. Second, **the frame lands on the interrupted
thread's own stack, but the prologue does not trust that stack blindly.** A
U-mode thread owns `sp` and can aim it at kernel memory, and the software
prologue runs in M-mode, which bypasses the unlocked PMP entries, so storing
through an unchecked `sp` would be a fully controlled kernel write. `mscratch`
holds a trusted per-hart trap stack: `trap_entry` swaps onto it first, so nothing
has been stored anywhere while it decides, and it adopts the interrupted `sp`
only if that `sp` is aligned, lies inside the running thread's own stack, and has
room *remaining* below it for the frame plus the deepest kernel descent this trap
can reach. An M-mode trap keeps its already-trusted `sp`; a U-mode `sp` that
fails any leg is routed to the fault reporter with nothing written through it.
Chapter 7.7,
[*Whoever stacks the trap frame owns the bounds check*](whoever-stacks-the-trap-frame-owns-the-bounds-check.md),
works through why each leg is needed and how the four backends differ.

## 3. The single-frame deferred switch (the heart of it)

Here is the central design decision, and the one most worth teaching.

A preemptive kernel switches contexts for two very different reasons:

- **Voluntarily**: a thread blocks (waits on a semaphore, sleeps). It is running
  ordinary kernel code and *asks* to be switched out.
- **Involuntarily**: a timer interrupt fires and the scheduler decides a
  higher-priority thread should run. The victim is preempted at an *arbitrary*
  instruction.

A naive port uses two different save formats -- a small one for the voluntary case
(save only callee-saved registers, like a function call) and a big one for the
preemptive case (save everything). Then the *resume* path must know which kind of
frame it is looking at. That "which format?" branch is a notorious source of
subtle bugs (KickOS's Xtensa port needs exactly this two-format split, because
its register windows make a uniform frame impractical -- and it pays for it in
complexity).

RISC-V lets a port avoid it. KickOS adopts the **single-frame, always-deferred**
model (the same one the ARM PendSV and RX SWINT backends use):

> **The physical register swap NEVER happens inline. It always happens in one
> place -- the software-interrupt trap -- which always saves the full frame.**

`arch_switch` does not switch. It records the target and *pends a machine
software interrupt* (the CLINT `msip` register), then returns:

```c
void arch_switch(from, to) {
    g_arch_next = to;
    *g_clint_msip = 1;   // pend -- fires later, at a safe point
}
```

Why is this safe and elegant?

- The scheduler calls `arch_switch` **with interrupts masked** (inside a critical
  section, `mstatus.MIE = 0`). So the pended `msip` cannot fire yet.
- When the critical section releases (`MIE` set back to 1), the pending interrupt
  fires *immediately* -- a trap -- and the switcher (`.Lswitch`) runs. It saves the
  outgoing thread's full frame, loads the incoming thread's frame, and `mret`s.
- A **preemptive** switch works identically: the timer handler calls
  `arch_switch` (pending msip) and returns; on the way out the pending msip fires
  and the *same* switcher runs. One code path, one frame format, for both cases.

The voluntary case now looks exactly like the involuntary one: the thread that
called `arch_switch` is simply *frozen mid-instruction* by the msip trap, its
complete state captured, and resumed -- mid-instruction -- when it is next
scheduled. There is no "voluntary frame" to distinguish. This is why a thread-exit
stress test has no edge cases to trip over on the single-frame model where a
two-format design must: there are no edges, only one path.

## 4. The system call: privilege without losing your continuation

A system call must (a) raise privilege and (b) let a *blocking* syscall (like
`sem_wait`) suspend the calling thread and resume it later, right where it left
off, returning the result to user code as if the call were an ordinary function.

The tempting shortcut -- handle the syscall directly in the trap handler -- breaks
(b). If `sem_wait` blocks while execution is "in the trap handler," the state that
would suspend and resume is the *handler's*, not a clean per-thread continuation. The
kernel's blocking primitives assume they run in **thread context**.

The fix (shared by every KickOS privilege port) is a **trampoline**. On `ecall`
the trap handler does not dispatch; it rewrites the return so that `mret` lands in
a small trampoline running **in machine mode on the calling thread's own stack**:

```
.Lecall:                       # in trap_entry
    ... load the syscall args from the saved frame into a0..a4 ...
    la   t0, svc_trampoline
    csrw mepc, t0              # mret will go to the trampoline ...
    csrr t0, mstatus
    ori  t0, t0, 0x1800        # ... in M-mode (MPP = machine) ...
    ori  t0, t0, 0x80          # ... with interrupts enabled (MPIE)
    csrw mstatus, t0
    mret
```

Now `syscall_dispatch` runs as ordinary privileged thread code. If it blocks, it
calls `arch_switch` -- which pends msip -- and the *ordinary* deferred switch from
section 3 freezes the trampoline's continuation on this thread's stack and resumes it,
inline, when the thread is rescheduled. When dispatch finally returns, the
trampoline writes the result into the saved frame, bumps the saved PC past the
`ecall`, and falls into the shared restore path, `mret`ing back to user code with
the result in `a0`. The thread's own privilege is carried in the saved
`mstatus.MPP`, so a privileged thread's syscall returns to machine mode and an
unprivileged thread's returns to user mode -- no bookkeeping field required.

The lesson: **the syscall path and the block/switch path are not independent.**
The trampoline exists precisely so that the *same* deferred-switch machinery
serves a blocking syscall. Design them together.

## 5. The methodology: bring it up on an emulator first

You cannot single-step trust into a context switch. The productive order is:

1. **Emulator before silicon.** QEMU's `virt` machine is a standard RISC-V
   platform with a documented CLINT (timer + software interrupt) and RISC-V
   *semihosting* (the host lends you a console and an exit code). Bring the
   arch up there first -- it de-risks the switch/trap/syscall logic against a
   known-good machine -- *then* layer the real chip (the ESP32-C6, with its own
   UART, watchdogs, and timer) on the de-risked core. One arch, two chips:
   the emulator chip and the silicon chip, the same shape as the ARM port on
   QEMU's `mps2` before real Cortex-M parts.
2. **Semihosting is your first `printf`.** Before a UART driver exists, the magic
   `ebreak` sequence hands a string to the host. The banner printing is your
   proof that reset, the C runtime, and the first thread all work.
3. **A distinct fault exit code.** An unhandled exception routes to a handler that
   exits with a recognizable status. A run that stops with that code instead of
   hanging is unambiguously a fault -- you can then ask the CPU what and where.

## 6. Three RISC-V specifics the seam must absorb

Three ISA-specific facts a textbook's idealized machine hides -- each one the seam
has to absorb before an unfamiliar RISC-V core runs:

- **`Zicsr` is a separate extension.** Modern toolchains split the CSR
  instructions (`csrr`, `csrw`, `mret`) out of the base ISA. `-march=rv32imac`
  will not assemble a single `csrw`; you need `-march=rv32imac_zicsr`. A one-word
  fix, but it stops the build cold until you know it.
- **PMP is fail-*closed*.** This is the big one. On ARM, an unprivileged thread
  with no MPU configured can access everything -- memory protection *subtracts*
  permission. On RISC-V it is the reverse: **once Physical Memory Protection is
  implemented, a user-mode access that matches no PMP entry faults.** So the very
  first instruction fetch of the first unprivileged thread takes an *instruction
  access fault* -- the thread cannot even fetch its own code. The fix is a
  single permissive bootstrap PMP entry (all memory, read/write/execute,
  user-accessible) that grants the same "unprivileged but unrestricted" posture
  ARM gives for free. Real per-task isolation narrows that entry per thread
  (Chapter 7, *Memory protection*); the point here is the *polarity* difference,
  which is easy to forget and produces a baffling first fault.
- **No hardware float, and that is fine.** RV32IMAC has no F/D extension, so the
  compiler uses soft-float and the context switch banks no floating-point
  registers -- simpler than the ARM M4F or RX DPFPU paths. A thread that compiles
  on one KickOS board still runs on another; it is just slower here.

## 7. What you end up with

Count the finished port by files and it runs to a few dozen; count it by *what you had
to understand*, and it is three: the context struct, the C seam, and the switch/trap
assembly. Everything else is bulk: optional-feature defaults, a chip's registers and
linker script, a board's configuration. How much of that a given chip needs is a property
of the chip, not of the port. That distribution is itself the lesson: the part
of a port that needs an ISA manual open is small and bounded.

Everything above the seam -- the
scheduler, semaphores, the syscall table, the tickless timer wheel -- is untouched
ISA-independent C. That ratio is the whole thesis of a portable microkernel: the
ISA-specific surface is small, well-defined, and the same shape on every
processor. Meet the questionnaire in section 1, respect the two couplings (single-frame
switch; syscall-through-the-same-switch), bring it up on an emulator, and a new
architecture is a few days of careful assembly -- not a rewrite.

---

*Source of truth for this chapter's scope, the 32-bit machine-mode port:
`arch/riscv/rv32imac/` (arch), `arch/riscv/chip/virt_rv32/` and `arch/riscv/chip/esp32c6/`
(chips). The 64-bit supervisor-mode port is a separate port with its own answers,
`arch/riscv/rv64imac/` and `arch/riscv/chip/virt_rv64/`, and is not taught here.
`docs/reference/porting.md` is the code-synced reference for both. This chapter explains the
*why*; the code and porting guide are the *what*.*
